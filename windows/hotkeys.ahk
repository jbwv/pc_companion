; ============================================================
; pc_companion hotkeys
;
; One line per Ctrl+Alt+ binding. Each line runs the matching
; script from scripts\, or a plain Windows executable.
;
; Only bindings for buttons that DON'T use a native Windows
; shortcut need an entry here (Snipping Tool, File Explorer,
; Task Manager, and Lock Computer all use native shortcuts and
; need no line in this file at all).
;
; Add your own here as you customize page1_buttons.h to match.
; ============================================================

^!k::Run "C:\pc_companion\scripts\kairos.bat"
^!t::Run "C:\pc_companion\scripts\telos.bat"
^!p::Run "powershell.exe"
^!n::Run "notepad.exe"
