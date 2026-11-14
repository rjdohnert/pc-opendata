<#
.SYNOPSIS
    Installs PC/OpenData SQL Server as a system-wide application to C:\Program Files\OpenDataSQL\bin and updates the System PATH.
.DESCRIPTION
    PC/OpenData SQL Server is a system-wide application.
    Installs to C:\Program Files\OpenDataSQL\bin and configures the Machine PATH for all users.
    NOTE: There is NO local user install. Requires Administrator elevation.
#>

[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\Program Files\OpenDataSQL\bin'
)

$ErrorActionPreference = 'Stop'

# Enforce Administrator privileges for system-wide installation
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "==============================================================" -ForegroundColor Yellow
    Write-Host "Administrative privileges are required for system-wide install." -ForegroundColor Yellow
    Write-Host "Target: $InstallDir" -ForegroundColor White
    Write-Host "Requesting UAC elevation..." -ForegroundColor Cyan
    Write-Host "==============================================================" -ForegroundColor Yellow
    
    $scriptPath = $PSCommandPath
    if (-not $scriptPath) { $scriptPath = $MyInvocation.MyCommand.Definition }
    if (-not $scriptPath) { $scriptPath = Join-Path $PSScriptRoot 'install.ps1' }

    # Use Base64 EncodedCommand to ensure complete immunity from spaces in file paths
    $elevateCmd = "& '$scriptPath' -InstallDir '$InstallDir'"
    $bytes = [System.Text.Encoding]::Unicode.GetBytes($elevateCmd)
    $encoded = [Convert]::ToBase64String($bytes)
    
    try {
        $proc = Start-Process -FilePath "powershell.exe" -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded) -Verb RunAs -Wait -PassThru
        if ($proc.ExitCode -eq 0) {
            Write-Host "`nSystem-wide installation completed successfully!" -ForegroundColor Green
        } else {
            Write-Error "Elevated installation exited with code $($proc.ExitCode)."
        }
        exit $proc.ExitCode
    } catch {
        Write-Error "Elevation failed or was declined by user: $_"
        exit 1
    }
}

# Running as Administrator: perform system-wide installation
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$candidates = @(
    (Join-Path $scriptDir 'opendata.exe'),
    (Join-Path (Split-Path -Parent $scriptDir) 'bin\opendata.exe'),
    (Join-Path $scriptDir 'bin\opendata.exe'),
    (Join-Path (Get-Location) 'bin\opendata.exe')
)

$sourceExe = $null
foreach ($cand in $candidates) {
    if ($cand -and (Test-Path -LiteralPath $cand -PathType Leaf)) {
        $sourceExe = (Resolve-Path -LiteralPath $cand).Path
        break
    }
}

if (-not $sourceExe) {
    Write-Error "Source executable 'opendata.exe' not found. Please compile the project first using .\build.ps1."
    exit 1
}

Write-Host "Found binary: $sourceExe" -ForegroundColor Cyan

# Create C:\Program Files\OpenDataSQL\bin
if (-not (Test-Path -LiteralPath $InstallDir)) {
    Write-Host "Creating directory: $InstallDir" -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}

# Copy opendata.exe and pdb
$destExe = Join-Path $InstallDir 'opendata.exe'
Write-Host "Copying binary to $destExe..." -ForegroundColor Green
Copy-Item -LiteralPath $sourceExe -Destination $destExe -Force

$sourcePdb = [System.IO.Path]::ChangeExtension($sourceExe, '.pdb')
if (Test-Path -LiteralPath $sourcePdb -PathType Leaf) {
    Copy-Item -LiteralPath $sourcePdb -Destination (Join-Path $InstallDir 'opendata.pdb') -Force
}

# Copy OpenDataSQLtest.exe diagnostic tool
$testCandidates = @(
    (Join-Path $scriptDir 'OpenDataSQLtest.exe'),
    (Join-Path $scriptDir 'test.exe'),
    (Join-Path (Split-Path -Parent $scriptDir) 'bin\OpenDataSQLtest.exe'),
    (Join-Path (Split-Path -Parent $scriptDir) 'installer\OpenDataSQLtest.exe'),
    (Join-Path (Get-Location) 'bin\OpenDataSQLtest.exe'),
    (Join-Path (Get-Location) 'installer\OpenDataSQLtest.exe')
)

