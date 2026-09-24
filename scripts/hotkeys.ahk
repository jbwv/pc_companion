#SingleInstance Force

; These match the factory-default Hotkeys page (page1_buttons.h) --
; edit the targets below (or add/remove lines to match however you've
; customized Hotkeys/Hotkeys 2 from the board's web editor) to launch
; whatever apps/scripts you actually want. Paths below are examples --
; update them for whatever's actually installed on this PC.
;
; NOTE: Ctrl+Alt+I (Toolbox) and Ctrl+Alt+M (Mithril) POINT TO PROJECTS
; NOT RELEASED ON GITHUB YET -- they will not do anything useful on your
; PC as shipped (Toolbox has no target at all here; Mithril's mithril.bat
; exists but the mithril_py project it launches isn't included). See
; docs/SETUP.md section 5. Relabel those two buttons (web editor or
; page1_buttons.h) and point these lines at your own scripts/apps.
^!k::Run "C:\pc_companion\scripts\kairos.bat"
^!t::Run "C:\pc_companion\scripts\telos.bat"
^!m::Run "C:\pc_companion\scripts\mithril.bat"
^!p::Run "powershell.exe"
^!n::Run "notepad.exe"
^!i::Run "D:\toolbox_py\toolbox_py\toolbox.pyw"  ; NOT RELEASED -- see NOTE above

; === PC Companion: auto-generated from board web config -- DO NOT EDIT BELOW ===
; This section is empty on a fresh install. Once you set a button's
; Action from the board's web editor (http://<board-ip>/) and save,
; pc_companion_telemetry.py fills this in automatically and reloads
; AutoHotkey for you -- nothing to do here by hand.
; === PC Companion: end auto-generated section ===
