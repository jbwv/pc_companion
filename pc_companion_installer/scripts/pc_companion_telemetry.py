#!/usr/bin/env python3
"""
pc_companion_telemetry.py

Single PC-side companion script feeding three PC Companion pages, plus
keeping hotkeys.ahk in sync with whatever's set in the board's web
config editor:
  - System page:      CPU / GPU / RAM  ->  POST /telemetry
  - Media page:        Now Playing      ->  POST /nowplaying
  - Media page:        mute indicator   ->  POST /mute
  - hotkeys.ahk sync:  GET /config polled every 30s; any button with a
                       non-empty Action gets (re)written into hotkeys.ahk's
                       auto-generated block, and AutoHotkey is reloaded.
and, only if the board's TRACKPAD_USE_HID_MOUSE is set to 0 (the
fallback path -- normally the board sends real USB HID mouse events
directly and this listener never sees anything):
  - Trackpad page:     mouse control    <-  MOVE,<dx>,<dy> / CLICK / RIGHT_CLICK

Telemetry/config are sent/fetched over WiFi/HTTP to the board's own
WebServer (not USB serial) -- ESP32-S3 composite USB devices combining
CDC and HID hit an open upstream Arduino-ESP32 bug
(espressif/arduino-esp32#10307) where the two interfaces stall each
other's PC->board serial data, so anything this script needs to SEND to
the board goes over HTTP instead. Serial is still used for two things
the board SENDS (board->PC direction, unaffected by that bug): the
Trackpad fallback protocol below, and watching the board's own debug
log for its "[NET] IP=..." line so this script knows where to send its
HTTP requests -- that line gets reprinted by the board every 30s in
case the IP ever changes.

Install:
    pip install -r pc_companion_telemetry_requirements.txt
    (minimum to run at all: psutil pyserial pyautogui requests)
    (optional, for Now Playing)  pip install winrt-Windows.Media.Control winrt-Windows.Foundation winrt-Windows.Foundation.Collections
    (optional, GPU fallback)     pip install nvidia-ml-py
"""
import time
import sys
import subprocess
import threading
import queue
import asyncio
import re
from pathlib import Path

try:
    import psutil
    import serial
    import serial.tools.list_ports
    import pyautogui
    import requests
    pyautogui.FAILSAFE = False
    pyautogui.PAUSE = 0
except ImportError:
    print("[ERROR] Missing core libraries. Run: pip install psutil pyserial pyautogui requests")
    sys.exit(1)

BAUD_RATE = 115200
SEND_INTERVAL = 1.0       # seconds between telemetry HTTP posts
CONFIG_POLL_INTERVAL = 30.0  # seconds between GET /config polls (AHK sync)
HTTP_TIMEOUT = 2.0        # seconds -- don't let a slow/unreachable board stall the main loop

# hotkeys.ahk lives next to this script (both land in scripts\ per the
# installer) -- resolved at import time so it works regardless of the
# current working directory the script is launched from (e.g. a
# Startup-folder shortcut).
HOTKEYS_AHK_PATH = Path(__file__).resolve().parent / "hotkeys.ahk"

AHK_BLOCK_START = "; === PC Companion: auto-generated from board web config -- DO NOT EDIT BELOW ==="
AHK_BLOCK_END = "; === PC Companion: end auto-generated section ==="

# Maps the board's GET /config modifier strings (see code_from_mod() in
# the sketch) to AutoHotkey v2's hotkey prefix symbols.
AHK_MOD_SYMBOLS = {"ctrl": "^", "alt": "!", "shift": "+", "win": "#", "none": ""}

serial_lock = threading.Lock()

# Set by serial_reader_thread() the moment it sees the board's own
# "[NET] IP=..." debug line; read by the main loop before every HTTP
# call. Plain string assignment is atomic enough under the GIL for this
# single-writer/single-reader use -- no lock needed.
board_ip = None

