$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '..\scripts\Publish-OpenDataRelease.ps1')
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('opendata-publish-' + [Guid]::NewGuid().ToString('N'))
[void][System.IO.Directory]::CreateDirectory($root)
try {
    $stage = Join-Path $root 'stage'
    $destination = Join-Path $root 'installed'
    [void][System.IO.Directory]::CreateDirectory($stage)
    [void][System.IO.Directory]::CreateDirectory($destination)
    foreach ($name in @('opendata.exe', 'opendata.build.json', 'opendata.pdb')) {
        [System.IO.File]::WriteAllText((Join-Path $stage $name), 'new')
        [System.IO.File]::WriteAllText((Join-Path $destination $name), 'old')
    }
    $blocked = [System.IO.File]::Open((Join-Path $destination 'opendata.pdb'),
        [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    $failed = $false
    try {
        Publish-OpenDataRelease $stage $destination @('opendata.exe', 'opendata.build.json', 'opendata.pdb')
    }
    catch {
        if ($_.Exception.Message -notlike '*previous release restored*') { throw }
        $failed = $true
    }
    finally { $blocked.Dispose() }
    if (-not $failed) { throw 'Expected publication to fail on locked symbols.' }
    foreach ($name in @('opendata.exe', 'opendata.build.json', 'opendata.pdb')) {
        if ([System.IO.File]::ReadAllText((Join-Path $destination $name)) -ne 'old') {
            throw "Rollback did not restore $name"
        }
        [System.IO.File]::WriteAllText((Join-Path $stage $name), 'new')
    }
    Publish-OpenDataRelease $stage $destination @('opendata.exe', 'opendata.build.json', 'opendata.pdb')
    foreach ($name in @('opendata.exe', 'opendata.build.json', 'opendata.pdb')) {
        if ([System.IO.File]::ReadAllText((Join-Path $destination $name)) -ne 'new') {
            throw "Publication did not install $name"
        }
    }
    Write-Host 'Release publication rollback and success tests passed.'
}
finally { Remove-Item -LiteralPath $root -Recurse -Force }
