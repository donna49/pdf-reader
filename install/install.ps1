# FeatherPDF installer (per-user, no administrator rights needed).
# Copies FeatherPDF.exe to %LOCALAPPDATA%\Programs\FeatherPDF, adds a Start menu
# shortcut, and registers FeatherPDF as a PDF handler so you can make it the default.
$ErrorActionPreference = 'Stop'

$src = Join-Path $PSScriptRoot 'FeatherPDF.exe'
if (-not (Test-Path $src)) { throw "FeatherPDF.exe was not found next to this script." }

$dir = Join-Path $env:LOCALAPPDATA 'Programs\FeatherPDF'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Get-Process FeatherPDF -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Copy-Item $src (Join-Path $dir 'FeatherPDF.exe') -Force
Copy-Item (Join-Path $PSScriptRoot 'uninstall.ps1') $dir -Force -ErrorAction SilentlyContinue
$exe = Join-Path $dir 'FeatherPDF.exe'
Unblock-File $exe -ErrorAction SilentlyContinue

# Start menu shortcut
$lnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\FeatherPDF.lnk'
$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk)
$s.TargetPath = $exe
$s.WorkingDirectory = $dir
$s.Description = 'FeatherPDF - lightweight PDF reader'
$s.Save()

# File association (current user only)
$cls = 'HKCU:\Software\Classes'
$prog = "$cls\FeatherPDF.Document"
New-Item -Force "$prog\shell\open\command" | Out-Null
Set-ItemProperty $prog '(default)' 'PDF Document'
New-Item -Force "$prog\DefaultIcon" | Out-Null
Set-ItemProperty "$prog\DefaultIcon" '(default)' "`"$exe`",0"
Set-ItemProperty "$prog\shell\open\command" '(default)' "`"$exe`" `"%1`""

New-Item -Force "$cls\.pdf\OpenWithProgids" | Out-Null
New-ItemProperty -Force "$cls\.pdf\OpenWithProgids" -Name 'FeatherPDF.Document' -Value ([byte[]]@()) -PropertyType None | Out-Null

$app = "$cls\Applications\FeatherPDF.exe"
New-Item -Force "$app\shell\open\command" | Out-Null
Set-ItemProperty "$app\shell\open\command" '(default)' "`"$exe`" `"%1`""
New-Item -Force "$app\SupportedTypes" | Out-Null
New-ItemProperty -Force "$app\SupportedTypes" -Name '.pdf' -Value '' | Out-Null
Set-ItemProperty $app 'FriendlyAppName' 'FeatherPDF'

$cap = 'HKCU:\Software\FeatherPDF\Capabilities'
New-Item -Force "$cap\FileAssociations" | Out-Null
Set-ItemProperty $cap 'ApplicationName' 'FeatherPDF'
Set-ItemProperty $cap 'ApplicationDescription' 'Lightweight PDF reader'
Set-ItemProperty "$cap\FileAssociations" '.pdf' 'FeatherPDF.Document'
New-Item -Force 'HKCU:\Software\RegisteredApplications' | Out-Null
Set-ItemProperty 'HKCU:\Software\RegisteredApplications' 'FeatherPDF' 'Software\FeatherPDF\Capabilities'

# Apps & features entry so it can be uninstalled from Settings
$un = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\FeatherPDF'
New-Item -Force $un | Out-Null
Set-ItemProperty $un 'DisplayName' 'FeatherPDF'
Set-ItemProperty $un 'DisplayVersion' '1.0.0'
Set-ItemProperty $un 'Publisher' 'FeatherPDF'
Set-ItemProperty $un 'DisplayIcon' $exe
Set-ItemProperty $un 'InstallLocation' $dir
Set-ItemProperty $un 'UninstallString' "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$dir\uninstall.ps1`""
Set-ItemProperty $un 'NoModify' 1 -Type DWord
Set-ItemProperty $un 'NoRepair' 1 -Type DWord

# Tell Explorer that associations changed
Add-Type -Namespace Win32 -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);'
[Win32.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

Write-Host ""
Write-Host "FeatherPDF installed to $dir" -ForegroundColor Green
Write-Host ""
Write-Host "Last step: Windows requires YOU to pick the default PDF app."
Write-Host "The Settings page will open now: choose FeatherPDF for .pdf files."
Start-Process 'ms-settings:defaultapps?registeredAppUser=FeatherPDF'
