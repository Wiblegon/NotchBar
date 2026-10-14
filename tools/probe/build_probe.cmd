@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /utf-8 probe.cpp /Fe:probe.exe ole32.lib oleaut32.lib advapi32.lib
