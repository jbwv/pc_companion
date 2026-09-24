@echo off
setlocal EnableExtensions

:: ============================================================
:: PC Companion - Master installer
::
:: Run this from the folder you unzipped (the one that has this
:: .bat, README.md, firmware\, scripts\, docs\ all next to it).
:: Safe to re-run any time -- it just re-creates folders if
:: missing and re-copies files on top of what's already there.
::
:: What it does, step by step:
::   1. Creates the full C:\pc_companion\ folder tree
::   2. Copies the firmware sketch into firmware\pc_companion\
::   3. Copies the Windows-side scripts into scripts\
::   4. Copies all docs (+ archive) into docs\
::   5. Copies README.md / LICENSE to the root
::   6. Silently installs the PC telemetry background service
::      (installs its Python deps + creates a Startup-folder shortcut
::      so it auto-starts, no console window, every login)
::   7. Creates a Startup-folder shortcut for hotkeys.ahk, so
::      AutoHotkey loads it automatically every login too
::
:: Unblocking: run the one-liner in START_HERE.txt from a PowerShell
:: window in this folder before running this .bat -- it clears the
:: "downloaded from the internet" flag on every file here (Windows'
:: "copy" command doesn't carry that flag over, so once files are
:: unblocked here, their copies under C:\pc_companion come out clean
:: too -- no separate unblock step needed on the destination side).
::
:: This installer does NOT touch any other folder on your PC
:: (in particular, it never touches any D:\ Arduino IDE working
:: copy you may already have -- everything below only ever reads
:: from the folder this .bat is run from and writes to
:: C:\pc_companion\).
:: ============================================================

set "BASE=C:\pc_companion"
set "SRC=%~dp0"

echo.
echo === PC Companion Installer ===
echo Installing from: %SRC%
echo Installing to  : %BASE%
echo.

:: ---- 1. Create the folder tree ----
echo Step 1/7: Creating folders...
if not exist "%BASE%" mkdir "%BASE%"
if not exist "%BASE%\firmware" mkdir "%BASE%\firmware"
if not exist "%BASE%\firmware\pc_companion" mkdir "%BASE%\firmware\pc_companion"
if not exist "%BASE%\scripts" mkdir "%BASE%\scripts"
if not exist "%BASE%\docs" mkdir "%BASE%\docs"
echo   done.

:: ---- 2. Firmware ----
echo.
echo Step 2/7: Copying firmware (firmware\pc_companion\)...
copy /y "%SRC%firmware\pc_companion\pc_companion.ino"     "%BASE%\firmware\pc_companion\pc_companion.ino"     >nul
copy /y "%SRC%firmware\pc_companion\secrets.h"            "%BASE%\firmware\pc_companion\secrets.h"            >nul
copy /y "%SRC%firmware\pc_companion\page1_buttons.h"      "%BASE%\firmware\pc_companion\page1_buttons.h"      >nul
copy /y "%SRC%firmware\pc_companion\page1b_buttons.h"     "%BASE%\firmware\pc_companion\page1b_buttons.h"     >nul
copy /y "%SRC%firmware\pc_companion\jarvis_ai_screensaver.h" "%BASE%\firmware\pc_companion\jarvis_ai_screensaver.h" >nul
copy /y "%SRC%firmware\pc_companion\lv_conf.h"            "%BASE%\firmware\pc_companion\lv_conf.h"            >nul
copy /y "%SRC%firmware\pc_companion\calendar_icon_24.c"   "%BASE%\firmware\pc_companion\calendar_icon_24.c"   >nul
copy /y "%SRC%firmware\pc_companion\weather_icons_24.c"   "%BASE%\firmware\pc_companion\weather_icons_24.c"   >nul
echo   done. (secrets.h has placeholder WiFi credentials -- edit it before flashing)

:: ---- 3. Windows-side scripts ----
echo.
echo Step 3/7: Copying scripts (scripts\)...
copy /y "%SRC%scripts\hotkeys.ahk"                              "%BASE%\scripts\hotkeys.ahk"                              >nul
copy /y "%SRC%scripts\kairos.bat"                                "%BASE%\scripts\kairos.bat"                                >nul
copy /y "%SRC%scripts\telos.bat"                                 "%BASE%\scripts\telos.bat"                                 >nul
copy /y "%SRC%scripts\close_all.ps1"                             "%BASE%\scripts\close_all.ps1"                             >nul
copy /y "%SRC%scripts\mithril.bat"                               "%BASE%\scripts\mithril.bat"                               >nul
copy /y "%SRC%scripts\pc_companion_telemetry.py"                 "%BASE%\scripts\pc_companion_telemetry.py"                 >nul
copy /y "%SRC%scripts\pc_companion_telemetry_requirements.txt"   "%BASE%\scripts\pc_companion_telemetry_requirements.txt"   >nul
copy /y "%SRC%scripts\setup_pc_companion_telemetry.bat"          "%BASE%\scripts\setup_pc_companion_telemetry.bat"          >nul
echo   done.

