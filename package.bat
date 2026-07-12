@echo off
setlocal
pushd "%~dp0"
rem WaveScope release packer: build (test gate included), stage the portable
rem layout under dist\, run the STAGED exe's selftest as the release gate,
rem sweep the selftest artifacts, and zip dist\WaveScope-v<ver>-win64.zip.

rem The version lives in src\main.cpp (WAVESCOPE_VERSION) - one source of truth.
set "RAW="
for /f "tokens=3" %%v in ('findstr /C:"#define WAVESCOPE_VERSION" src\main.cpp') do set "RAW=%%v"
if not defined RAW echo Could not read WAVESCOPE_VERSION from src\main.cpp & exit /b 1
set "VER=%RAW:~2,-1%"

call "%~dp0build.bat"
if %errorlevel% neq 0 exit /b %errorlevel%

rem Locate the app-local VC++ runtime for the projectM DLLs (the core exe is
rem /MT). Same vswhere lookup as build.bat; filesystem only, no vcvars needed.
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VSROOT="
if exist "%VSINSTALLER%\vswhere.exe" (
  for /f "usebackq delims=" %%i in (`"%VSINSTALLER%\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CRTDIR="
for /d %%d in ("%VSROOT%\VC\Redist\MSVC\*") do (
  for /d %%c in ("%%d\x64\Microsoft.VC*.CRT") do set "CRTDIR=%%c"
)
if not defined CRTDIR echo No VC redist CRT folder found under %VSROOT% & exit /b 1

set "STAGE=dist\WaveScope-v%VER%-win64"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"

copy /y WaveScope.exe "%STAGE%" >nul
copy /y projectM-4.dll "%STAGE%" >nul
copy /y projectM-4-playlist.dll "%STAGE%" >nul
copy /y "%CRTDIR%\*.dll" "%STAGE%" >nul
xcopy /e /i /q presets "%STAGE%\presets" >nul
xcopy /e /i /q presets-classic "%STAGE%\presets-classic" >nul
xcopy /e /i /q textures "%STAGE%\textures" >nul
copy /y README.md "%STAGE%" >nul
copy /y LICENSE "%STAGE%" >nul
copy /y CHANGELOG.md "%STAGE%" >nul
copy /y THIRD-PARTY-NOTICES.txt "%STAGE%" >nul

rem Release gate: the STAGED exe must pass its own selftest in place - this
rem proves the zip layout (DLLs, CRT, presets, textures) runs standalone.
pushd "%STAGE%"
start "" /wait "%CD%\WaveScope.exe" selftest
if %errorlevel% neq 0 (
  echo staged selftest FAILED - see %STAGE%\wavescope-selftest.log
  popd
  exit /b 1
)
rem (wavescope.ini appears because the Spotify loader persists its default
rem clientId on first launch - the zip ships no config, first run recreates it)
del /q selftest-*.png wavescope-selftest.log wavescope-*.mp4 wavescope.ini 2>nul
popd

set "ZIP=dist\WaveScope-v%VER%-win64.zip"
del /q "%ZIP%" 2>nul
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%ZIP%' -Force"
if %errorlevel% neq 0 exit /b %errorlevel%
for %%z in ("%ZIP%") do echo Packaged %%z (%%~zz bytes)
popd
