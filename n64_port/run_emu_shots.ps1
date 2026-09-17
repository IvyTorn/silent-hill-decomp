# Run a SMALL (<64MB) ROM in ares, capture the game's debug log (ISViewer on
# stdout, as run_emu_log.ps1) AND a PrintWindow screenshot every -Every
# seconds from -FirstShot on. The pad robot (pad_n64.c SH_N64_AUTOSTART, only
# without an SD card) walks New Game; main_n64.c starts emulator runs on
# map2_s02, the exterior behind the police-station door.
#
#   powershell -ExecutionPolicy Bypass -File n64_port/run_emu_shots.ps1 -Rom n64_port/bin/sh_diag.z64 -Seconds 600 -FirstShot 150 -Every 30
#
# Output: %TEMP%/n64shots/<stamp>/ares_log.txt + shot_NNNs.png. A minimized
# ares window yields 160x28 shots; keep it restored.
param(
    [Parameter(Mandatory=$true)][string]$Rom,
    [int]$Seconds = 480,
    [int]$FirstShot = 180,
    [int]$Every = 30,
    [string]$Emu = "C:\Emulators\ares\ares-v148\ares.exe"
)
Add-Type -AssemblyName System.Drawing
$stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $env:TEMP "n64shots\$stamp"
New-Item -ItemType Directory -Force $outDir | Out-Null
$log = Join-Path $outDir "ares_log.txt"
$err = Join-Path $outDir "ares_err.txt"
$romFull = (Resolve-Path $Rom).Path
$p = Start-Process -FilePath $Emu -ArgumentList @('--system', '"Nintendo 64"', '--no-file-prompt', "`"$romFull`"") `
     -RedirectStandardOutput $log -RedirectStandardError $err -PassThru

$sig = @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
$u = (Add-Type -MemberDefinition $sig -Name WinCap2 -Namespace Native2 -PassThru) | Where-Object { $_.Name -eq 'WinCap2' }

$t = 0
$n = 0
while ($t -lt $Seconds) {
    Start-Sleep -Seconds 5
    $t += 5
    if ($p.HasExited) { Write-Output "emulator exited at $t s"; break }
    if ($t -ge $FirstShot -and (($t - $FirstShot) % $Every) -eq 0) {
        $p.Refresh()
        $hwnd = $p.MainWindowHandle
        if ($hwnd -ne [IntPtr]::Zero) {
            $r = New-Object Native2.WinCap2+RECT
            [void]$u::GetWindowRect($hwnd, [ref]$r)
            $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
            if ($w -gt 0 -and $h -gt 0) {
                $bmp = New-Object System.Drawing.Bitmap $w, $h
                $g = [System.Drawing.Graphics]::FromImage($bmp)
                $dc = $g.GetHdc()
                [void]$u::PrintWindow($hwnd, $dc, 2)
                $g.ReleaseHdc($dc)
                $bmp.Save((Join-Path $outDir ("shot_{0:D3}s.png" -f $t)), [System.Drawing.Imaging.ImageFormat]::Png)
                $g.Dispose(); $bmp.Dispose()
                $n++
            }
        }
    }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "dir : $outDir"
Write-Output "log : $log ($((Get-Item $log).Length) bytes), shots: $n"
