(New-Object -ComObject Shell.Application).Windows() | ForEach-Object { $_.Quit() }
Get-Process | Where-Object {$_.MainWindowTitle -ne ""} | ForEach-Object { $_.CloseMainWindow() }
Get-Process powershell -ErrorAction SilentlyContinue | Stop-Process -Force