# Boot a .z64 in an emulator, grab the window after a delay, and kill it.
#
#   powershell -File n64_port/run_emu.ps1 -Rom n64_port/bin/sh.z64 -Seconds 10
#
# Exists because the useful headless option (mupen64plus --testshots) is only
# in the 2019 build on this machine, and that one cannot boot libdragon's
# open-source IPL3 -- it segfaults on the user's own SummerCart64 menu ROM too,
# which is the control that proves it is the emulator and not our ROM.
#
# Shots go to a NEW timestamped directory every run. Never delete old ones:
# clearing a shot directory prompts the user, and comparing this run against
# the last one is most of what these images are for.
# ares, not the local mupen64plus or Project64. mupen64plus 2.5 (2019) cannot
# boot libdragon's open-source IPL3 -- it segfaults on the SummerCart64 menu ROM
# too, which is the control that proves it is the emulator. Project64 3.0
# ignored the ROM on the command line and sat in its browser.
param(
    [Parameter(Mandatory=$true)][string]$Rom,
    [int]$Seconds = 10,
    [string]$Emu = "C:\Emulators\ares\ares-v148\ares.exe",
    [string]$OutDir = "$env:TEMP\n64shots"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$dir = Join-Path $OutDir $stamp
New-Item -ItemType Directory -Force $dir | Out-Null

$romFull = (Resolve-Path $Rom).Path
Write-Output "rom : $romFull"
Write-Output "emu : $Emu"

if ($Emu -match 'ares') {
    # --no-file-prompt or ares stops for a 64DD disk dialog and never boots.
    $emuArgs = @('--system', '"Nintendo 64"', '--no-file-prompt', "`"$romFull`"")
} else {
    $emuArgs = @("`"$romFull`"")
}
$p = Start-Process -FilePath $Emu -ArgumentList $emuArgs -PassThru
Start-Sleep -Seconds $Seconds

if ($p.HasExited) {
    Write-Output "EMULATOR EXITED EARLY (code $($p.ExitCode)) - it could not boot the ROM"
    exit 1
}

# PrintWindow rather than a screen grab: it works on an unfocused or partly
# covered window, which matters when this runs unattended.
$sig = @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
# -PassThru returns BOTH the generated class and the nested RECT struct, so it
# has to be filtered; using the array directly fails with "does not contain a
# method named GetWindowRect".
$u = (Add-Type -MemberDefinition $sig -Name WinCap -Namespace Native -PassThru) |
        Where-Object { $_.Name -eq 'WinCap' }

$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Output "no main window"
    Stop-Process -Id $p.Id -Force
    exit 1
}

$r = New-Object Native.WinCap+RECT
[void]$u::GetWindowRect($hwnd, [ref]$r)
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# flag 2 = PW_RENDERFULLCONTENT, needed for a GPU-composited client area
[void]$u::PrintWindow($hwnd, $dc, 2)
$g.ReleaseHdc($dc)
$out = Join-Path $dir "shot.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "shot: $out"