# Windows-only flag that stops a console subprocess (like the
# `powershell` calls below) from popping its own visible window.
# CREATE_NO_WINDOW only exists on the subprocess module on Windows --
# getattr() with a 0 fallback keeps this importable (harmlessly
# no-op'd) on any other OS.
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def find_esp32_port():
    ports = serial.tools.list_ports.comports()
    if not ports:
        return None
    keywords = ["ch340", "cp210", "esp32", "usb serial", "cdc", "silicon labs", "wch"]
    for port in ports:
        desc = (port.description or "").lower()
        if any(k in desc for k in keywords):
            return port.device
    return ports[0].device


# ============================================================
# CPU / GPU readers -- match Task Manager's numbers, not raw
# psutil.cpu_percent()/nvidia-smi (those read differently)
# ============================================================
class CpuUtilityReader:
    COUNTER_PATH = r"\Processor Information(_Total)\% Processor Utility"

    def __init__(self):
        self._latest = queue.Queue(maxsize=1)
        self._last_good_value = 0
        self._proc = None
        self._thread = None
        self._start_process()

    def _start_process(self):
        ps_script = (
            f"$ErrorActionPreference = 'SilentlyContinue'; "
            f"Get-Counter -Counter '{self.COUNTER_PATH}' -SampleInterval 1 -Continuous | "
            f"ForEach-Object {{ $_.CounterSamples[0].CookedValue }}"
        )
        self._proc = subprocess.Popen(
            ["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", ps_script],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1,
            creationflags=CREATE_NO_WINDOW,
        )
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def _reader_loop(self):
        for line in self._proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                val = min(100, max(0, int(float(line))))
                if self._latest.full():
                    try:
                        self._latest.get_nowait()
                    except queue.Empty:
                        pass
                self._latest.put_nowait(val)
            except ValueError:
                continue

    def get(self):
        try:
            self._last_good_value = self._latest.get_nowait()
        except queue.Empty:
            pass
        return self._last_good_value

    def close(self):
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()


class GpuUtilityReader:
    def __init__(self):
        self._latest = queue.Queue(maxsize=1)
        self._last_good_value = 0
        self._proc = None
        self._thread = None
        self._nvml_ok = False
        self._pynvml = None
        self._handle = None
        self._start_perfmon_process()
        self._init_nvml_fallback()

    def _start_perfmon_process(self):
        ps_script = (
            "$ErrorActionPreference = 'SilentlyContinue'; "
            "while ($true) { "
            "  $samples = (Get-Counter -Counter '\\GPU Engine(*)\\Utilization Percentage' "
            "    -ErrorAction SilentlyContinue).CounterSamples; "
            "  if ($samples) { "
            "    $sum = ($samples | Where-Object { $_.InstanceName -like '*engtype_3D*' } "
            "      | Measure-Object -Property CookedValue -Sum).Sum; "
            "    if ($sum -eq $null) { $sum = 0 }; "
            "    Write-Output $sum "
            "  } else { Write-Output 0 } "
            "  Start-Sleep -Milliseconds 1000 "
            "}"
        )
        try:
            self._proc = subprocess.Popen(
                ["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", ps_script],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1,
                creationflags=CREATE_NO_WINDOW,
            )
            self._thread = threading.Thread(target=self._reader_loop, daemon=True)
            self._thread.start()
        except Exception as e:
            print(f"[TELEMETRY] GPU Engine counter unavailable: {e}")

    def _reader_loop(self):
        for line in self._proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                val = min(100, max(0, int(float(line))))
                if self._latest.full():
                    try:
                        self._latest.get_nowait()
                    except queue.Empty:
                        pass
                self._latest.put_nowait(val)
            except ValueError:
                continue

    def _init_nvml_fallback(self):
        try:
            import pynvml
            pynvml.nvmlInit()
            count = pynvml.nvmlDeviceGetCount()
            for i in range(count):
                h = pynvml.nvmlDeviceGetHandleByIndex(i)
                name = pynvml.nvmlDeviceGetName(h)
                if isinstance(name, bytes):
                    name = name.decode("utf-8", errors="ignore")
                print(f"[TELEMETRY] NVML fallback ready -> GPU #{i}: {name}")
                self._handle = h
                self._pynvml = pynvml
                self._nvml_ok = True
                break
        except Exception as e:
            print(f"[TELEMETRY] NVML fallback not available: {e}")

    def get(self):
        try:
            self._last_good_value = self._latest.get_nowait()
            return self._last_good_value
        except queue.Empty:
            pass
        if self._nvml_ok:
            try:
                rates = self._pynvml.nvmlDeviceGetUtilizationRates(self._handle)
                return int(rates.gpu)
            except Exception:
                pass
        return self._last_good_value

    def close(self):
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()


