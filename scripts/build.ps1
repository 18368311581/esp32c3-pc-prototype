param(
    [ValidateSet('standalone', 'device', 'receiver')]
    [string]$Role = 'standalone'
)
$ErrorActionPreference = 'Stop'
if (-not $env:IDF_PATH -or -not (Test-Path (Join-Path $env:IDF_PATH 'tools/idf.py'))) {
    throw 'Activate ESP-IDF v6.0.2 before running this script.'
}
$firmwareDir = Join-Path $PSScriptRoot '../hello_world'
Push-Location $firmwareDir
try {
    $buildDir = Join-Path (Get-Location).Path "build-$Role"
    $configFile = Join-Path $buildDir 'sdkconfig'
    & python (Join-Path $env:IDF_PATH 'tools/idf.py') -B $buildDir `
        "-DSDKCONFIG=$configFile" '-DIDF_TARGET=esp32c3' "-DAPP_ROLE=$Role" build
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF build failed for $Role ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
