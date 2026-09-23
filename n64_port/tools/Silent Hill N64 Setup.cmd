@echo off
setlocal
title Silent Hill - N64 SD card setup

rem Drag a Silent Hill disc image (.bin or .cue) onto this file, or just run it
rem and type the path when asked.

set "PY="
for %%P in ("py -3" "python" "python3") do (
    if not defined PY (
        %%~P -c "import sys; sys.exit(0 if sys.version_info >= (3,8) else 1)" >nul 2>&1
        if not errorlevel 1 set "PY=%%~P"
    )
)

if not defined PY (
    echo.
    echo   Python 3.8 or newer is needed and was not found.
    echo.
    echo   Install it from https://www.python.org/downloads/ and tick
    echo   "Add python.exe to PATH" in the installer, then run this again.
    echo.
    pause
    exit /b 1
)

%PY% "%~dp0sh_n64_setup.py" %*
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