# ============================================================
# System speaker mute state (the Media page's mute/unmute button
# already sends the toggle blind via ConsumerControl -- this just
# reports current state so the board can show it in red)
# ============================================================
class MuteReader:
    def __init__(self):
        self._ok = False
        self._warned = False
        try:
            from ctypes import cast, POINTER
            from comtypes import CLSCTX_ALL
            from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume
            self._cast = cast
            self._POINTER = POINTER
            self._CLSCTX_ALL = CLSCTX_ALL
            self._AudioUtilities = AudioUtilities
            self._IAudioEndpointVolume = IAudioEndpointVolume
            self._ok = True
            print("[TELEMETRY] Mute state: pycaw import OK")
        except Exception as e:
            print(f"[TELEMETRY] Mute state unavailable (pip install pycaw comtypes): {e}")

    def get_muted(self):
        if not self._ok:
            return None
        try:
            devices = self._AudioUtilities.GetSpeakers()
            interface = devices.Activate(self._IAudioEndpointVolume._iid_, self._CLSCTX_ALL, None)
            volume = self._cast(interface, self._POINTER(self._IAudioEndpointVolume))
            return bool(volume.GetMute())
        except Exception as e:
            if not self._warned:
                print(f"[TELEMETRY] Mute state: call failed: {e}")
                self._warned = True
            return None


# ============================================================
# Now Playing (Windows Global System Media Transport Controls).
# Optional -- degrades to "unavailable" cleanly if winrt isn't
# installed, rather than crashing the whole script.
# ============================================================
class NowPlayingReader:
    def __init__(self):
        self._ok = False
        self._warned_empty = False
        try:
            from winrt.windows.media.control import (
                GlobalSystemMediaTransportControlsSessionManager as MediaManager,
            )
            self._MediaManager = MediaManager
            self._ok = True
            print("[TELEMETRY] Now Playing: winrt import OK")
        except Exception as e:
            # A plain `pip install winrt` installs the OLD, deprecated
            # package and imports under a DIFFERENT path -- it will not
            # satisfy this import even though "winrt" shows up installed.
            # You need the split packages specifically:
            #   pip install winrt-Windows.Media.Control winrt-Windows.Foundation winrt-Windows.Foundation.Collections
            print(f"[TELEMETRY] Now Playing unavailable -- winrt import failed: {e}")
            print("[TELEMETRY]   Fix: pip uninstall winrt -y   (removes the old/wrong package)")
            print("[TELEMETRY]        pip install winrt-Windows.Media.Control winrt-Windows.Foundation winrt-Windows.Foundation.Collections")

    async def _get_async(self):
        manager = await self._MediaManager.request_async()

        # get_current_session() picks whichever session Windows THINKS is
        # most relevant -- that's often stale (e.g. an app that had focus
        # earlier, even if paused) rather than whatever is actually
        # playing right now in a browser tab. Walking all sessions and
        # preferring one that's actually PLAYING is far more reliable for
        # "I'm watching something on YouTube right now" style checks.
        sessions = manager.get_sessions()
        if not self._warned_empty:
            print(f"[TELEMETRY] Now Playing: GSMTC sees {sessions.size} session(s)")
        chosen = None
        for i in range(sessions.size):
            s = sessions.get_at(i)
            try:
                playback = s.get_playback_info()
                # 4 == Playing, per GlobalSystemMediaTransportControlsSessionPlaybackStatus
                if playback is not None and int(playback.playback_status) == 4:
                    chosen = s
                    break
            except Exception:
                continue

        if chosen is None:
            chosen = manager.get_current_session()

        if chosen is None:
            if not self._warned_empty and sessions.size == 0:
                print("[TELEMETRY] Now Playing: no sessions -- make sure something is actually "
                      "playing (not just paused/loaded), and that it's an app/tab Windows shows "
                      "in the volume flyout's per-app mixer, which is the same registry GSMTC reads.")
                self._warned_empty = True
            return ("", "")

        info = await chosen.try_get_media_properties_async()
        return (info.title or "", info.artist or "")

    def get(self):
        if not self._ok:
            return ("", "")
        try:
            return asyncio.run(self._get_async())
        except Exception as e:
            if not self._warned_empty:
                print(f"[TELEMETRY] Now Playing: GSMTC call failed: {e}")
                self._warned_empty = True
            return ("", "")


