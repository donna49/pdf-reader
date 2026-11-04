# Removes FeatherPDF (per-user install).
$ErrorActionPreference = 'SilentlyContinue'
Get-Process FeatherPDF | Stop-Process -Force
$dir = Join-Path $env:LOCALAPPDATA 'Programs\FeatherPDF'
Remove-Item (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\FeatherPDF.lnk') -Force
Remove-Item 'HKCU:\Software\Classes\FeatherPDF.Document' -Recurse -Force
Remove-ItemProperty 'HKCU:\Software\Classes\.pdf\OpenWithProgids' -Name 'FeatherPDF.Document' -Force
Remove-Item 'HKCU:\Software\Classes\Applications\FeatherPDF.exe' -Recurse -Force
Remove-Item 'HKCU:\Software\FeatherPDF' -Recurse -Force
Remove-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name 'FeatherPDF' -Force
Remove-Item 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\FeatherPDF' -Recurse -Force
Remove-Item (Join-Path $env:APPDATA 'FeatherPDF') -Recurse -Force
# The script may live inside $dir; delete the folder after this process exits.
Start-Process cmd.exe -ArgumentList "/c timeout /t 2 >nul & rmdir /s /q `"$dir`"" -WindowStyle Hidden
Write-Host "FeatherPDF has been removed."
