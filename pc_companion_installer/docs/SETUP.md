# Setup

## 1. Arduino IDE

1. Install [Arduino IDE](https://www.arduino.cc/en/software) (2.x).
2. Add the ESP32 board package (3.0.2+) via Boards Manager.
3. Install the libraries this sketch uses: `lvgl` (v9.2.2),
   `Arduino_GFX_Library`, `ArduinoJson`, plus the Waveshare-provided
   board support files (`TCA9554`, `esp_lcd_touch_axs15231b`,
   `es8311`) -- these normally come bundled with Waveshare's demo
   package for this board.
4. Open `firmware/pc_companion/pc_companion.ino`.
5. Copy `firmware/pc_companion/lv_conf.h` to wherever your Arduino
   library search path expects it (next to the `lvgl` library folder,
   or set `LV_CONF_INCLUDE_SIMPLE` and add its path) -- it's included
   in this repo pre-configured for this project.

## 2. Board settings (Tools menu)

- Board: ESP32S3 Dev Module
- USB CDC On Boot: Enabled
- CPU Frequency: 240MHz (WiFi)
- USB DFU On Boot: Disabled
- Flash Mode: QIO 80MHz
- Flash Size: 16MB (128Mb)
- USB Firmware MSC On Boot: Disabled
- Partition Scheme: "ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)"
- PSRAM: OPI PSRAM
- USB Mode: USB-OTG (TinyUSB)
- Upload Mode: USB-OTG CDC (TinyUSB)
- Upload Speed: 921600

See [jarvis_tools_settings.png](jarvis_tools_settings.png) for a
screenshot of these exact settings, and
[TROUBLESHOOTING.md](TROUBLESHOOTING.md) if the board hangs on boot.

## 3. WiFi credentials

Two ways to get the board onto your WiFi -- pick whichever's easier:

- **Before flashing (optional):** edit `firmware/pc_companion/secrets.h`
  and replace the placeholder values with your real SSID and password:

  ```cpp
  #define WIFI_SSID "PUT_SSID_HERE"
  #define WIFI_PASSWORD "PUT+PSK_HERE"
  ```

- **After flashing, no editing needed:** leave `secrets.h` as-is (or
  even delete those two lines -- they're optional). On first boot, or
  any time the board doesn't recognize the WiFi it's on, it starts its
  own hotspot named **`PC_Companion_Setup`**. Connect to it from a
  phone or laptop, browse to `http://192.168.4.1`, and fill in your
  real network's SSID/password there instead -- the board saves it and
  reboots onto your network. This is also the only way to get WiFi
  configured on a board that was flashed with the placeholder values
  left in.

Once the board is on WiFi and you know its IP (shown on its Info
page), two more things become available from `http://<board-ip>/` in
any browser on the same network, no reflashing ever:
- A **"Reconfigure WiFi"** button, if you ever need to move the board
  to a different network -- it puts the board back into the
  `PC_Companion_Setup` hotspot above.
- If the board has no WiFi to reach that page with at all (e.g. you've
  taken it somewhere away from its configured network), a short press
  of the **PWR** button on the board does the same thing -- forces it
  back into setup mode with no network required. (A single click when
  the board is fully off still just powers it on as normal; holding
  PWR for 6+ seconds still forces a hardware shutdown as normal -- this
  is a short press while the board is already running.)

## 4. Flash the board

Plug the board in over USB-C, select its port in Tools -> Port, and
Upload. If it doesn't take, see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## 5. Windows-side scripts

All Windows-side files live in `scripts\` under `C:\pc_companion\`
(that's where `install_pc_companion.bat` puts them). The firmware's
Hotkeys pages send key combos (e.g. Ctrl+Alt+K); AutoHotkey turns
those into real actions on your PC.

1. Install [AutoHotkey](https://www.autohotkey.com/).
2. That's it -- `install_pc_companion.bat` already unblocked
   `hotkeys.ahk` and created a Startup-folder shortcut for it, so it
   loads automatically every login. Run it once now if you don't want
   to wait for your next login: double-click
   `C:\pc_companion\scripts\hotkeys.ahk`.
3. Edit `kairos.bat`, `telos.bat`, etc. to launch whatever apps/scripts
   you actually want.

**IMPORTANT: THE "TOOLBOX" AND "MITHRIL" BUTTONS ON THE HOTKEYS PAGE DO
NOT WORK OUT OF THE BOX.** They're wired to two of the original
author's other personal projects (an IT admin toolbox app and a TUI
called Mithril) that have not been released on GitHub yet -- pressing
them will try to launch a script/path that doesn't exist on your PC.
This is intentional, not a bug: they're left in as real, working
examples of the button/AutoHotkey format rather than blanked out.
Relabel these two buttons and point them at your own scripts/apps from
the web editor (`http://<board-ip>/`) or by hand in
`firmware/pc_companion/page1_buttons.h` (label/key),
`pc_companion.ino`'s `PAGE1_DEFAULT_ACTIONS` (the default action), and
`hotkeys.ahk` (the `^!i::` / `^!m::` lines).

`hotkeys.ahk` normally doesn't need hand-editing at all: set a
button's Action (the path/script/command to run) from the web config
editor at `http://<board-ip>/` and the telemetry script keeps
`hotkeys.ahk` in sync automatically, inside a clearly-marked
auto-generated block near the bottom of the file. Anything you write
above that block by hand is left alone -- and if you've already
hand-defined a combo AutoHotkey uses elsewhere, the auto-sync skips
generating a duplicate for it rather than risk a conflicting hotkey
definition. This all happens automatically: the telemetry script
polls the board's `/config` endpoint (a machine-readable JSON list of
every button, separate from the `/` editor page a browser sees) every
30 seconds, and reloads AHK itself the moment it writes a real change
-- no tray-icon reload, no restart, nothing to do on your end. Saving
in the web editor is the only step.

**Hotkeys vs. Hotkeys 2 -- one of these needs setup before it does
anything.** The board has two Hotkeys pages, reached by swiping down
from each other: **Hotkeys** (page 1) ships with 6 of its 8 buttons
fully working out of the box (Kairos/Telos/Snip/PowerShell/Notepad/Lock
all have real actions pre-wired into `hotkeys.ahk`) -- the other 2
(Toolbox/Mithril) are the intentionally-non-functional example buttons
described above. **Hotkeys 2** (page 2, one more swipe down) ships as 8
blank `Custom 1`-`Custom 8` placeholders with no action assigned at all
-- pressing one sends its key combo, but nothing on the PC catches it
yet. To make Hotkeys 2 (or Toolbox/Mithril) actually do something: visit
`http://<board-ip>/`, set a label/key/Action for the button you want,
and Save -- the telemetry script picks it up and updates `hotkeys.ahk`
within 30 seconds, no restart needed. If a button on either page
"doesn't work," this is the first thing to check.

## 6. PC telemetry script (System page, Now Playing, mute indicator)

`scripts/pc_companion_telemetry.py` sends CPU/RAM/GPU usage, the
currently playing track, and mute state to the board over WiFi.

This is installed automatically -- `install_pc_companion.bat` calls
`scripts\setup_pc_companion_telemetry.bat` for you as its last step,
which installs the script's Python dependencies and creates a
Startup-folder shortcut so it runs silently (no console window) every
login, no Task Scheduler involved. Nothing further to do here unless
you deleted `C:\pc_companion\` and only re-ran a partial install --
in that case just re-run `scripts\setup_pc_companion_telemetry.bat`
directly.

To run it manually in the foreground instead (useful for checking
it's finding the board -- see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md)):
```powershell
pip install -r pc_companion_telemetry_requirements.txt
python pc_companion_telemetry.py
```

The script also watches its own file on disk and relaunches itself
automatically within a few seconds of being replaced (e.g. by a future
update to this project) -- so a fresh copy takes effect without you
needing to manually restart the Startup shortcut or reboot.

## 7. MicroSD (optional)

Insert a FAT32-formatted microSD card and the board uses it for two
things, both entirely optional -- everything still works with no card
inserted, just without these two extras:

- **Weather/location cache**, so the Info page shows last-known
  weather immediately on boot instead of a blank display while WiFi
  and the weather API catch up.
- **Button config backup**, written automatically every time you hit
  Save All in the web editor. This is a safety net, not a manual
  restore tool: it's only ever read back automatically, and only if
  the board's saved settings are completely empty (a factory reset or
  a freshly erased flash) -- a normal reflash or reboot never touches
  it.

Both live at `/pc_companion/` on the card. Check whether a card is
detected and when each item last saved on the board's System page
(Orange card) -- it also shows free/total space on the card.

## 8. Home Assistant control (optional)

Same two options as WiFi in step 3 -- pick whichever's easier:

- **Before flashing (optional):** edit `firmware/pc_companion/secrets.h`
  with your HA instance's address and a long-lived access token:

  ```cpp
  #define HA_HOST "192.168.1.100"
  #define HA_PORT 8123
  #define HA_TOKEN "PUT_TOKEN_HERE"
  ```

- **After flashing, no editing or reflashing needed:** once the board
  is on real WiFi, visit `http://<board-ip>/` from any browser on the
  same network and use the **"Home Assistant connection"** form there
  -- host/IP, port, and token, applied immediately, no restart. This
  is the easier way to enter a long token especially, since typing one
  into the `PC_Companion_Setup` hotspot's form from a phone keyboard is
  painful. The token field is write-only: it's never shown back to you
  once saved, and leaving it blank on a later save just keeps whatever
  token is already stored.

Generate the token from your HA user profile page (bottom of the
page, "Long-Lived Access Tokens" -> Create Token) -- HA only shows it
once, so copy it right away. Each HA button calls one service on one
entity; assign buttons from the same web config editor used for
Hotkeys (`http://<board-ip>/`). No HA instance configured? The page
just won't do anything when pressed -- nothing else on the board
depends on it.

**IMPORTANT: THE FIRST HA BUTTON ("TV Bedroom") IS A WORKED EXAMPLE,
NOT A REAL ENTITY.** It ships pointed at `switch.bedroom_tv`
(domain `switch`, service `toggle`) purely so you can see what a
filled-in button looks like -- **THIS ENTITY DOES NOT EXIST ON YOUR
HOME ASSISTANT INSTANCE**, and pressing it before editing will just get
a harmless error back from your HA server. Edit it (or any of the other
7 blank `HA 2`-`HA 8` slots) from the web editor to point at your own
real entities.

## 9. The Jarvis AI screensaver

No setup needed -- it's on by default. The board tracks its own touch
inactivity and ramps through three looks the longer it sits idle (6 /
8 / 10 minutes: brighter and faster -> calmer -> a single faint
breathing dot), or press the physical BOOT button on the board to pop
it up instantly regardless of the idle timer. Touch anywhere on the
screen to dismiss it and return to whatever tile was showing.

## 10. Voice commands (optional)

See [JARVIS_SETUP.md](JARVIS_SETUP.md) for the full wake-word setup --
it's a bigger, one-time toolchain install (ESP-IDF), separate from
everyday Arduino IDE use.
