@echo off
setlocal

cd /d "%~dp0"

git pull --ff-only origin main
if errorlevel 1 exit /b 1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
echo Build completed successfully.
exit /b 0
