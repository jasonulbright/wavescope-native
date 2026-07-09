@echo off
rem SpectraViz native build: one file, one command, one .exe.
rem /MT statically links the CRT: the output has zero runtime dependencies.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c++20 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE src\main.cpp /Fe:SpectraViz.exe
if %errorlevel% neq 0 exit /b %errorlevel%
del main.obj >nul 2>&1
echo Built SpectraViz.exe
