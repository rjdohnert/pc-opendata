<#
.SYNOPSIS
    PC/OpenData - WinRM (Windows Remote Management) Execution Bridge
.DESCRIPTION
    Executes SQL commands on remote Windows servers running PC/OpenData over
    native WinRM / WS-Management. Deserializes JSON results into native PowerShell objects.
.PARAMETER ComputerName
    The target Windows machine (IP address or NetBIOS/FQDN).
.PARAMETER DatabasePath
    Path to the database on the remote system (default: C:\Data\production.pcod).
.PARAMETER Query
    The SQL query to execute.
.PARAMETER Credential
    Optional PSCredential for authentication over WinRM.
.PARAMETER Format
    Output format: json, csv, or table (default: json).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$ComputerName,

    [Parameter(Mandatory = $true, Position = 1)]
    [string]$Query,

    [string]$DatabasePath = "C:\Data\production.pcod",
    [pscredential]$Credential,
    [ValidateSet('json', 'csv', 'table')]
    [string]$Format = 'json'
)

$invokeParams = @{
    ComputerName = $ComputerName
    ScriptBlock  = {
        param($db, $sql, $fmt)
        & opendata.exe --database $db --format $fmt --execute $sql
    }
    ArgumentList = @($DatabasePath, $Query, $Format)
}

if ($Credential) {
    $invokeParams['Credential'] = $Credential
}

Write-Verbose "Connecting to $ComputerName via WinRM..."
$result = Invoke-Command @invokeParams

if ($Format -eq 'json' -and $result) {
    try {
        $jsonStr = ($result -join "`n").Trim()
        return ($jsonStr | ConvertFrom-Json)
    } catch {
        return $result
    }
}

return $result
