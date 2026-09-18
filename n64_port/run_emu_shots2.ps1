# Run a ROM in ares, wait for a LOG MARKER (not a wall-clock guess), then take
# screenshots. ares' speed swings between 1 and 50 VPS on this host, so fixed
# -FirstShot timings captured the boot console as often as the game.
#
#   powershell -ExecutionPolicy Bypass -File n64_port/run_emu_shots2.ps1 `
#       -Rom n64_port/bin/sh_diag.z64 -WaitFor "T3DW] blocks=" -Shots 6 -Every 10 -MaxWait 400
#
# Output: %TEMP%/n64shots/<stamp>/ares_log.txt + shot_NNNs.png
param(
    [Parameter(Mandatory=$true)][string]$Rom,
    [string]$WaitFor = "T3DW] blocks=",
    [int]$Shots = 6,
    [int]$Every = 10,
    [int]$MaxWait = 420,
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
$u = (Add-Type -MemberDefinition $sig -Name WinCap3 -Namespace Native3 -PassThru) | Where-Object { $_.Name -eq 'WinCap3' }

function Grab($path) {
    $p.Refresh()
    $hwnd = $p.MainWindowHandle
    if ($hwnd -eq [IntPtr]::Zero) { return $false }
    $r = New-Object Native3.WinCap3+RECT
    [void]$u::GetWindowRect($hwnd, [ref]$r)
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { return $false }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc(); [void]$u::PrintWindow($hwnd, $dc, 2); $g.ReleaseHdc($dc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $true
}

# wait for the marker
$t = 0; $seen = $false
while ($t -lt $MaxWait) {
    Start-Sleep -Seconds 5
    $t += 5
    if ($p.HasExited) { Write-Output "emulator exited at $t s"; break }
    if (Test-Path $log) {
        $txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($txt -and $txt.Contains($WaitFor)) { $seen = $true; break }
    }
}
Write-Output "marker '$WaitFor' seen=$seen after $t s"
$n = 0
if ($seen) {
    for ($i = 0; $i -lt $Shots; $i++) {
        Start-Sleep -Seconds $Every
        if ($p.HasExited) { break }
        if (Grab (Join-Path $outDir ("shot_{0:D2}.png" -f $i))) { $n++ }
    }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output "dir : $outDir"
Write-Output "log : $log ($((Get-Item $log).Length) bytes), shots: $n"
