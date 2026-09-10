@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\package.ps1" %*
exit /b %errorlevel%
