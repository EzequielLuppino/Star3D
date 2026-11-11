@echo off
rem Build Star 3D (New 3DS port) with Docker.
rem Put your own GameCube disc image (.iso, PAL GSAP01) next to this file.
rem With no arguments, a menu of the usual build options is shown. Arguments
rem are passed to make instead (for example: build.bat help, build.bat clean).
setlocal
cd /d "%~dp0"

docker build -t star3d-builder . || goto :error
if not "%1"=="" goto :passthrough

:menu
echo.
echo Star 3D build menu
echo ==================
echo   1^) Game data    Convert the disc into out\star3d-gamedata.zip
echo                   ^(players extract this once per SD card^)
echo   2^) .3dsx        Code-only build for the Homebrew Launcher
echo   3^) .cia         Code-only build, install with FBI
echo   4^) Everything   The whole release set: game data + .3dsx + .cia
echo   5^) Clean        Remove the build outputs
echo   0^) Exit
echo.
choice /C 123450 /N /M "Choice: "
if errorlevel 6 exit /b 0
if errorlevel 5 set TARGET=clean & goto :build
if errorlevel 4 set TARGET=all & goto :build
if errorlevel 3 set TARGET=cia & goto :build
if errorlevel 2 set TARGET=3dsx & goto :build
if errorlevel 1 set TARGET=gamedata & goto :build
exit /b 0

:passthrough
docker run --rm -v "%cd%:/workspace" star3d-builder %* || goto :error
goto :end

:build
docker run --rm -v "%cd%:/workspace" star3d-builder %TARGET% || goto :error
goto :end

:error
echo.
echo Build failed. Is Docker Desktop installed and running?
exit /b 1

:end
echo.
echo Done. The results are in the out\ folder (README.md: Installing).
exit /b 0
