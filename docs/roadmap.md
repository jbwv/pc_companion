# pc_companion -- Roadmap / Build List

Personal working list, not required reading to use the project.
Tracks what's next, roughly in order.

## Completed

**Telos (close_all)** -- root causes found and fixed: the script's own
hidden PowerShell process was matching and killing itself (fixed by
excluding `$PID`); Windows 11's tabbed File Explorer broke the old
`Shell.Application.Windows().Quit()` approach (replaced with
`FindWindowEx` + `SendMessage(WM_CLOSE)` targeting `CabinetWClass`
windows directly); Windows Terminal needed its own explicit
`Stop-Process` line since it's a separate process from
`powershell.exe`. Confirmed working across File Explorer, PowerShell,
Windows Terminal, and other apps.

**Jarvis wake word + voice commands** -- fully working, coexists
cleanly with LVGL, WiFi, and USB HID. See
[JARVIS_SETUP.md](JARVIS_SETUP.md).

**2x3 page grid + Now Playing + Trackpad + System page + wireless
button config editor** -- full swipe-navigation expansion plus a
web-based config editor, all in one build:
- Tileview is a 2x3 grid: **Hotkeys <-> Media <-> Info** on top,
  **Hotkeys 2 <-> Trackpad <-> System** on the bottom.
- **Media**: combined play/pause and mute/unmute icon buttons, plus a
  Now Playing panel (track/artist).
- **Hotkeys 2**: same button-grid pattern as Hotkeys, placeholder
  buttons out of the box, editable from the web config page. Touch-only
  for now -- not wired into Jarvis voice commands.
- **Trackpad**: touch-drag feeds `USBHIDMouse` directly.
- **System**: CPU/RAM/GPU as three cards, same visual style as Info.
- **Web-based config editor**: visit `http://<board-ip>/` from any
  browser on the same WiFi to rename Hotkeys/Hotkeys 2 buttons and
  change their modifier keys -- changes apply immediately and persist
  across reboots (NVS flash, `Preferences`, namespace `pcconfig`).

### Telemetry transport pivot: USB serial -> WiFi/HTTP

CPU/RAM/GPU, Now Playing, and mute state originally rode the same
USB-CDC serial port used for keyboard/mouse HID. On hardware, that
path was unreliable -- root cause: **espressif/arduino-esp32#10307**,
an open upstream bug where USB CDC and HID interfaces stall each
other's data on ESP32-S3 composite USB devices. Confirmed via a
from-scratch isolation test (no HID at all: 12/12 test pings received
cleanly; full firmware with HID active: nothing received).

