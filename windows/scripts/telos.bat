@echo off
:: ============================================================
:: Telos — the "close everything" companion to Kairos. Bound to
:: the Telos button on Page 1.
::
:: Runs close_all.ps1, which:
::   1. Quits all File Explorer windows
::   2. Sends a graceful close request to every other app with
::      a visible window (so unsaved-work prompts still appear)
::   3. Force-stops any lingering PowerShell process as a
::      catch-all
:: ============================================================
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "C:\pc_companion\scripts\close_all.ps1"