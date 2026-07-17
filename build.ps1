#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Gateway', 'Probe', 'All')]
    [string]$Role = 'All',
    [switch]$Setup,
    [switch]$Clean,
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port,
    [int]$Baud = 921600
)

$ErrorActionPreference = 'Stop'
$ProjectDir = $PSScriptRoot
$IdfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { 'D:\Espressif\.espressif\v6.0\esp-idf' }
if (-not $env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH = 'C:\Espressif' }
$SecretsPath = Join-Path $ProjectDir 'secrets.local.json'
$GeneratedDir = Join-Path $ProjectDir 'generated\include'

function Invoke-Checked([string]$Description, [scriptblock]$Command) {
    Write-Host "== $Description ==" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

function New-HexKey([int]$Length) {
    $bytes = New-Object byte[] $Length
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Convert-HexBytes([string]$Hex, [int]$ExpectedLength) {
    $clean = $Hex.Replace(':', '').Replace('-', '').ToLowerInvariant()
    if ($clean.Length -ne ($ExpectedLength * 2) -or $clean -notmatch '^[0-9a-f]+$') {
        throw "Expected $ExpectedLength bytes of hexadecimal data, got '$Hex'"
    }
    $items = for ($i = 0; $i -lt $clean.Length; $i += 2) {
        '0x' + $clean.Substring($i, 2)
    }
    return ($items -join ', ')
}

function Initialize-Secrets {
    if (-not (Test-Path $SecretsPath)) {
        $secrets = [ordered]@{
            pmk = New-HexKey 16
            lmk = New-HexKey 16
            gateway_mac = '14:c1:9f:cd:33:4c'
            probe_mac = '14:c1:9f:cc:80:5c'
        }
        $secrets | ConvertTo-Json | Set-Content -Encoding UTF8 $SecretsPath
        Write-Host "Generated untracked secrets: $SecretsPath" -ForegroundColor Yellow
    }

    $s = Get-Content -Raw $SecretsPath | ConvertFrom-Json
    New-Item -ItemType Directory -Force $GeneratedDir | Out-Null
    $header = @"
#pragma once
#include <stdint.h>

static const uint8_t DATLINK_GENERATED_PMK[16] = { $(Convert-HexBytes $s.pmk 16) };
static const uint8_t DATLINK_GENERATED_LMK[16] = { $(Convert-HexBytes $s.lmk 16) };
static const uint8_t DATLINK_GENERATED_GATEWAY_MAC[6] = { $(Convert-HexBytes $s.gateway_mac 6) };
static const uint8_t DATLINK_GENERATED_PROBE_MAC[6] = { $(Convert-HexBytes $s.probe_mac 6) };
"@
    Set-Content -Encoding UTF8 (Join-Path $GeneratedDir 'datlink_secrets.h') $header
    # CMake treats backslashes in component arguments as escape characters.
    $env:DATLINK_GENERATED_DIR = $GeneratedDir.Replace('\', '/')
}

if (-not (Test-Path (Join-Path $IdfPath 'export.ps1'))) {
    throw "ESP-IDF not found at $IdfPath"
}

$PythonEnv = Join-Path $env:IDF_TOOLS_PATH 'python_env\idf6.0_py3.14_env\Scripts\python.exe'
if ($Setup -or -not (Test-Path $PythonEnv)) {
    $oldLocation = Get-Location
    try {
        Set-Location $IdfPath
        Invoke-Checked 'ESP-IDF setup for esp32s3' { & (Join-Path $IdfPath 'install.ps1') esp32s3 }
    } finally {
        Set-Location $oldLocation
    }
}

. (Join-Path $IdfPath 'export.ps1') | Out-Null
$env:IDF_PATH = $IdfPath
$env:IDF_TARGET = 'esp32s3'
Initialize-Secrets

$Roles = if ($Role -eq 'All') { @('Gateway', 'Probe') } else { @($Role) }

# ESP-IDF discovers all local components even for a Gateway build, so the
# generated loader component must exist before configuring either role.
$loaderScript = Join-Path $ProjectDir 'tools\mspm0_loader\build_loader.ps1'
if (Test-Path $loaderScript) {
    Invoke-Checked 'Build MSPM0 SRAM loader' { & $loaderScript }
}

foreach ($CurrentRole in $Roles) {
    $roleName = $CurrentRole.ToLowerInvariant()
    $buildDir = Join-Path $ProjectDir "build\$roleName"
    $sdkconfigPath = Join-Path $buildDir 'sdkconfig'
    $defaults = "$(Join-Path $ProjectDir 'sdkconfig.defaults');$(Join-Path $ProjectDir "sdkconfig.defaults.$roleName")"

    if ($Clean -and (Test-Path $buildDir)) {
        $resolved = [System.IO.Path]::GetFullPath($buildDir)
        $buildRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir 'build'))
        if (-not $resolved.StartsWith($buildRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean unexpected directory: $resolved"
        }
        Remove-Item -Recurse -Force -LiteralPath $resolved
    }

    $env:DATLINK_ROLE_NAME = $roleName
    $idf = Join-Path $IdfPath 'tools\idf.py'
    Invoke-Checked "Build $CurrentRole" {
        & python $idf -B $buildDir "-DSDKCONFIG=$sdkconfigPath" "-DSDKCONFIG_DEFAULTS=$defaults" build
    }

    if ($Flash) {
        $flashPort = if ($Port) { $Port } elseif ($roleName -eq 'gateway') { 'COM8' } else { 'COM7' }
        Invoke-Checked "Flash $CurrentRole on $flashPort" {
            & python $idf -B $buildDir -p $flashPort -b $Baud flash
        }
    }

    if ($Monitor) {
        $monitorPort = if ($Port) { $Port } elseif ($roleName -eq 'gateway') { 'COM8' } else { 'COM7' }
        Invoke-Checked "Monitor $CurrentRole on $monitorPort" {
            & python $idf -B $buildDir -p $monitorPort monitor
        }
    }
}