$sourceTestExe = $null
foreach ($cand in $testCandidates) {
    if ($cand -and (Test-Path -LiteralPath $cand -PathType Leaf)) {
        $sourceTestExe = (Resolve-Path -LiteralPath $cand).Path
        break
    }
}

if ($sourceTestExe) {
    $destTestExe = Join-Path $InstallDir 'OpenDataSQLtest.exe'
    Write-Host "Copying diagnostic tool to $destTestExe..." -ForegroundColor Green
    Copy-Item -LiteralPath $sourceTestExe -Destination $destTestExe -Force
}

# Update System (Machine) PATH for all users
$envTarget = [System.EnvironmentVariableTarget]::Machine
$currentPath = [System.Environment]::GetEnvironmentVariable('Path', $envTarget)
$pathEntries = if ($currentPath) { $currentPath -split ';' | ForEach-Object { $_.Trim().TrimEnd('\') } } else { @() }

if ($pathEntries -notcontains $InstallDir.TrimEnd('\')) {
    Write-Host "Adding '$InstallDir' to System PATH (Machine)..." -ForegroundColor Cyan
    $newPath = if ($currentPath) { "$currentPath;$InstallDir" } else { $InstallDir }
    [System.Environment]::SetEnvironmentVariable('Path', $newPath, $envTarget)
    $env:Path = "$env:Path;$InstallDir"
    Write-Host "System PATH updated successfully." -ForegroundColor Green
} else {
    Write-Host "'$InstallDir' is already present in System PATH." -ForegroundColor Yellow
}

# Clean up any legacy local AppData path from User PATH if found
try {
    $userPath = [System.Environment]::GetEnvironmentVariable('Path', [System.EnvironmentVariableTarget]::User)
    if ($userPath) {
        $staleAppdata = (Join-Path $env:LOCALAPPDATA 'Programs\OpenDataSQL\bin').TrimEnd('\')
        $userParts = $userPath -split ';'
        $cleanedUserParts = $userParts | Where-Object { $_.Trim().TrimEnd('\') -ne $staleAppdata -and $_.Trim().Length -gt 0 }
        if ($cleanedUserParts.Count -ne $userParts.Count) {
            $newCleanedUserPath = $cleanedUserParts -join ';'
            [System.Environment]::SetEnvironmentVariable('Path', $newCleanedUserPath, [System.EnvironmentVariableTarget]::User)
            Write-Host "Removed legacy local AppData entry from User PATH." -ForegroundColor Cyan
        }
    }
} catch {
    # Non-critical if user env cleanup cannot be read
}

Write-Host ""
Write-Host "==============================================================" -ForegroundColor Green
Write-Host "PC/OpenData SQL Server installed successfully (System-Wide)!" -ForegroundColor Green
Write-Host "Target Directory : $InstallDir" -ForegroundColor White
Write-Host "Installed Files  :" -ForegroundColor White
Write-Host "  - opendata.exe (SQL Server Engine & Interactive Shell)" -ForegroundColor White
if ($sourceTestExe) {
    Write-Host "  - OpenDataSQLtest.exe (System & Engine Diagnostic Suite)" -ForegroundColor White
}
Write-Host "System PATH      : Configured for all users" -ForegroundColor White
Write-Host "Scope            : Machine-Wide (NO LOCAL USER INSTALL)" -ForegroundColor White
Write-Host "==============================================================" -ForegroundColor Green
Write-Host ""
Write-Host "You're ready! Open a new command prompt and type:" -ForegroundColor Cyan
Write-Host "  opendata mydata.pcod" -ForegroundColor White
Write-Host ""
Write-Host "Then try: CREATE TABLE friends (id NUMBER PRIMARY KEY, name VARCHAR2(50));" -ForegroundColor DarkGray
Write-Host "See the full guide: user_guide.md" -ForegroundColor DarkGray
