# Setup Guide

## 1. Arduino IDE

1. Install [Arduino IDE](https://www.arduino.cc/en/software) (2.x).
2. **File → Preferences → Additional boards manager URLs**, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Tools → Board → Boards Manager**, search `esp32`, install
   **"esp32" by Espressif Systems**. (Not "Arduino ESP32 Boards" by
   Arduino — that's a different, unofficial package.)

## 2. Libraries

**Sketch → Include Library → Manage Libraries**, install:

- **lvgl** — pin to **v9.2.2** specifically. Newer versions (9.5.0+)
  are not compatible with this firmware's LVGL API usage.
- **GFX Library for Arduino** (search this exact name)
- **XPowersLib**
- **SensorLib**
- **ESP32-audioI2S-master** (search "audioI2S" if the full name doesn't turn up)
- **TCA9554**
- **OneButton**
- **ArduinoJson** by Benoit Blanchon — pin to **v7.4.3**

Two more libraries are **not** in the online Library Manager — they
come from Waveshare's official demo package:

- `esp_lcd_touch_axs15231b` (touch driver)
- `es8311` (audio codec driver)

Download: [ESP32-S3-Touch-LCD-3.5B-Demo.zip](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.5B/ESP32-S3-Touch-LCD-3.5B-Demo.zip)
— copy both folders into `Documents\Arduino\libraries\`, then restart
Arduino IDE.

## 3. Sketch setup

1. Open `firmware/pc_companion/pc_companion.ino` in Arduino IDE.
2. Confirm `page1_buttons.h`, `secrets.h`, `weather_icons_24.c`,
   `calendar_icon_24.c`, and `lv_conf.h` are all sitting in the same
   folder as the `.ino` — Arduino IDE should show them as separate
   tabs automatically if they are.
3. Edit `secrets.h` with your real WiFi SSID and password.
4. Optionally edit `page1_buttons.h` to set your own Page 1 buttons
   (see the README for the format).

## 4. Board settings (Tools menu)

- **Board:** ESP32S3 Dev Module
- **USB CDC On Boot:** Enabled
- **PSRAM:** OPI PSRAM
- **Flash Size:** 16MB
- **Partition Scheme:** a 16MB-appropriate scheme with a large app
  partition, e.g. "16M Flash (3MB APP/9.9MB FATFS)"
- **USB Mode:** USB-OTG (TinyUSB) — required for the board to act as
  a USB keyboard
- **Upload Mode:** USB-OTG CDC (TinyUSB)

## 5. Flashing

1. Plug the board in via USB-C.
2. Select the correct COM port under **Tools → Port**.
3. Click **Upload**.
4. After a successful flash, fully **unplug and replug** the board
   rather than relying on the automatic reset — soft-resets are
   unreliable in USB-OTG mode.

**If the upload fails** ("No serial data received" / "Could not
open COM_"):
1. Unplug the USB-C cable
2. Hold the **BOOT** button on the board
3. While still holding BOOT, plug the cable back in
4. Keep holding BOOT for ~2 seconds after plugging in, then release
5. Check **Tools → Port** for the (possibly new) COM number, select
   it, and click Upload again

If nothing shows up at all, try a different USB-C cable first —
many cheap cables are charge-only and carry no data.

## 6. Windows-side setup (AutoHotkey)

Page 1's launcher buttons work by sending keyboard shortcuts
(Ctrl+Alt+letter) that [AutoHotkey](https://www.autohotkey.com)
turns into real actions on your PC.

1. Install **AutoHotkey v2**.
2. Create `C:\pc_companion\` and `C:\pc_companion\scripts\` on your
   PC — this is the real, live location; it's unrelated to any
   folder name in this repo.
3. Copy this repo's `windows\hotkeys.ahk` and everything in
   `windows\scripts\` into those two folders.
4. Edit `hotkeys.ahk` and the scripts in `scripts\` to point at
   real paths on your machine (see the comments in each file).
5. Double-click `hotkeys.ahk` to run it — it sits quietly in your
   system tray (a green "H" icon).
6. Test each Ctrl+Alt+combo from your keyboard before testing from
   the board.

**Optional — auto-start on login:** put a shortcut to `hotkeys.ahk`
in your Startup folder (open `shell:startup` in File Explorer to
find it).

**Buttons that don't need an AHK entry:** any button bound to a
native Windows shortcut (Snipping Tool, File Explorer, Task
Manager, Lock Computer) works with no AutoHotkey configuration at
all — those fire directly, regardless of whether AHK is running.

For deeper technical notes and known quirks, see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).
