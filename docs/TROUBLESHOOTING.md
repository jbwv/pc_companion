# Troubleshooting & Known Quirks

Real issues hit while building this, kept here so you don't have to
rediscover them.

## Firmware / LVGL

- **`lv_conf.h` missing → `fatal error: ../../lv_conf.h`.** Every
  LVGL demo example ships its own `lv_conf.h` next to the `.ino`.
  Starting a fresh sketch doesn't carry it over automatically — copy
  it in manually if you ever start a new sketch from scratch.
- **Duplicate `.ino` files in one sketch folder.** Arduino IDE
  compiles every `.ino` in a folder together as one program. If you
  ever copy in an example sketch alongside your own, you'll get
  "redefinition" errors for every function — only one `.ino` should
  exist per folder.
- **Landscape rotation is broken on this display driver.** Setting
  the display rotation to landscape on the AXS15231B driver produces
  a black screen — this matches an open, unresolved upstream bug,
  not a mistake in this code. This firmware stays in native portrait.
- **Custom fonts render as white boxes.** This board's LVGL build
  only supports **1-bit (bpp=1)** custom fonts, not 4-bit
  anti-aliased. If you regenerate `weather_icons_24.c` or
  `calendar_icon_24.c` with the [LVGL font converter](https://lvgl.io/tools/fontconverter),
  make sure bpp is set to 1.
- **Media keys need a separate class.** `USBHIDKeyboard` does not
  support media key constants (`KEY_MEDIA_PLAY_PAUSE` etc. don't
  exist in that class). Media/consumer control keys require
  `USBHIDConsumerControl` with its own `CONSUMER_CONTROL_*`
  constants — both objects coexist fine in the same sketch.

## Flashing

- **TinyUSB mode breaks the normal auto-reset.** In "USB-OTG
  (TinyUSB)" mode, Arduino IDE's usual auto-reset-into-bootloader
  trick doesn't reliably work — use the manual BOOT-hold sequence in
  [SETUP.md](SETUP.md) instead.
- **Soft-reset after flashing is unreliable in this mode.** A
  successful upload doesn't guarantee the chip restarts cleanly — do
  a full physical unplug/replug rather than trusting the "Hard
  resetting via RTS pin" message.
- **COM port changes when USB Mode changes.** Windows treats each
  USB Mode as a different device identity. This is expected — just
  recheck **Tools → Port** each time.
- **If the COM port doesn't show up at all after the BOOT-hold
  sequence:** try selecting it manually and uploading anyway, or
  just start the BOOT-hold sequence over from step 1.
- **If none of the above works:** try fully unplugging the board
  from power (not just re-running the BOOT sequence) and plugging
  it back in.
- **Sometimes the fix is simply trying again.** A handful of failed
  uploads in a row, then a retry with no other changes, succeeding
  is a real (if annoying) pattern with this board in this USB mode.

## Windows-side (AutoHotkey / shortcuts)

- **Windows' native "Shortcut key" property only works from the
  Desktop or Start Menu.** This is why this project uses AutoHotkey
  instead — a `.lnk` file's hotkey binding silently stops working the
  moment it's moved to any other folder.
- **Hotkey conflicts are silent.** A Ctrl+Alt+letter combo may
  already be claimed by other software (especially on a managed
  work PC), with no warning. If a button doesn't fire, test the
  exact combo from a real keyboard first.
- **Closing all open windows reliably is harder than it looks.** A
  naive "count open windows, then send Alt+F4 that many times"
  approach is unreliable — window focus shifts unpredictably as
  things close, and in the worst case it can land on the desktop and
  trigger the Windows Shutdown dialog. `close_all.ps1` instead: (1)
  quits all File Explorer windows via the Shell.Application COM
  object, (2) sends `CloseMainWindow()` to every process with a
  visible window (a graceful close request, not a force-kill), (3)
  force-stops any lingering `powershell` process by name as a
  catch-all.
