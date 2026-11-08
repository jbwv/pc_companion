# Troubleshooting

## Board hangs at `USB.begin()` during bring-up

This is expected, not a bug -- **if** you're testing under
**USB Mode: "Hardware CDC and JTAG"**. That mode has no USB HID
support at the hardware/stack level, so `USB.begin()`/`Keyboard.begin()`
can never complete under it, by design. Use it only as a temporary
diagnostic view (it's the only mode combo that reliably shows Serial
Monitor output during early boot). To actually run the device:

- **USB Mode:** USB-OTG (TinyUSB)
- **Upload Mode:** USB-OTG CDC (TinyUSB)

You won't see Serial Monitor output in this mode (or it'll be limited)
-- that's also expected, not a failure sign. The workflow: switch to
Hardware CDC/UART0 to diagnose with visible logs, make your change,
then switch both settings back to USB-OTG (TinyUSB) to verify it
actually works on real hardware.

## Board is bricked / won't take a new upload

1. Unplug the USB-C cable completely.
2. Hold down the **BOOT** button on the board.
3. While still holding BOOT, plug the USB-C cable back in.
4. Keep holding BOOT for about 2 more seconds after plugging in.
5. Release BOOT.
6. In Arduino IDE, check Tools -> Port -- a COM port should now show
   up (possibly a new number). Select it and Upload right away.

Notes: sometimes the COM port doesn't get picked up -- just start
back at step 1. Sometimes unplugging power and plugging back in is
what it actually needed. A few times, simply re-running the upload a
second time was what made it take.

## `partitions.csv` -- tried and abandoned

An earlier debugging pass tried several custom partition tables to
free up app space, suspecting the partition table was behind a boot
hang. It wasn't -- see the USB Mode issue above, which was the real
cause and is unrelated to partition content (confirmed via live
on-device partition dumps). The final, working firmware uses the
Arduino IDE's named **"ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)"**
Partition Scheme instead of any custom `partitions.csv`, so no custom
partition file ships in this repo.

## WiFi telemetry (System page / Now Playing) not updating

The original design sent CPU/RAM/GPU, Now Playing, and mute state
over the same USB-CDC serial port used for keyboard/mouse HID. On this
board, that path is unreliable: composite USB devices combining CDC
and HID on ESP32-S3 hit an open upstream Arduino-ESP32 bug
(espressif/arduino-esp32#10307) where the two interfaces stall each
other's data. HID keeps working; serial RX to the board silently
drops.

Fix already applied in this firmware: telemetry rides WiFi/HTTP
instead (the board runs a small web server on port 80). If it's still
not updating, check:
- `pc_companion_telemetry.py` is actually running (see
  [SETUP.md](SETUP.md) for the auto-start installer).
- The board and PC are on the same WiFi network.
- The board's IP (shown on the Info page) hasn't changed since the
  script last discovered it -- it re-discovers automatically via the
  board's serial debug output, so restarting the script fixes a stale
  IP.

## Board won't join WiFi / no way to reach the web config page

If the board never joined the WiFi network you expected, or you've
moved it somewhere new, it's most likely sitting in its own setup
hotspot rather than failing silently:

- Look for a WiFi network named **`PC_Companion_Setup`** from your
  phone or laptop. If you see it, connect to it and browse to
  `http://192.168.4.1` -- that's the board's own setup page. Enter
  your real SSID/password (and HA info, if you want) and submit; the
  board saves it and reboots onto your network.
- If you don't see that network and the board also isn't reachable at
  its usual IP, give it a minute after power-on -- it only starts the
  hotspot after a real connection attempt fails.
- No WiFi at all where you are, and the board is already configured
  for a different network? A short press of the physical **PWR**
  button forces it back into `PC_Companion_Setup` mode regardless --
  no network needed to trigger it. (This is a short press while the
  board is already on -- a single click while it's fully off just
  powers it on normally, and holding PWR for 6+ seconds still forces a
  hardware shutdown as always.)
- Once you know the board's IP again (shown on its Info page, or in
  the Serial Monitor's `[NET] IP=...` line), `http://<board-ip>/` also
  has a "Reconfigure WiFi" button and a "Home Assistant connection"
  form for changing either without ever reflashing.

## Reference material

- [jarvis_boot_log_reference.txt](jarvis_boot_log_reference.txt) -- a
  real serial-monitor capture of a normal, successful boot.
- [jarvis_tools_settings.png](jarvis_tools_settings.png) -- a
  screenshot of the known-working Arduino IDE Tools menu settings.
