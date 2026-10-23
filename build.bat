@echo off
rem Builds dist\ishimurapatch.asi (x64). Needs Visual Studio with the
rem "Desktop development with C++" workload; run from anywhere.
rem "build.bat test" builds the playtest version (TEST_BUILD: neutral ini
rem and log names) into dist\test\ instead.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with C++ tools not found & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set "OUT=dist"
set "DEFS="
if /i "%~1"=="test" (set "OUT=dist\test" & set "DEFS=/DTEST_BUILD")
if not exist dist mkdir dist
if not exist %OUT% mkdir %OUT%
rem The build stamp in the log's first line: the commit built from, and
rem whether the working copy had uncommitted changes. The version number
rem itself (MOD_VERSION) only changes when a version is released.
set "BUILD_ID=no git"
for /f %%h in ('git rev-parse --short HEAD 2^>nul') do set "BUILD_ID=%%h"
git diff --quiet HEAD -- 2>nul
if errorlevel 1 if not "%BUILD_ID%"=="no git" set "BUILD_ID=%BUILD_ID% + local changes"
> dist\build_id.h echo #define BUILD_ID "%BUILD_ID%"
cl /nologo /LD /O2 /MT /EHsc /W3 /std:c++17 %DEFS% /Idist /Fo%OUT%\ /Fe%OUT%\ishimurapatch.asi ^
   ishimurapatch.cpp vtindex.c ^
   third_party\minhook\src\hook.c third_party\minhook\src\buffer.c ^
   third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde64.c || exit /b 1
if /i "%~1"=="test" exit /b 0
rem the development probe (loaded by the mod at runtime when [Advanced] Probe = 1)
cl /nologo /LD /O2 /MT /EHsc /W3 /std:c++17 /Fodist\ /Fedist\ishimurapatch_probe.dll probe.cpp
