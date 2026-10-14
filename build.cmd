@echo off
rem Reproducible build: Visual Studio 2022 Build Tools (MSVC v143), Windows SDK 10.0.22621+ (C++/WinRT headers ship with the SDK).
rem Usage: build.cmd [Release^|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL (echo MSVC bulunamadi & exit /b 1)
set VSLANG=1033
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cmake -S . -B build\%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build build\%CONFIG% || exit /b 1
ctest --test-dir build\%CONFIG% --output-on-failure || exit /b 1
echo.
echo Cikti: %~dp0build\%CONFIG%\NotchBar.exe
