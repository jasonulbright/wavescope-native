@echo off
rem WaveScope native build. The core app is one /MT static exe with zero
rem runtime deps; the projectM (MilkDrop) engine is the one deliberate
rem dependency, linked as projectM-4.dll and staged next to the exe.
rem The unit tests (src\tests.cpp) build and RUN as part of every build;
rem a red test fails the build.
rem Find Visual Studio (any edition) via vswhere, falling back to the classic
rem Community path. The Installer dir joins PATH so vcvars' own vswhere
rem lookups work too.
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VSROOT="
if exist "%VSINSTALLER%\vswhere.exe" (
  set "PATH=%VSINSTALLER%;%PATH%"
  for /f "usebackq delims=" %%i in (`"%VSINSTALLER%\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
rem A running instance holds WaveScope.exe locked and fails the link.
taskkill /f /im WaveScope.exe >nul 2>&1
if not defined PMROOT set "PMROOT=C:\projects\projectm\install-native"

rem --- unit tests first: pure math must be green before the app links ---
cl /nologo /std:c++20 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE ^
   src\tests.cpp /Fe:"%~dp0wavescope-tests.exe"
if %errorlevel% neq 0 exit /b %errorlevel%
"%~dp0wavescope-tests.exe"
if %errorlevel% neq 0 exit /b %errorlevel%
del tests.obj >nul 2>&1

rem App icon (src\WaveScope.rc -> WaveScope.ico, see assets\icon*.svg for the
rem hand-authored source) compiles to a .res linked into the exe below.
rc /nologo /fo WaveScope.res src\WaveScope.rc
if %errorlevel% neq 0 exit /b %errorlevel%

rem Spout (third_party\spout, pinned - see PIN.md) compiles in for the O
rem output; d3d11/dxgi come with it.
cl /nologo /std:c++20 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /I "%PMROOT%\include" ^
   src\main.cpp ^
   third_party\spout\SpoutDX.cpp third_party\spout\SpoutDirectX.cpp ^
   third_party\spout\SpoutSenderNames.cpp third_party\spout\SpoutSharedMemory.cpp ^
   third_party\spout\SpoutFrameCount.cpp third_party\spout\SpoutCopy.cpp ^
   third_party\spout\SpoutUtils.cpp ^
   WaveScope.res ^
   /Fe:WaveScope.exe ^
   /link "%PMROOT%\lib\projectM-4.lib" d3d11.lib dxgi.lib ^
   /MANIFEST:EMBED /MANIFESTINPUT:src\WaveScope.manifest
if %errorlevel% neq 0 exit /b %errorlevel%
del *.obj WaveScope.res >nul 2>&1
rem The manifest (src\WaveScope.manifest) is embedded at link time; sweep any
rem stale external copy so it can't shadow the embedded one.
del WaveScope.exe.manifest >nul 2>&1

rem Stage the projectM DLLs beside the exe so it runs in place.
copy /y "%PMROOT%\bin\projectM-4.dll" . >nul
copy /y "%PMROOT%\bin\projectM-4-playlist.dll" . >nul
echo Built WaveScope.exe
