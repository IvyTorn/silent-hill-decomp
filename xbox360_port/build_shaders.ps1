# Compile the Xenos shaders with the Xbox 360 XDK's fxc.
#
#   powershell -ExecutionPolicy Bypass -File xbox360_port\build_shaders.ps1
#
# Output goes next to the sources as vs.vsu / ps.psu, which is what
# gpu_xenos.c looks for beside the disc image at runtime. They are ASSETS, not
# linked into the ELF, so reshading needs no rebuild.
#
# fxc is the only piece of the XDK this port uses. It emits the same container
# libXenon's Xe_LoadShaderFromMemory validates -- magic >> 16 == 0x102a -- which
# was verified by compiling libXenon's own cube example and byte-comparing
# against its shipped blobs (identical but for an embedded version string).
#
# NEVER commit anything out of the XDK itself. Compiled shader output is our own
# work and is fine to keep; XDK headers, libs and binaries are not.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "shaders"

$fxc = "C:\Program Files (x86)\Microsoft Xbox 360 SDK\bin\win32\fxc.exe"
if (-not (Test-Path $fxc)) {
    Write-Error "fxc not found at $fxc - set the path for your XDK install"
}

Push-Location $src
try {
    Write-Host "[ FXC ] vs.hlsl -> vs.vsu"
    & $fxc /Tvs_3_0 /Fovs.vsu vs.hlsl
    if ($LASTEXITCODE -ne 0) { Write-Error "vertex shader failed" }

    Write-Host "[ FXC ] ps.hlsl -> ps.psu"
    & $fxc /Tps_3_0 /Fops.psu ps.hlsl
    if ($LASTEXITCODE -ne 0) { Write-Error "pixel shader failed" }

    # Cheap sanity gate: libXenon refuses anything whose magic's high half is not
    # 0x102a, and it refuses it by calling Xe_Fatal, so catching it here is much
    # friendlier than catching it on the console.
    foreach ($f in @("vs.vsu", "ps.psu")) {
        $b = [IO.File]::ReadAllBytes((Join-Path $src $f))
        if ($b[0] -ne 0x10 -or $b[1] -ne 0x2a) {
            Write-Error "$f has magic $('{0:x2}{1:x2}' -f $b[0],$b[1]), expected 102a"
        }
        Write-Host ("        {0}  {1} bytes  magic ok" -f $f, $b.Length)
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "Deploy: copy shaders\vs.vsu and shaders\ps.psu next to the .bin on the USB stick"