Fix: moved telemetry off USB serial entirely, onto WiFi/HTTP. The
board runs a small `WebServer` on port 80 (`POST /telemetry`,
`POST /nowplaying`, `POST /mute`), which also hosts the button config
editor. `pc_companion_telemetry.py` auto-discovers the board's IP from
its serial debug output -- no IP hardcoding, and it re-adapts if the
board's IP changes. Serial is still used for that discovery plus
general debug logging (board->PC direction was never affected by
#10307 -- only PC->board was).

**Status: confirmed working on hardware.**

**Home Assistant control page** -- a grid of buttons, each calling one
HA service on one entity via HA's REST API (`call_ha_service()`),
same web-based editor pattern as Hotkeys (`build_ha_grid()`,
`ha_button_row_html()`). Config (`HA_HOST`/`HA_PORT`/`HA_TOKEN`) lives
in `secrets.h`; button assignments back up to `ha_backup.json` on
microSD the same way the Hotkeys config does.

**Jarvis AI screensaver** -- a full-screen LVGL overlay recreating the
"reactor HUD" look from an earlier HTML mockup (rotating rings,
glowing core, squashing eyes, corner brackets, a breathing standby
dot), sitting above the tileview. Three intensity tiers driven by
LVGL's own input-inactivity clock: 6 min idle -> WAKE, 8 min -> IDLE,
10 min -> STANDBY (near-black, one faint dot). The physical BOOT
button (GPIO0) force-triggers it on demand regardless of the idle
timer. Touch anywhere dismisses it.
- Root cause of several freezes during bring-up: this build's LVGL
  config caps the internal allocator pool at 64KB
  (`LV_MEM_SIZE`) and hangs the MCU forever on a failed allocation
  (`LV_ASSERT_HANDLER while(1);`). Any style-based `transform_rotation`
  / `transform_scale` -- even on small widgets -- forces LVGL to
  allocate a large non-chunkable "layer" buffer first, which blew
  through that pool. Fixed by rotating/resizing widgets directly
  (`lv_arc_set_rotation()`, plain width/height changes) instead of
  using transform styles, on every widget that needed a non-identity
  transform (rings, core, eyes, and the standby dot).
- **Status: confirmed working end-to-end on hardware** -- full
  WAKE -> IDLE -> STANDBY cascade, BOOT-button force-trigger, and
  touch-to-dismiss all tested.

**WiFi + Home Assistant setup without Arduino IDE** -- WiFi and HA
credentials moved from compile-time `secrets.h` values into NVS flash,
so they can be set or changed without ever reflashing. A never-
configured (or away-from-its-network) board starts its own
`PC_Companion_Setup` WiFi hotspot with a plain setup form; once on real
WiFi, both a "Reconfigure WiFi" button and a "Home Assistant
connection" form are available from the board's normal web page at
`http://<board-ip>/`, applied live with no restart. A short press of
the physical **PWR** button forces setup mode with no network needed
at all -- for when the board is somewhere with no WiFi to load the web
page over in the first place. See [SETUP.md](SETUP.md) section 3 and 8.
**Status: confirmed working end-to-end on hardware, including a real
away-from-home test of the PWR button trigger.**

## Not yet started

### Voice recorder (record to microSD)
Press a button (or "Hey Jarvis, record") to record audio to a
timestamped `.wav` on the onboard microSD card. The board has a
genuine onboard TF card slot, and Waveshare's own demo already proves
SD + audio codec work together on this exact board
(`SD_MMC` + `es8311_codec_init()`, `i2s.recordWAV()`). Simpler than
Jarvis -- no wake-word detection or ESP-IDF detour needed.

### Gesture-controlled ("Iron Man") PC actions
Recognize specific hand poses (fist, open palm, point, a
"repulsor"-style push) via a camera and trigger PC actions from them.
Decided this needs to be a **separate device** from pc_companion --
real hand-pose recognition (vs. simple motion detection) needs
MediaPipe/OpenCV-class compute an ESP32 can't do, so the plan is a
standalone camera node (e.g. a Pi 5 + Camera Module, enough to run
Google's MediaPipe Hand Landmarker on-device -- the current model
bundle is a single ~7.8MB `hand_landmarker.task` file, no separate
"lite" download needed) that recognizes the pose and either drives the
PC directly (Pi as its own USB HID gadget) or hands the event to
pc_companion over WiFi to reuse its existing HID-injection/action
mapping. Not started -- next step is prototyping pose classification
reliability at actual desk distance/lighting before any hardware
purchase.

## Deferred / parked

**Weather outline bug** -- the amber "fetching" border on the Info
page never visibly renders. Root cause identified (missing
`lv_task_handler()` redraw call between showing/hiding the border);
fix added but the visual symptom was never confirmed resolved. Not
worth further effort right now.

## Also considered, ruled out (needs a battery / portable form factor)

Several capable pieces of onboard hardware only make sense if the
device becomes battery-powered and portable -- not a fit for the
current stationary, USB-tethered setup:
- **QMI8658 (6-axis IMU)** -- shake/tilt gestures
- **Camera (OV5640)** -- rear-facing, not usable in the current mount
  (see the gesture-control roadmap item above -- that's going on a
  separate device instead of trying to relocate this one)
- **AXP2101 power chip diagnostics** -- only meaningful with a battery

**BOOT button (GPIO0)**: no longer parked -- now force-triggers the
Jarvis AI screensaver on demand (see Completed, above). Reachable
enough for occasional presses even mounted on a stand.

**PWR button**: no longer parked either -- it's wired through the
TCA9554 I2C I/O expander (not the AXP2101 as first assumed from the
schematic), and now force-triggers WiFi setup mode on a short press
(see Completed, above). The board's own hardware still owns power-on
(single click while off) and forced shutdown (6+ second hold).

One exception: the **PCF85063 RTC chip** could make the clock
resilient through reboots/WiFi outages without needing a battery -- a
small, low-priority reliability improvement, not tied to the
"needs a battery" bucket above.

Also ruled out: on-device password autofill (decided the effort of
migrating off Edge's built-in autofill wasn't worth it), and a
Groq/Whisper cloud voice pipeline (decided on-device ESP-SR fits
better long-term, no ongoing cloud dependency -- Groq remains a
fallback option if ESP-SR ever proves too big a toolchain jump).

## Bluetooth HID instead of USB (later/future)

Switch from USB HID to Bluetooth HID for a wireless connection.
Deliberately deferred until the battery question above is solved,
since going wireless means the board can no longer draw power from
the USB connection it would be replacing.
