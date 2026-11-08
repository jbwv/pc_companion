# File Structure

```
pc_companion_installer.zip (extracted)
├── START_HERE.txt
├── install_pc_companion.bat
├── README.md
├── LICENSE
├── scripts\
│   ├── hotkeys.ahk
│   ├── kairos.bat
│   ├── telos.bat
│   ├── close_all.ps1
│   ├── mithril.bat
│   ├── pc_companion_telemetry.py
│   ├── pc_companion_telemetry_requirements.txt
│   └── setup_pc_companion_telemetry.bat
├── firmware\
│   └── pc_companion\
│       ├── pc_companion.ino
│       ├── secrets.h
│       ├── page1_buttons.h
│       ├── page1b_buttons.h
│       ├── jarvis_ai_screensaver.h
│       ├── weather_icons_24.c
│       ├── calendar_icon_24.c
│       └── lv_conf.h
└── docs\
    ├── SETUP.md
    ├── TROUBLESHOOTING.md
    ├── FILE_STRUCTURE.md   (this file)
    ├── BOM.md
    ├── roadmap.md
    ├── JARVIS_SETUP.md
    ├── jarvis_boot_log_reference.txt
    └── jarvis_tools_settings.png
```

`START_HERE.txt` and `install_pc_companion.bat` are one-time installer
files -- they stay in whatever folder you extracted the zip to and
don't get copied into `C:\pc_companion\`. Everything else in the tree
above lands in `C:\pc_companion\` in the same layout shown.

The Arduino sketch lives at `firmware\pc_companion\pc_companion.ino`
-- nested inside a same-named folder because Arduino IDE requires the
sketch folder name to exactly match the `.ino` filename.

`secrets.h` ships in the repo with placeholder WiFi/HA values (no
`.gitignore` needed for it), and every field in it is optional --
WiFi and HA can also be set up entirely from the board's own web
pages after flashing, with no editing or reflashing needed (see
[SETUP.md](SETUP.md) sections 3 and 8). If you do fill in real values
locally before flashing, don't commit them back.

`partitions.csv` is intentionally not part of this structure -- a
custom partition table was tried during Jarvis bring-up and ruled out
as unnecessary (see [TROUBLESHOOTING.md](TROUBLESHOOTING.md)). The
firmware uses Arduino IDE's named "ESP SR 16M" Partition Scheme
instead.

`docs\archive\` holds earlier working versions of the firmware, kept
for reference -- not needed to build or run the current version.
