[CmdletBinding()]
param(
    [string]$Compiler = 'clang',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Amalgamation,
    [switch]$WithoutTLS,
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$compilerCommand = Get-Command $Compiler -CommandType Application -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$compilerName = [System.IO.Path]::GetFileNameWithoutExtension($compilerPath)
if ($compilerName -notmatch '^(clang|gcc|cc)(-[0-9]+)?$') {
    throw 'Use a Clang or GCC C11 driver, not cl.exe or clang-cl.exe.'
}

$repoRoot = if (Test-Path (Join-Path $PSScriptRoot 'src')) { $PSScriptRoot } else { Split-Path -Parent $PSScriptRoot }
$sourceDirectory = Join-Path $repoRoot 'src'
$binaryDirectory = Join-Path $repoRoot 'bin'
if ((Test-Path -LiteralPath $binaryDirectory) -and
    -not (Test-Path -LiteralPath $binaryDirectory -PathType Container)) {
    throw "The binary output path is not a directory: $binaryDirectory"
}
[void][System.IO.Directory]::CreateDirectory($binaryDirectory)
. (Join-Path $repoRoot 'scripts\Publish-OpenDataRelease.ps1')
$buildLock = $null
$stage = $null
$published = $false
try {
try {
    $buildLock = [System.IO.File]::Open((Join-Path $binaryDirectory '.build.lock'),
        [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
}
catch {
    throw "Cannot acquire exclusive build lock; another build may be running: $($_.Exception.Message)"
}

if ($Amalgamation) {
    $amalgScript = Join-Path $PSScriptRoot 'amalgamate.ps1'
    & $amalgScript
    $sources = @(Join-Path $sourceDirectory 'opendata.c')
}
else {
    $names = @(Get-Content -LiteralPath (Join-Path $sourceDirectory 'modules.txt') |
        Where-Object { $_.Trim().Length -gt 0 })
    if ($names.Count -eq 0) {
        throw 'The module manifest is empty.'
    }
    $seen = @{}
    $sources = @(
        foreach ($name in $names) {
            if ($name -notmatch '^[a-z][a-z0-9_]*\.c$' -or $name -eq 'opendata.c' -or $seen.ContainsKey($name)) {
                throw "Invalid or duplicate module in manifest: $name"
            }
            $seen[$name] = $true
            $source = Join-Path $sourceDirectory $name
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                throw "Missing module source: $source"
            }
            $source
        }
    )
}

$isWindowsHost = $env:OS -eq 'Windows_NT'
$binaryName = if ($isWindowsHost) { 'opendata.exe' } else { 'opendata' }
$binary = Join-Path $binaryDirectory $binaryName
$pythonCmd = Get-Command python -CommandType Application -ErrorAction SilentlyContinue
if (-not $pythonCmd) { $pythonCmd = Get-Command py -CommandType Application -ErrorAction SilentlyContinue }
$pythonPath = if ($pythonCmd) { $pythonCmd.Source } else { $null }
$stage = Join-Path $binaryDirectory ('.build-' + [Guid]::NewGuid().ToString('N'))
[void][System.IO.Directory]::CreateDirectory($stage)
$candidate = Join-Path $stage $binaryName
    $sourceHashes = @(
        foreach ($source in @((Join-Path $sourceDirectory 'database_engine.h')) + $sources) {
            [ordered]@{
                name = [System.IO.Path]::GetFileName($source)
                sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
            }
        }
    )
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Wconversion',
        '-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes', '-Werror', '-g'
    )
    $tlsLibraries = @()
    $cryptoLibraries = @()
    if ($isWindowsHost) { $cryptoLibraries += '-lbcrypt' }
    else { $cryptoLibraries += '-lcrypto' }
    if (-not $WithoutTLS) {
        $arguments += '-DOPENDATA_TLS'
        if ($isWindowsHost) {
            $tlsLibraries = @('-lcrypt32')
        } else {
            $tlsLibraries = @('-lssl')
        }
    }
    if ($Configuration -eq 'Release') { $arguments += '-O2' }
    if ($isWindowsHost) { $arguments += @('-lws2_32', '-lsecur32', '-ladvapi32', '-lwldap32') }
    elseif ($Configuration -eq 'Debug') { $arguments += '-O0' }
    if ($isWindowsHost -and $compilerName -match '^clang') {
        $arguments += @('-gcodeview', '-Xlinker', '/DEBUG', '-Xlinker',
            ("/PDB:" + (Join-Path $stage 'opendata.pdb')))
    }
    elseif (-not $isWindowsHost) {
        $arguments += '-pthread'
    }

    $libraries = @()
    if (-not $isWindowsHost) { $libraries += '-lm' }
    & $compilerPath @arguments @sources @tlsLibraries @cryptoLibraries @libraries '-o' $candidate
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed with exit code $LASTEXITCODE."
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "The compiler did not produce $candidate."
    }
    if (-not $SkipTests) {
        $testExit = 0
        & $candidate self-test
        $testExit = $LASTEXITCODE
        if ($testExit -ne 0) {
            throw "Release validation failed: built-in self-test (exit code $testExit). Existing executable was not replaced."
        }
    }
    $compilerVersion = & $compilerPath --version
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record compiler version.' }
    foreach ($entry in $sourceHashes) {
        if ((Get-FileHash -LiteralPath (Join-Path $sourceDirectory $entry.name) -Algorithm SHA256).Hash -ne $entry.sha256) {
            throw "Source changed during build: $($entry.name). Existing release was not replaced."
        }
    }
    $metadata = [ordered]@{
        compiler = $compilerPath
        compilerVersion = ($compilerVersion -join "`n")
        configuration = $Configuration
        tls = (-not $WithoutTLS)
        amalgamation = [bool]$Amalgamation
        builtUtc = [DateTime]::UtcNow.ToString('o')
        binarySha256 = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
        sources = $sourceHashes
        compileArguments = $arguments
    }
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'opendata.build.json') -Encoding UTF8
    $namesToPublish = @($binaryName, 'opendata.build.json')
    if (Test-Path -LiteralPath (Join-Path $stage 'opendata.pdb') -PathType Leaf) {
        $namesToPublish += 'opendata.pdb'
    }
    Publish-OpenDataRelease -Stage $stage -Destination $binaryDirectory -Names $namesToPublish
    $published = $true
    Write-Host "Built $binary ($Configuration; amalgamation=$Amalgamation; TLS=$(-not $WithoutTLS))."
}
finally {
    try {
        if ($stage -and (Test-Path -LiteralPath $stage -PathType Container)) {
            $recoveryFiles = @(Get-ChildItem -LiteralPath $stage -Filter '*.previous' -File)
            if ($published -or $recoveryFiles.Count -eq 0) {
                Remove-Item -LiteralPath $stage -Recurse -Force
            } else {
                Write-Warning "Release recovery files retained at $stage."
            }
        }
    }
    finally {
        if ($buildLock) { $buildLock.Dispose() }
    }
}
