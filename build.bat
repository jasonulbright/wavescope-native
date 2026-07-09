@echo off
rem SpectraViz native build. The core app is one /MT static exe with zero
rem runtime deps; the projectM (MilkDrop) engine is the one deliberate
rem dependency, linked as projectM-4.dll and staged next to the exe.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PMROOT=C:\projects\projectm\install-native"
cl /nologo /std:c++20 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE ^
   /I "%PMROOT%\include" ^
   src\main.cpp ^
   /Fe:SpectraViz.exe ^
   /link "%PMROOT%\lib\projectM-4.lib"
if %errorlevel% neq 0 exit /b %errorlevel%
del main.obj >nul 2>&1

rem Stage the projectM DLLs beside the exe so it runs in place.
copy /y "%PMROOT%\bin\projectM-4.dll" . >nul
copy /y "%PMROOT%\bin\projectM-4-playlist.dll" . >nul
echo Built SpectraViz.exe