# ============================================================
# hotkeys.ahk auto-sync -- polls the board's GET /config every
# CONFIG_POLL_INTERVAL seconds and keeps hotkeys.ahk's auto-generated
# block matching whatever Actions are set in the web editor.
# ============================================================
def _ahk_combo(mod1, mod2, key):
    """Build an AutoHotkey v2 hotkey combo string, e.g. '^!k', from the
    board's GET /config modifier words + raw key char."""
    return f"{AHK_MOD_SYMBOLS.get(mod1, '')}{AHK_MOD_SYMBOLS.get(mod2, '')}{key}"


_HAND_DEFINED_COMBO_RE = re.compile(r"^\s*([\^\!\+#]*[A-Za-z0-9]+)\s*::")


def _combos_defined_above_block(before_block_text):
    """Every hotkey combo the user hand-defined above the auto-generated
    block, so the sync never generates a duplicate definition (AutoHotkey
    errors on/ignores a repeated hotkey)."""
    combos = set()
    for line in before_block_text.splitlines():
        stripped = line.strip()
        if stripped.startswith(";"):
            continue
        m = _HAND_DEFINED_COMBO_RE.match(line)
        if m:
            combos.add(m.group(1))
    return combos


def sync_hotkeys_ahk(buttons):
    """buttons: the 'buttons' list from GET /config's JSON. Rewrites
    hotkeys.ahk's auto-generated block to match, then reloads
    AutoHotkey. Returns True if the file was actually changed."""
    if not HOTKEYS_AHK_PATH.exists():
        print(f"[AHK-SYNC] {HOTKEYS_AHK_PATH} not found -- skipping sync.")
        return False

    content = HOTKEYS_AHK_PATH.read_text(encoding="utf-8")
    if AHK_BLOCK_START not in content or AHK_BLOCK_END not in content:
        print("[AHK-SYNC] hotkeys.ahk is missing the auto-generated markers "
              f"({AHK_BLOCK_START!r} / {AHK_BLOCK_END!r}) -- skipping sync so "
              "nothing gets clobbered. Re-copy the shipped hotkeys.ahk to get "
              "the markers back if you removed them by accident.")
        return False

    before, rest = content.split(AHK_BLOCK_START, 1)
    _old_middle, after = rest.split(AHK_BLOCK_END, 1)

    hand_defined = _combos_defined_above_block(before)

    lines = [AHK_BLOCK_START]
    seen_combos = set()
    for b in buttons:
        action = (b.get("action") or "").strip()
        if not action:
            continue
        combo = _ahk_combo(b.get("mod1", "none"), b.get("mod2", "none"), b.get("key", ""))
        if not combo:
            continue
        if combo in hand_defined:
            print(f"[AHK-SYNC] Skipping {combo} (page {b.get('page')} button "
                  f"{int(b.get('idx', 0)) + 1}) -- already hand-defined above the "
                  "auto-generated block.")
            continue
        if combo in seen_combos:
            # Two board buttons landed on the same combo (e.g. edited via
            # the web editor without realizing) -- keep the first, warn.
            print(f"[AHK-SYNC] Skipping duplicate combo {combo} (page {b.get('page')} "
                  f"button {int(b.get('idx', 0)) + 1}) -- already used by an earlier button.")
            continue
        seen_combos.add(combo)
        lines.append(f'{combo}::Run "{action}"  ; from board web config '
                      f"(page {b.get('page')} button {int(b.get('idx', 0)) + 1})")
    lines.append(AHK_BLOCK_END)
    new_middle = "\n".join(lines)

    new_content = before + new_middle + after
    if new_content == content:
        return False  # nothing actually changed -- don't touch the file or reload AHK

    HOTKEYS_AHK_PATH.write_text(new_content, encoding="utf-8")
    print(f"[AHK-SYNC] hotkeys.ahk updated ({len(seen_combos)} auto-generated combo(s)).")
    _reload_ahk()
    return True


