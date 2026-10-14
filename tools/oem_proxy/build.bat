@echo off
rem Builds the XP-compatible 32-bit logging proxy for PakonIMAu.dll.
rem Usage (from this folder): build.bat   -> out\PakonIMAu.dll
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist out mkdir out
cl /nologo /c /O1 /GS- /Gs999999 /EHs-c- /GR- /Zl /W4 /WX /std:c++17 pakonimau_proxy.cpp /Foout\pakonimau_proxy.obj || exit /b 1
link /nologo /DLL /NODEFAULTLIB /ENTRY:ProxyEntry /SUBSYSTEM:WINDOWS,5.01 /OSVERSION:5.1 /DEF:pakonimau_proxy.def /OUT:out\PakonIMAu.dll out\pakonimau_proxy.obj kernel32.lib user32.lib || exit /b 1
echo built out\PakonIMAu.dll
