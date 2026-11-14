@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
if "%~1"=="" (
    echo.
    pause
)
exit /b %EXIT_CODE%
