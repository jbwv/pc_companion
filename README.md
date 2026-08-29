# pc_companion

A USB-connected touchscreen companion for your PC, built on a
Waveshare ESP32-S3-Touch-LCD-3.5B-C. Touch buttons launch apps and
scripts, control media playback, and show a live clock/weather/WiFi
status page — no cloud service, no companion app, just a keyboard
your PC thinks is plugged in.

## What it does

- **Page 1 — Launchers.** Touch buttons send keyboard shortcuts that
  your Windows PC (via AutoHotkey) turns into real actions: open an
  app, run a script, open a folder, whatever you configure.
- **Page 2 — Media controls.** Play/pause, next/previous track,
  volume, mute, close the active window — all real USB media keys,
  works with anything currently playing.
- **Page 3 — Live info.** Clock, date, current weather, and WiFi
  status, auto-refreshing.

## Hardware

See [docs/BOM.md](docs/BOM.md).

## Setup

See [docs/SETUP.md](docs/SETUP.md) for the full walkthrough:
Arduino IDE setup, required libraries, flashing the board, and
configuring the Windows-side scripts.

## Customizing Page 1

Page 1's buttons are config-driven — as long as you keep the current
2×4 grid (8 buttons), you don't need to touch the main firmware file
at all. Open `firmware/pc_companion/page1_buttons.h` and edit the
array: one line per button, each with a label and the key combo it
sends. See the comments at the top of that file for the exact format.

Want more or fewer than 8 buttons? That means changing the grid
itself (`GRID_COLS` / `GRID_ROWS` near the top of
`pc_companion.ino`), not just the config file — a slightly bigger
edit, but still straightforward.

Whatever key combo you choose, wire it up on the Windows side in
`windows/hotkeys.ahk` (see [docs/SETUP.md](docs/SETUP.md)).

## A note on `secrets.h`

`firmware/pc_companion/secrets.h` ships with placeholder WiFi
credentials. Fill in your real SSID and password before flashing —
and if you fork this repo, remember to keep your real credentials
out of anything you commit back.

## License

MIT — see [LICENSE](LICENSE).
