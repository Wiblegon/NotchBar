@echo off
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if "%1"=="" (set CFG=Release) else (set CFG=%1)
cmake -S . -B build\%CFG% -G Ninja -DCMAKE_BUILD_TYPE=%CFG% >nul || exit /b 1
cmake --build build\%CFG% 2>&1
