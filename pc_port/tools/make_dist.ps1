# make_dist.ps1 - mirror a clean, distributable build into dist\.
#
# Takes the build directory and produces a folder a tester can run, leaving out
# everything that should not travel: build intermediates, logs, static libs and
# generated headers; the three copyrighted disc .bin images (a tester supplies
# their own, as every fan port requires); and personal saves. Everything the
# port needs at runtime - the exe, the launcher, the runtime DLLs, maps\, and
# gamedata\ minus the disc images and saves - is copied.
#
# robocopy /MIR keeps dist\ an exact mirror on every run (added files appear,
# removed ones disappear), so it is cheap to run after each build.
#
# Usage:  powershell -ExecutionPolicy Bypass -File make_dist.ps1 [-Build <dir>] [-Dist <dir>]

param(
    [string]$Build = "C:\Claude\silenthill-online\silent-hill-decomp\pc_port\build",
    [string]$Dist  = "C:\Claude\silenthill-online\dist"
)

if (-not (Test-Path $Build)) { Write-Error "Build dir not found: $Build"; exit 1 }
New-Item -ItemType Directory -Force -Path $Dist | Out-Null

# Files that must never ship: logs, the old manual package, ninja/cmake state,
# static libs, generated headers, the capture helper, and the disc images.
$xf = @(
    "*.log", "SHOnlineBuild.zip", "*.zip",
    "build.ninja", ".ninja_deps", ".ninja_log",
    "CMakeCache.txt", "cmake_install.cmake",
    "*.a", "*.dll.a",
    "sh_build_info.h", "sh_version.h",
    "run_capture.bat",
    "Silent Hill (USA).bin",
    "Silent Hill (Europe) (En,Fr,De,Es,It).bin",
    "Silent Hill (Japan).bin"
)

# Directories that must never ship: cmake/ninja build trees, the psycross build
# subdir (its .a is linked into the exe already), personal saves, temp, and the
# extracted FMV movies (disc-derived and 1.5 GB - a tester plays them from their
# own disc, same as the .bin images).
$xd = @(
    (Join-Path $Build "CMakeFiles"),
    (Join-Path $Build "psycross"),
    (Join-Path $Build "gamedata\FMV"),
    (Join-Path $Build "gamedata\save"),
    (Join-Path $Build "gamedata\save2"),
    (Join-Path $Build "gamedata\save3"),
    (Join-Path $Build "gamedata\temp")
)

$args = @($Build, $Dist, "/MIR", "/NFL", "/NDL", "/NJH", "/NP", "/R:1", "/W:1")
$args += "/XF"; $args += $xf
$args += "/XD"; $args += $xd

& robocopy @args | Out-Null
$code = $LASTEXITCODE  # robocopy: 0-7 are success (8+ is failure)

# /MIR does not descend into an /XD-excluded directory, so one that was copied
# by an earlier run (before it was excluded) would linger. Remove the excluded
# disc-derived / personal dirs from dist explicitly, so the script is idempotent
# whatever state dist was left in.
foreach ($d in @("gamedata\FMV", "gamedata\temp")) {
    $p = Join-Path $Dist $d
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
}

# Folders the game writes into at runtime; ship them empty so a fresh install
# has somewhere to put saves without the disc data or anyone's progress.
foreach ($d in @("gamedata\save", "gamedata\save2", "gamedata\save3", "gamedata\coopsaves")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Dist $d) | Out-Null
}

if ($code -ge 8) { Write-Error "robocopy failed ($code)"; exit $code }

$size = (Get-ChildItem -Recurse -File $Dist | Measure-Object Length -Sum).Sum
"{0}: {1:N0} MB in dist (disc .bin images and personal saves excluded)" -f $Dist, ($size / 1MB)
exit 0
