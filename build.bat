@echo off
rem Build Star 3D (New 3DS port) with Docker.
rem Put your own GameCube disc image (.iso, PAL GSAP01) next to this file, then run build.bat.
rem The release set is written to out\: the code-only star3d.3dsx and star3d.cia plus
rem star3d-gamedata.zip (the game data for the SD card; see README.md and RELEASING.md).
rem Arguments are build targets, for example:
rem   build.bat           the release set (all)
rem   build.bat code      code-only .3dsx (no ISO needed)
rem   build.bat cia       only the .cia
rem   build.bat gamedata  only the game data zip
rem   build.bat full      .3dsx + .cia with the data embedded
rem   build.bat help      list every target
setlocal
cd /d "%~dp0"

docker build -t star3d-builder . || goto :error
docker run --rm -v "%cd%:/workspace" star3d-builder %* || goto :error
exit /b 0

:error
echo.
echo Build failed. Is Docker Desktop installed and running?
exit /b 1
