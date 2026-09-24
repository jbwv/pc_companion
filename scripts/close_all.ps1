# Telos (close_all) -- closes everything open at once.
#
# Fixed version. Root causes (see pc_companion_roadmap.md "Fix Telos
# (close_all)" and the live diagnostic run that pinned down the last
# one):
#
# 1. The script's own hidden PowerShell process matched
#    `Get-Process powershell | Stop-Process` and killed itself mid-run,
#    before it finished closing everything else. Fixed by excluding
#    $PID (this process's own ID) from that line.
# 2. Windows 11's tabbed File Explorer broke the old
#    `(New-Object -ComObject Shell.Application).Windows() | ForEach {$_.Quit()}`
#    approach -- it doesn't reliably see/close tabbed Explorer windows.
# 3. A later attempt to fix #2 with `FindWindowEx(..., "CabinetWClass", ...)`
#    looked right but silently found ZERO windows every time -- confirmed
#    live: FindWindowEx's P/Invoke declaration wasn't given an explicit
#    CharSet, so it quietly resolved to the ANSI variant and never
#    matched the class name, even though the window was right there.
#    Fixed by switching to EnumWindows + GetClassName instead (each
#    declared with CharSet = CharSet.Auto), which was confirmed live to
#    find and correctly identify the CabinetWClass (File Explorer)
#    window every time -- collect every matching handle first, then
#    close each one, rather than repeatedly re-searching mid-loop.
# 4. Windows Terminal is its own separate process (WindowsTerminal.exe),
#    not powershell.exe -- an explicit Stop-Process line was needed for
#    it too, or it survives everything else closing.
#
# An earlier "count open windows, then fire Alt+F4 that many times"
# approach was tried and abandoned -- it's fundamentally unreliable
# since focus shifts unpredictably as windows close, and worst case can
# land on the desktop and pop the Shut Down Windows dialog.

Add-Type -Namespace Win32 -Name WindowCloser -MemberDefinition @"
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
"@

$WM_CLOSE = 0x0010

# ---- Fix #2/#3: find every File Explorer window by walking ALL
# top-level windows (EnumWindows) and checking each one's real class
# name (GetClassName) -- proven, live, to actually find CabinetWClass
# windows where FindWindowEx silently didn't. Collect handles first,
# then close them, so closing one window mid-enumeration can't disturb
# the walk.
$explorerWindows = New-Object System.Collections.Generic.List[IntPtr]
$enumCallback = {
    param($hWnd, $lParam)
    if ([Win32.WindowCloser]::IsWindowVisible($hWnd)) {
        $sb = New-Object System.Text.StringBuilder 256
        [Win32.WindowCloser]::GetClassName($hWnd, $sb, 256) | Out-Null
        if ($sb.ToString() -eq "CabinetWClass") {
            $explorerWindows.Add($hWnd)
        }
    }
    return $true
}
[Win32.WindowCloser]::EnumWindows($enumCallback, [IntPtr]::Zero) | Out-Null
foreach ($hWnd in $explorerWindows) {
    [Win32.WindowCloser]::SendMessage($hWnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
}

# ---- Ask every other visible-window app to close normally ----
Get-Process | Where-Object { $_.MainWindowTitle -ne "" } | ForEach-Object { $_.CloseMainWindow() }

# ---- Fix #4: Windows Terminal is its own process, not powershell.exe ----
Get-Process WindowsTerminal -ErrorAction SilentlyContinue | Stop-Process -Force

# ---- Final catch-all: force-close any lingering powershell process --
# Fix #1: exclude this script's OWN process ($PID), or it kills itself
# mid-run before finishing the cleanup above.
Get-Process powershell -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $PID } | Stop-Process -Force
