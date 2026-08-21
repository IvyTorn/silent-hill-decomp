# Run a SMALL (<64MB) ROM in ares and capture the game's full debug log.
#
# ares implements the ISViewer debug channel and prints it to stdout, but ONLY
# for ROMs that fit the cart window -- the 85MB sh.z64 silently loses it, which
# is why this script exists alongside run_emu.ps1 (screenshots). Use
# bin/sh_diag.z64 (SND-less disc pack + maps, ~28MB) built by build_n64.sh.
#
#   powershell -File n64_port/run_emu_log.ps1 -Rom n64_port/bin/sh_diag.z64 -Seconds 480
#
# The log lands next to the shot dir: %TEMP%\n64shots\<stamp>\ares_log.txt
param(
    [Parameter(Mandatory=$true)][string]$Rom,
    [int]$Seconds = 300,
    [string]$Emu = "C:\Emulators\ares\ares-v148\ares.exe"
)

$stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $env:TEMP "n64shots\$stamp"
New-Item -ItemType Directory -Force $outDir | Out-Null
$log = Join-Path $outDir "ares_log.txt"
$err = Join-Path $outDir "ares_err.txt"

$p = Start-Process -FilePath $Emu -ArgumentList ('"' + (Resolve-Path $Rom) + '"') `
     -RedirectStandardOutput $log -RedirectStandardError $err -PassThru
Start-Sleep -Seconds $Seconds
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

Write-Output "rom : $Rom"
Write-Output "log : $log"
Write-Output ("size: {0} bytes" -f (Get-Item $log).Length)