:: ---- 4. Docs ----
echo.
echo Step 4/7: Copying docs (docs\)...
copy /y "%SRC%docs\SETUP.md"                    "%BASE%\docs\SETUP.md"                    >nul
copy /y "%SRC%docs\JARVIS_SETUP.md"             "%BASE%\docs\JARVIS_SETUP.md"             >nul
copy /y "%SRC%docs\TROUBLESHOOTING.md"          "%BASE%\docs\TROUBLESHOOTING.md"          >nul
copy /y "%SRC%docs\FILE_STRUCTURE.md"           "%BASE%\docs\FILE_STRUCTURE.md"           >nul
copy /y "%SRC%docs\BOM.md"                      "%BASE%\docs\BOM.md"                      >nul
copy /y "%SRC%docs\roadmap.md"                  "%BASE%\docs\roadmap.md"                  >nul
copy /y "%SRC%docs\jarvis_boot_log_reference.txt" "%BASE%\docs\jarvis_boot_log_reference.txt" >nul
copy /y "%SRC%docs\jarvis_tools_settings.png"   "%BASE%\docs\jarvis_tools_settings.png"   >nul
echo   done.

:: ---- 5. Root files ----
echo.
echo Step 5/7: Copying README + LICENSE to %BASE%...
copy /y "%SRC%README.md" "%BASE%\README.md" >nul
copy /y "%SRC%LICENSE"   "%BASE%\LICENSE"   >nul
echo   done.

:: ---- 6. Silently install the PC telemetry background service ----
echo.
echo Step 6/7: Installing PC telemetry background service...
call "%BASE%\scripts\setup_pc_companion_telemetry.bat" --silent-auto
if errorlevel 1 (
    echo   [WARNING] Telemetry service install hit an error above -- the
    echo   rest of the install is still fine. You can re-run it any time:
    echo   %BASE%\scripts\setup_pc_companion_telemetry.bat
) else (
    echo   done.
)

:: ---- 7. Create a Startup shortcut for hotkeys.ahk ----
echo.
echo Step 7/7: Creating Startup shortcut for hotkeys.ahk...

set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "AHK_PS1=%TEMP%\pc_companion_make_ahk_shortcut.ps1"
> "%AHK_PS1%" echo $shell = New-Object -ComObject WScript.Shell
>> "%AHK_PS1%" echo $s = $shell.CreateShortcut('%STARTUP%\hotkeys.ahk - Shortcut.lnk')
>> "%AHK_PS1%" echo $s.TargetPath = '%BASE%\scripts\hotkeys.ahk'
>> "%AHK_PS1%" echo $s.WorkingDirectory = '%BASE%'
>> "%AHK_PS1%" echo $s.Description = 'PC Companion hotkeys -- loads AutoHotkey bindings every login'
>> "%AHK_PS1%" echo $s.Save()

powershell -NoProfile -ExecutionPolicy Bypass -File "%AHK_PS1%"
set "AHK_PS1_RESULT=%errorlevel%"
del "%AHK_PS1%" >nul 2>&1

if not "%AHK_PS1_RESULT%"=="0" (
    echo   [WARNING] Failed to create the hotkeys.ahk Startup shortcut.
    echo   You can drop your own shortcut to %BASE%\scripts\hotkeys.ahk
    echo   in %STARTUP% by hand instead.
) else (
    echo   done.
)

echo.
echo === Install complete ===
echo Everything is now in %BASE%
echo.
echo Next steps:
echo   1. (Optional) Edit %BASE%\firmware\pc_companion\secrets.h with your WiFi
echo      SSID/password -- or skip this and set WiFi up after flashing instead,
echo      from the board's own setup hotspot (see docs\SETUP.md section 3).
echo   2. Open %BASE%\firmware\pc_companion\pc_companion.ino in Arduino IDE and flash the board
echo      (see %BASE%\docs\SETUP.md for the full board settings)
echo   3. Install AutoHotkey v2 if you haven't already -- hotkeys.ahk is now set to
echo      load automatically every login (or run it once now to start it right away)
echo.
pause
