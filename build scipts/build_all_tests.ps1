[CmdletBinding()]
param(
    [string]$TestName = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$mods = @(Get-Content -LiteralPath 'src/modules.txt' | Where-Object { $_.Trim().Length -gt 0 -and $_ -ne 'opendata_main.c' } | ForEach-Object { "src/$_" })
$tlsFlags = @('-DOPENDATA_TLS', '-lcrypt32')

$tests = if ($TestName -ne '') {
    @(Join-Path 'tests' "$TestName.c")
} else {
    @(Get-ChildItem -Path 'tests' -Filter '*.c' | ForEach-Object { $_.FullName })
}

foreach ($test in $tests) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($test)
    $out = "bin/$name.exe"
    Write-Host "Compiling $name -> $out..."
    & clang -std=c11 -Wall -Wextra -Iinclude -Isrc $test $mods @tlsFlags -lws2_32 -lsecur32 -lbcrypt -o $out
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to compile $name"
    }
}
Write-Host "Done building tests."