def _reload_ahk():
    """Restart whichever AutoHotkey process is running this hotkeys.ahk,
    so the new bindings take effect immediately. Falls back to just
    launching it if it wasn't already running."""
    ahk_path_str = str(HOTKEYS_AHK_PATH)
    killed_any = False
    try:
        for proc in psutil.process_iter(["pid", "name", "cmdline"]):
            name = (proc.info.get("name") or "").lower()
            if "autohotkey" not in name:
                continue
            cmdline = proc.info.get("cmdline") or []
            if any(ahk_path_str.lower() in (arg or "").lower() for arg in cmdline):
                proc.terminate()
                killed_any = True
        if killed_any:
            time.sleep(0.3)
    except Exception as e:
        print(f"[AHK-SYNC] Couldn't check/stop the running AutoHotkey process: {e}")

    try:
        import os
        os.startfile(ahk_path_str)  # uses the .ahk file association, same as double-clicking it
        print(f"[AHK-SYNC] AutoHotkey {'reloaded' if killed_any else 'started'}.")
    except Exception as e:
        print(f"[AHK-SYNC] Couldn't (re)start AutoHotkey automatically: {e}. "
              f"Double-click {HOTKEYS_AHK_PATH} to load the new bindings yourself.")


def maybe_sync_hotkeys_ahk():
    if not board_ip:
        return
    try:
        resp = requests.get(f"http://{board_ip}/config", timeout=HTTP_TIMEOUT)
        resp.raise_for_status()
        buttons = resp.json().get("buttons", [])
    except Exception as e:
        print(f"[AHK-SYNC] Couldn't reach GET /config at {board_ip}: {e}")
        return
    try:
        sync_hotkeys_ahk(buttons)
    except Exception as e:
        print(f"[AHK-SYNC] Failed to update hotkeys.ahk: {e}")


# ============================================================
# Serial listener thread -- watches for the board's "[NET] IP=..."
# debug line (so the main loop knows where to send HTTP requests) and
# handles the Trackpad fallback protocol (MOVE/CLICK/RIGHT_CLICK, only
# sent if the board's TRACKPAD_USE_HID_MOUSE is set to 0). Everything
# else the board prints is shown as-is for visibility.
# ============================================================
_NET_IP_RE = re.compile(r"\[NET\]\s*IP=([0-9.]+)")


def serial_reader_thread(ser, stop_event):
    global board_ip
    buffer = ""
    while not stop_event.is_set():
        try:
            data = None
            with serial_lock:
                if ser.in_waiting > 0:
                    data = ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
            if data:
                buffer += data
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue

                    m = _NET_IP_RE.search(line)
                    if m:
                        new_ip = m.group(1)
                        if new_ip != board_ip:
                            print(f"[TELEMETRY] Board IP: {new_ip}")
                            board_ip = new_ip

                    parts = line.split(",")
                    cmd = parts[0]
                    if cmd == "MOVE" and len(parts) == 3:
                        try:
                            pyautogui.moveRel(int(parts[1]), int(parts[2]))
                        except ValueError:
                            pass
                    elif cmd == "CLICK":
                        pyautogui.click()
                    elif cmd == "RIGHT_CLICK":
                        pyautogui.rightClick()
                    else:
                        # Anything else from the board (debug prints,
                        # boot messages, the [NET] IP= line, etc.) -- just
                        # show it so both directions are visible here.
                        print(f"[BOARD] {line}")
            else:
                time.sleep(0.005)
        except Exception:
            time.sleep(0.01)


