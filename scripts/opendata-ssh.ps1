<#
.SYNOPSIS
    PC/OpenData - PowerShell SSH Remote Invocation & Tunnel Helper
.DESCRIPTION
    Executes remote SQL commands over SSH or sets up a background secure tunnel
    to a remote PC/OpenData headless server instance.
.PARAMETER HostName
    The remote SSH target (e.g. user@remote-host).
.PARAMETER Query
    SQL query to execute remotely.
.PARAMETER Format
    Output format: json, csv, or table (default: json).
.PARAMETER Tunnel
    If specified, opens a persistent SSH port-forwarding tunnel.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$HostName,

    [Parameter(Position = 1)]
    [string]$Query,

    [ValidateSet('json', 'csv', 'table')]
    [string]$Format = 'json',

    [switch]$Tunnel,
    [ValidateRange(1, 65535)]
    [int]$LocalPort = 7337,
    [ValidateRange(1, 65535)]
    [int]$RemotePort = 7337
)

# Reject host names that could be interpreted as ssh options or contain invalid characters.
if ($HostName.StartsWith('-') -or $HostName -notmatch '^[a-zA-Z0-9_.-]+(@[a-zA-Z0-9_.-]+)?(:[0-9]+)?$') {
    throw "Invalid host name: must be a valid hostname or user@hostname format."
}

if ($Tunnel) {
    Write-Host "Creating SSH tunnel to PC/OpenData ($LocalPort -> ${HostName}:$RemotePort)..." -ForegroundColor Cyan
    ssh -N -L "$LocalPort`:127.0.0.1`:$RemotePort" -- $HostName
    return
}

if (-not $Query) {
    throw "Specify -Query '<SQL>' to execute remotely."
}

Write-Verbose "Executing remote query on $HostName..."
$quotedQuery = "'" + $Query.Replace("'", "'\''") + "'"
$remoteCmd = "opendata --database /data/production.pcod --format $Format --execute $quotedQuery"
# Pass the remote command as a single argument; '--' stops ssh option parsing.
$rawOutput = ssh -- $HostName $remoteCmd

if ($Format -eq 'json' -and $rawOutput) {
    try {
        return ($rawOutput | ConvertFrom-Json)
    } catch {
        return $rawOutput
    }
}

return $rawOutput
