@echo off
rem Builds the 32-bit OEM host (Windows 10/11). Usage: build.bat -> out\oem_host.exe
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist out mkdir out
cl /nologo /O2 /W4 /WX /EHsc /std:c++20 /MT /D_CRT_SECURE_NO_WARNINGS oem_host.cpp /Fe:out\oem_host.exe /Foout\ || exit /b 1
echo built out\oem_host.exe