def _post(path, data):
    if not board_ip:
        return False
    try:
        requests.post(f"http://{board_ip}{path}", data=data, timeout=HTTP_TIMEOUT)
        return True
    except Exception as e:
        print(f"[TELEMETRY] POST {path} to {board_ip} failed: {e}")
        return False


def main():
    port = find_esp32_port()
    if not port:
        print("[ERROR] No serial port detected. Plug in your PC Companion.")
        sys.exit(1)

    print(f"[TELEMETRY] Connecting to {port} @ {BAUD_RATE} baud...")
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = BAUD_RATE
        ser.timeout = 0.1
        # DTR/RTS true: matches how a real terminal (Arduino Serial
        # Monitor, etc.) opens this board's native TinyUSB CDC port.
        # Only affects the board->PC direction this script actually
        # relies on now (Trackpad fallback + the [NET] IP= line) --
        # telemetry/config themselves go over HTTP, not serial (see the
        # module docstring for why).
        ser.dtr = True
        ser.rts = True
        ser.open()
    except Exception as e:
        print(f"[ERROR] Could not open serial port {port}: {e}")
        sys.exit(1)

    print("[TELEMETRY] Waiting 2 seconds for board boot...")
    time.sleep(2)

    stop_event = threading.Event()
    reader_thread = threading.Thread(target=serial_reader_thread, args=(ser, stop_event), daemon=True)
    reader_thread.start()

    print("[TELEMETRY] Starting CPU/GPU counters...")
    cpu_reader = CpuUtilityReader()
    gpu_reader = GpuUtilityReader()
    mute_reader = MuteReader()
    now_playing_reader = NowPlayingReader()
    time.sleep(1.5)

    print("[TELEMETRY] Waiting for the board's [NET] IP=... line before sending "
          "anything over HTTP (up to 30s if it just booted)...")

    print("[TELEMETRY] Streaming System / Now Playing / Mute state, and syncing "
          "hotkeys.ahk from the board's web config. Ctrl+C to stop.\n")

    last_track = None
    last_artist = None
    last_muted = None
    packet_count = 0
    last_config_poll = 0.0

    try:
        while True:
            loop_start = time.time()

            cpu = cpu_reader.get()
            ram = int(psutil.virtual_memory().percent)
            gpu = gpu_reader.get()
            _post("/telemetry", {"cpu": cpu, "gpu": gpu, "ram": ram})

            muted = mute_reader.get_muted()
            if muted is not None and muted != last_muted:
                if _post("/mute", {"muted": 1 if muted else 0}):
                    last_muted = muted

            track, artist = now_playing_reader.get()
            if track != last_track or artist != last_artist:
                if _post("/nowplaying", {"track": track, "artist": artist}):
                    last_track, last_artist = track, artist

            if loop_start - last_config_poll >= CONFIG_POLL_INTERVAL:
                last_config_poll = loop_start
                maybe_sync_hotkeys_ahk()

            packet_count += 1
            print(f"[#{packet_count}] CPU {cpu:3d}% | GPU {gpu:3d}% | RAM {ram:3d}% | "
                  f"Mute {muted} | Now Playing: {track!r} - {artist!r} | "
                  f"Board {board_ip or '(not seen yet)'}")

            elapsed = time.time() - loop_start
            time.sleep(max(0.0, SEND_INTERVAL - elapsed))

    except KeyboardInterrupt:
        print("\n[TELEMETRY] Stopping...")
    except Exception as e:
        print(f"\n[ERROR] {e}")
    finally:
        stop_event.set()
        cpu_reader.close()
        gpu_reader.close()
        if "ser" in locals() and ser.is_open:
            ser.close()
            print("[TELEMETRY] Port closed cleanly.")


if __name__ == "__main__":
    main()
