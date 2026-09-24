# pc_companion

A USB-connected touchscreen companion for your PC, built on a
Waveshare ESP32-S3-Touch-LCD-3.5B-C. Touch buttons launch apps and
scripts, control media playback, show live system/weather/WiFi info,
act as a USB trackpad, and (optionally) listen for a "Jarvis" wake
word -- no cloud service, no companion app, just a keyboard and mouse
your PC thinks are plugged in.

## What it does

- **Hotkeys / Hotkeys 2** -- touch buttons send real keyboard shortcuts
  (via USB HID), turned into actions by AutoHotkey: open an app, run a
  script, whatever you configure. Editable from the board's own
  web-based config page -- no reflashing needed.
- **Media** -- play/pause, next/previous, volume, mute, plus a live
  Now Playing panel.
- **Info** -- clock, date, a week-at-a-glance strip, today/tomorrow
  weather, and US timezones (ET/CT/MT/PT).
- **Trackpad** -- the touchscreen itself acts as a USB HID mouse.
- **System** -- live CPU / RAM / GPU usage, microSD status (see
  below), and WiFi/network info.
- **MicroSD** (optional) -- caches weather for instant display on
  boot and automatically backs up your button config as a safety net.
  See [docs/SETUP.md](docs/SETUP.md).
- **Jarvis voice commands** (optional) -- wake-word detection and
  voice-triggered hotkeys, fully on-device via ESP-SR.
- **Home Assistant control** (optional) -- a grid of buttons, each
  calling one HA service on one entity over HA's REST API, editable
  from the same web config page as Hotkeys. Host/port/token can be set
  in `secrets.h` before flashing, or entered later from the board's own
  web page -- no reflashing needed either way. The first HA button ships
  as a worked example (**not a real entity** -- see
  [docs/SETUP.md](docs/SETUP.md#8-home-assistant-control-optional)).
- **Jarvis AI screensaver** -- a full-screen animated reactor-HUD
  overlay (rotating rings, glowing core, breathing standby dot) that
  ramps through three intensity tiers the longer the board sits idle,
  or pops up instantly with the physical BOOT button. Touch anywhere
  to dismiss.
- **No-reflash WiFi/HA setup** -- a fresh or away-from-home board starts
  its own WiFi hotspot with a simple setup page, and a short press of
  the physical PWR button forces that mode on demand with no network
  needed at all. See [docs/SETUP.md](docs/SETUP.md#3-wifi-credentials).

## Hardware

See [docs/BOM.md](docs/BOM.md).

## Setup

New to this project? Start with `START_HERE.txt` in the zip -- unzip,
unblock, run the installer.

See [docs/SETUP.md](docs/SETUP.md) for the full walkthrough: Arduino
IDE setup, flashing the board, and configuring the Windows-side
scripts. For the voice-command feature, see
[docs/JARVIS_SETUP.md](docs/JARVIS_SETUP.md).

## Customizing Hotkeys

**IMPORTANT: THE "TOOLBOX" AND "MITHRIL" HOTKEYS BUTTONS DO NOT WORK
OUT OF THE BOX** -- they're worked examples pointing at two of the
original author's other personal projects that aren't released on
GitHub yet. Hotkeys 2's 8 buttons also ship with no PC-side action
assigned at all. See
[docs/SETUP.md](docs/SETUP.md#5-windows-side-scripts) before assuming a
button is broken.

Hotkeys and Hotkeys 2 are config-driven two ways:
- **Easiest:** visit `http://<board-ip>/` from any browser on the same
  WiFi (IP shown on the Info page) and edit a button's label, key
  combo, and Action (the program/script/command it runs on your PC) --
  no reflashing, changes persist across reboots. The telemetry service
  running on your PC automatically keeps `hotkeys.ahk` in sync with
  whatever Action you set, so this is normally the only place you need
  to touch.
- **By hand:** edit `firmware/pc_companion/page1_buttons.h` (Hotkeys)
  or `page1b_buttons.h` (Hotkeys 2) -- one line per button, label plus
  the key combo it sends. See the comments at the top of each file.
  For the Windows-side action, hand-edit
  [hotkeys.ahk](hotkeys.ahk) directly -- any combo you define there
  yourself is left alone and never overwritten by the web editor's
  auto-sync.

## A note on `secrets.h`

`firmware/pc_companion/secrets.h` ships with placeholder WiFi and
Home Assistant credentials, and every field in it is optional -- you
can leave it exactly as-is and set up WiFi/HA later from the board's
own web pages instead (see [docs/SETUP.md](docs/SETUP.md)). If you do
fill in real values before flashing, keep them out of anything you
commit back if you fork this repo.

## License

MIT -- see [LICENSE](LICENSE).

## A note on how this was built

This is my first hardware project like this, and I leaned heavily on
AI-assisted coding (Claude) for the firmware and scripting along the
way. Every design decision, every test, and every bug hunt was done by
hand, at my own pace, learning as I went.

Consider this a functional starting point rather than a finished
product. It works, end to end, but there's plenty of room to grow.
