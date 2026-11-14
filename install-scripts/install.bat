@echo off
setlocal EnableDelayedExpansion

:: Check for Administrative privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrative privileges are required to install PC/OpenData SQL Server system-wide.
    echo Requesting UAC elevation...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process cmd.exe -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs -Wait"
    exit /b %ERRORLEVEL%
)

set "TARGET_DIR=C:\Program Files\OpenDataSQL\bin"
set "SOURCE_EXE=%~dp0opendata.exe"

if not exist "%SOURCE_EXE%" (
    set "SOURCE_EXE=%~dp0..\bin\opendata.exe"
)

if not exist "%SOURCE_EXE%" (
    set "SOURCE_EXE=%~dp0bin\opendata.exe"
)

if not exist "%SOURCE_EXE%" (
    set "SOURCE_EXE=%CD%\bin\opendata.exe"
)

if not exist "%SOURCE_EXE%" (
    echo [ERROR] Source executable not found: %SOURCE_EXE%
    echo Please compile the project first using build.bat or build.ps1.
    pause
    exit /b 1
)

:: Create target installation directory
if not exist "%TARGET_DIR%" (
    echo Creating installation directory: "%TARGET_DIR%"
    mkdir "%TARGET_DIR%"
    if %errorlevel% neq 0 (
        echo [ERROR] Failed to create "%TARGET_DIR%".
        pause
        exit /b 1
    )
)

:: Copy opendata.exe
echo Installing opendata.exe to "%TARGET_DIR%"...
copy /Y "%SOURCE_EXE%" "%TARGET_DIR%\opendata.exe" >nul
if %errorlevel% neq 0 (
    echo [ERROR] Failed to copy opendata.exe to "%TARGET_DIR%".
    pause
    exit /b 1
)

:: Copy PDB if present
set "SOURCE_PDB=%~dp0..\bin\opendata.pdb"
if not exist "%SOURCE_PDB%" set "SOURCE_PDB=%~dp0bin\opendata.pdb"
if exist "%SOURCE_PDB%" (
    copy /Y "%SOURCE_PDB%" "%TARGET_DIR%\opendata.pdb" >nul
)

:: Copy OpenDataSQLtest.exe if present
set "SOURCE_TEST=%~dp0OpenDataSQLtest.exe"
if not exist "%SOURCE_TEST%" set "SOURCE_TEST=%~dp0test.exe"
if not exist "%SOURCE_TEST%" set "SOURCE_TEST=%~dp0..\bin\OpenDataSQLtest.exe"
if not exist "%SOURCE_TEST%" set "SOURCE_TEST=%CD%\bin\OpenDataSQLtest.exe"

if exist "%SOURCE_TEST%" (
    echo Installing OpenDataSQLtest.exe to "%TARGET_DIR%"...
    copy /Y "%SOURCE_TEST%" "%TARGET_DIR%\OpenDataSQLtest.exe" >nul
)

:: Update System PATH using PowerShell environment API
echo Updating System PATH for all users...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$target = 'C:\Program Files\OpenDataSQL\bin';" ^
    "$mPath = [System.Environment]::GetEnvironmentVariable('Path', 'Machine');" ^
    "$parts = if ($mPath) { $mPath -split ';' | ForEach-Object { $_.Trim().TrimEnd('\') } } else { @() };" ^
    "if ($parts -notcontains $target.TrimEnd('\')) {" ^
    "    $new = if ($mPath) { \"$mPath;$target\" } else { $target };" ^
    "    [System.Environment]::SetEnvironmentVariable('Path', $new, 'Machine');" ^
    "    Write-Host 'System PATH updated successfully.';" ^
    "} else {" ^
    "    Write-Host 'Target directory already present in System PATH.';" ^
    "}"

echo.
echo ==============================================================
echo PC/OpenData SQL Server installed successfully (System-Wide)!
echo Target: %TARGET_DIR%
echo Installed:
echo   - opendata.exe (SQL Server Engine)
echo   - OpenDataSQLtest.exe (Diagnostic Suite)
echo System PATH: Configured for all users
echo Scope: Machine-Wide (NO LOCAL USER INSTALL)
echo.
echo You're ready! Open a new command prompt and type:
echo   opendata mydata.pcod
echo.
echo Then try: CREATE TABLE friends (id NUMBER PRIMARY KEY, name VARCHAR2(50^));
echo See the full guide: user_guide.md
echo ==============================================================
echo.
pause
