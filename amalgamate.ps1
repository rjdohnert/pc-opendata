[CmdletBinding()]
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$targetScript = Join-Path $PSScriptRoot 'build-scripts\amalgamate.ps1'
if (-not (Test-Path $targetScript)) {
    throw "amalgamate.ps1 not found in build-scripts."
}
& $targetScript -Check:$Check
