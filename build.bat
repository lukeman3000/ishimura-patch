@echo off
rem Builds dist\ishimurapatch.asi (x64). Needs Visual Studio with the
rem "Desktop development with C++" workload; run from anywhere.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with C++ tools not found & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist dist mkdir dist
cl /nologo /LD /O2 /MT /EHsc /W3 /std:c++17 /Fodist\ /Fedist\ishimurapatch.asi ^
   ishimurapatch.cpp vtindex.c ^
   third_party\minhook\src\hook.c third_party\minhook\src\buffer.c ^
   third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde64.c
