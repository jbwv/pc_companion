@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: ============================================================
:: PC Companion - Telemetry background service installer
::
:: Run this ONCE, from the same folder as pc_companion_telemetry.py
:: and pc_companion_telemetry_requirements.txt (e.g. your Downloads
:: folder). It is safe to re-run any time -- it just re-copies files,
:: re-creates folders if missing, and replaces the Startup shortcut
:: if one already exists.
::
:: What it does:
::   1. Makes sure Python itself is installed -- if "python" isn't on
::      PATH at all, this installs it automatically (via winget, or a
::      direct download if winget isn't available) with PATH already
::      configured, no manual checkbox to remember. This is the step
::      that used to be an undocumented prerequisite -- see the long
::      comment above that section for why it's handled this way.
::   2. Creates C:\pc_companion\scripts if it doesn't exist yet
::      (matches the folder that already holds kairos.bat, telos.bat,
::      mithril.bat, close_all.ps1)
::   3. Copies pc_companion_telemetry.py + its requirements file there
::   4. Installs/updates the Python packages it needs
::   5. Creates a shortcut in your Startup folder (via %APPDATA%, so
::      this works the same on any Windows account/PC -- no
::      hardcoded username) that launches the script silently, with
::      NO console window, every time you log in -- using pythonw.exe
::      instead of python.exe
::   6. Starts it immediately so it's running right now too, not just
::      after your next login
::
:: install_pc_companion.bat calls this script automatically (passing
:: --silent-auto, which just skips the "press any key" pauses) -- you
:: normally won't need to run this one by hand. Re-run it directly
:: only if you deleted C:\pc_companion and want just the telemetry
:: service reinstalled without a full re-install.
:: ============================================================

set "BASE=C:\pc_companion"
set "SCRIPTS=%BASE%\scripts"
set "SRC_DIR=%~dp0"
set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
set "SHORTCUT_NAME=PC Companion Telemetry.lnk"

:: --silent-auto is passed when install_pc_companion.bat chains this
:: script automatically -- it skips the "press any key" pauses so it
:: doesn't sit there waiting for input during an unattended install.
set "SILENT="
if /i "%~1"=="--silent-auto" set "SILENT=1"

echo.
echo === PC Companion Telemetry Setup ===
echo.

:: ---- 1. Make sure Python itself is installed ----
:: This step didn't exist for a while, and it was a real gap: everything
:: past this point assumes "python"/"pythonw"/"pip" already work on
:: PATH, but a brand-new Windows install (or a new PC like this one)
:: doesn't have Python at all. Previously this just failed later at the
:: pip-install step with a confusing error, or at the pythonw-not-found
:: check below with a "go install Python yourself" message. Since the
:: whole point of this installer is that it "just works," that's no
:: longer good enough -- so this step installs Python automatically
:: when it's missing, the same way a person would, with PATH already
:: configured correctly (no manually checking "Add python.exe to PATH"
:: during the installer, which is also easy to miss).
where python >nul 2>&1
if errorlevel 1 (
    echo Python not found -- installing it automatically, this can take a minute...
    where winget >nul 2>&1
    if not errorlevel 1 (
        winget install --id Python.Python.3.12 -e --silent --accept-package-agreements --accept-source-agreements
    ) else (
        echo winget isn't available on this PC -- downloading the official Python installer directly instead...
        set "PYEXE=%TEMP%\pc_companion_python_installer.exe"
        powershell -NoProfile -Command "Invoke-WebRequest -Uri 'https://www.python.org/ftp/python/3.12.7/python-3.12.7-amd64.exe' -OutFile '!PYEXE!'"
        if not exist "!PYEXE!" (
            echo.
            echo [ERROR] Couldn't download the Python installer -- check your internet
            echo connection, or install Python yourself from python.org ^(make sure
            echo to check "Add python.exe to PATH"^), then run this script again.
            echo.
            if not defined SILENT pause
            exit /b 1
        )
        :: /quiet + PrependPath=1 is the installer's own equivalent of
        :: checking "Add python.exe to PATH" by hand -- no dialog, no
        :: checkbox to forget.
        "!PYEXE!" /quiet InstallAllUsers=0 PrependPath=1
        del "!PYEXE!" >nul 2>&1
    )

    :: The installer just updated PATH in the registry, but THIS cmd
    :: window already has the old PATH loaded in memory and won't see
    :: that change on its own -- normally that means closing and
    :: reopening the window before "python" works. Pulling the fresh
    :: value back out of the registry here means the rest of this same
    :: run can use it immediately, no reopening required.
    echo Refreshing this window's PATH so Python is usable right away...
    set "SysPath="
    set "UserPath="
    for /f "usebackq tokens=2,*" %%A in (`reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path 2^>nul`) do set "SysPath=%%B"
    for /f "usebackq tokens=2,*" %%A in (`reg query "HKCU\Environment" /v Path 2^>nul`) do set "UserPath=%%B"
    set "PATH=!SysPath!;!UserPath!;!PATH!"

    where python >nul 2>&1
    if errorlevel 1 (
        echo.
        echo [ERROR] Python was installed but still isn't on PATH in this window.
        echo Close this window, open a new Command Prompt ^(so Windows re-reads
        echo PATH from scratch^), and run this script again.
        echo.
        if not defined SILENT pause
        exit /b 1
    )
    echo Python is installed and ready.
) else (
    echo Python found on PATH -- nothing to install.
)

:: ---- 2. Make sure the folder structure exists ----
if not exist "%BASE%" (
    echo Creating %BASE% ...
    mkdir "%BASE%"
)
if not exist "%SCRIPTS%" (
    echo Creating %SCRIPTS% ...
    mkdir "%SCRIPTS%"
)

:: ---- 3. Copy the script + requirements into scripts\ ----
if not exist "%SRC_DIR%pc_companion_telemetry.py" (
    echo [ERROR] pc_companion_telemetry.py was not found next to this .bat file.
    echo Put this .bat in the same folder as pc_companion_telemetry.py and
    echo pc_companion_telemetry_requirements.txt, then run it again.
    echo.
    if not defined SILENT pause
    exit /b 1
)

echo Copying pc_companion_telemetry.py to %SCRIPTS% ...
copy /y "%SRC_DIR%pc_companion_telemetry.py" "%SCRIPTS%\pc_companion_telemetry.py" >nul

if exist "%SRC_DIR%pc_companion_telemetry_requirements.txt" (
    echo Copying requirements file to %SCRIPTS% ...
    copy /y "%SRC_DIR%pc_companion_telemetry_requirements.txt" "%SCRIPTS%\pc_companion_telemetry_requirements.txt" >nul
) else (
    echo [WARNING] Requirements file not found next to this .bat -- skipping pip install.
)

:: ---- 4. Install/update Python dependencies ----
if exist "%SCRIPTS%\pc_companion_telemetry_requirements.txt" (
    echo.
    echo Installing Python dependencies -- this can take a minute...
    python -m pip install --upgrade -r "%SCRIPTS%\pc_companion_telemetry_requirements.txt"
    if errorlevel 1 (
        echo [WARNING] pip reported an error above -- the script may not run
        echo correctly until that's resolved, but setup will continue.
    )
)

:: ---- 5. Find pythonw.exe (the windowless Python interpreter) ----
set "PYTHONW="
for /f "delims=" %%P in ('where pythonw 2^>nul') do (
    if not defined PYTHONW set "PYTHONW=%%P"
)
if not defined PYTHONW (
    echo.
    echo [ERROR] Could not find pythonw.exe on PATH.
    echo Is Python installed with "Add python.exe to PATH" checked?
    echo Aborting -- fix that, then run this .bat again.
    echo.
    if not defined SILENT pause
    exit /b 1
)
echo.
echo Found pythonw.exe: %PYTHONW%

:: ---- 6. Create/replace the Startup shortcut (silent, every login) ----
echo.
echo Creating Startup shortcut...

set "PS1=%TEMP%\pc_companion_make_shortcut.ps1"
> "%PS1%" echo $shell = New-Object -ComObject WScript.Shell
>> "%PS1%" echo $s = $shell.CreateShortcut('%STARTUP%\%SHORTCUT_NAME%')
>> "%PS1%" echo $s.TargetPath = '%PYTHONW%'
>> "%PS1%" echo $s.Arguments = '"%SCRIPTS%\pc_companion_telemetry.py"'
>> "%PS1%" echo $s.WorkingDirectory = '%SCRIPTS%'
>> "%PS1%" echo $s.Description = 'PC Companion telemetry (CPU/RAM/GPU, Now Playing, Mute) -- runs silently at login, sends to the board over WiFi'
>> "%PS1%" echo $s.Save()

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
set "PS1_RESULT=%errorlevel%"
del "%PS1%" >nul 2>&1

if not "%PS1_RESULT%"=="0" (
    echo [ERROR] Failed to create the Startup shortcut.
    echo.
    if not defined SILENT pause
    exit /b 1
)

echo.
echo === Setup complete ===
echo   Script installed to : %SCRIPTS%\pc_companion_telemetry.py
echo   Auto-starts silently via: %STARTUP%\%SHORTCUT_NAME%
echo.

echo Starting it now, in the background, for the rest of this session...
start "" "%PYTHONW%" "%SCRIPTS%\pc_companion_telemetry.py"

:: ---- Verify it's actually running, don't just assume "start" worked ----
timeout /t 3 /nobreak >nul
tasklist /FI "IMAGENAME eq pythonw.exe" 2>NUL | find /I "pythonw.exe" >nul
if errorlevel 1 (
    echo.
    echo [WARNING] pythonw.exe does not appear to be running -- the telemetry
    echo service may not have started. Try running this script again, or run
    echo it manually to see any error: python "%SCRIPTS%\pc_companion_telemetry.py"
) else (
    echo.
    echo Confirmed: pythonw.exe is running.
)

echo.
echo Done. You can close this window -- the telemetry script is now
echo running silently and will keep auto-starting at every login.
echo.
if not defined SILENT pause
exit /b 0
