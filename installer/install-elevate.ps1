<#
.SYNOPSIS
    Helper for install.bat self-elevation. Re-launches the batch file elevated.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ScriptPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ScriptPath)) {
    Write-Error "Installer script not found: $ScriptPath"
    exit 1
}

$proc = Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c `"' + $ScriptPath + '`"') -Verb RunAs -Wait -PassThru
exit $proc.ExitCode
