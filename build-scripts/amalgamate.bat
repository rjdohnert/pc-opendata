@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0amalgamate.ps1" %*
exit /b %errorlevel%
