[CmdletBinding()]
param(
    [string]$Compiler = 'clang',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Amalgamation,
    [switch]$WithoutTLS,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$targetScript = Join-Path $PSScriptRoot 'build-scripts\build.ps1'
if (-not (Test-Path $targetScript)) {
    throw "build.ps1 not found in build-scripts."
}
& $targetScript -Compiler $Compiler -Configuration $Configuration -Amalgamation:$Amalgamation -WithoutTLS:$WithoutTLS -SkipTests:$SkipTests
