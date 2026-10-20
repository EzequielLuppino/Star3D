@echo off
rem Build Star 3D (New 3DS port) with Docker.
rem Put your own GameCube disc image (.iso, PAL GSAP01) next to this file, then run build.bat.
rem The result is written to out\star3d.3dsx. Arguments are passed to make
rem (for example: build.bat clean).
setlocal
cd /d "%~dp0"

docker build -t star3d-builder . || goto :error
docker run --rm -v "%cd%:/workspace" star3d-builder %* || goto :error
exit /b 0

:error
echo.
echo Build failed. Is Docker Desktop installed and running?
exit /b 1
