@echo off
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
cmake -S . -B build\Tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNB_APP=OFF >nul || exit /b 1
cmake --build build\Tests || exit /b 1
build\Tests\nb_tests.exe
