#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Sdk = 'D:\ti\mspm0_sdk_2_10_00_04',
    [string]$Compiler = 'D:\ti\ccs2050\ccs\tools\compiler\ti-cgt-armllvm_4.0.4.LTS'
)

$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$Out = Join-Path $Here 'build'
$Generated = Join-Path (Split-Path (Split-Path $Here -Parent) -Parent) 'components\mspm0_loader\generated'
$Clang = Join-Path $Compiler 'bin\tiarmclang.exe'
$Objcopy = Join-Path $Compiler 'bin\tiarmobjcopy.exe'
$Nm = Join-Path $Compiler 'bin\tiarmnm.exe'
foreach ($tool in @($Clang, $Objcopy, $Nm)) {
    if (-not (Test-Path $tool)) { throw "Required TI tool not found: $tool" }
}
New-Item -ItemType Directory -Force $Out, $Generated | Out-Null

$common = @(
    '-D__MSPM0G3507__', '-mcpu=cortex-m0plus', '-march=thumbv6m',
    '-mfloat-abi=soft', '-mthumb', '-Oz', '-ffunction-sections', '-fdata-sections',
    '-fno-builtin', '-fno-exceptions', '-Wall', '-Wextra',
    "-I$Sdk\source", "-I$Sdk\source\third_party\CMSIS\Core\Include"
)
$loaderObj = Join-Path $Out 'loader.obj'
$flashObj = Join-Path $Out 'dl_flashctl.obj'
& $Clang @common -c (Join-Path $Here 'loader.c') -o $loaderObj
if ($LASTEXITCODE -ne 0) { throw 'loader.c compilation failed' }
& $Clang @common -c (Join-Path $Sdk 'source\ti\driverlib\dl_flashctl.c') -o $flashObj
if ($LASTEXITCODE -ne 0) { throw 'TI dl_flashctl.c compilation failed' }

$outFile = Join-Path $Out 'mspm0_loader.out'
$mapFile = Join-Path $Out 'mspm0_loader.map'
& $Clang '-nostdlib' '-mcpu=cortex-m0plus' '-march=thumbv6m' '-mthumb' `
    $loaderObj $flashObj (Join-Path $Here 'loader.cmd') `
    "-Wl,-m,$mapFile" '-Wl,--unused_section_elimination=on' `
    "-L$Compiler\lib" '-llibclang_rt.builtins.a' -o $outFile
if ($LASTEXITCODE -ne 0) { throw 'loader link failed' }

$binFile = Join-Path $Out 'mspm0_loader.bin'
& $Objcopy '-O' 'binary' $outFile $binFile
if ($LASTEXITCODE -ne 0) { throw 'loader binary extraction failed' }
$bytes = [System.IO.File]::ReadAllBytes($binFile)
if ($bytes.Length -eq 0 -or $bytes.Length -gt 8192) {
    throw "Loader size $($bytes.Length) is outside 1..8192 bytes"
}
$symbols = & $Nm '-n' '--defined-only' $outFile
$entryLine = $symbols | Where-Object { $_ -match '\bloader_entry$' } | Select-Object -First 1
if (-not $entryLine -or $entryLine -notmatch '^\s*([0-9A-Fa-f]+)') {
    throw 'loader_entry symbol was not found'
}
$entry = [Convert]::ToUInt32($Matches[1], 16)
if ($entry -lt 0x20200000 -or $entry -ge 0x20202000) {
    throw ('loader_entry address 0x{0:X8} is outside loader code region' -f $entry)
}
$sha = [System.Security.Cryptography.SHA256]::Create()
try { $hash = (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '') }
finally { $sha.Dispose() }

$header = @"
#pragma once
#include <stddef.h>
#include <stdint.h>
extern const uint8_t g_mspm0_loader_blob[];
extern const size_t g_mspm0_loader_blob_size;
extern const uint32_t g_mspm0_loader_entry;
extern const char g_mspm0_loader_sha256[];
"@
$rows = for ($offset = 0; $offset -lt $bytes.Length; $offset += 12) {
    $end = [Math]::Min($offset + 12, $bytes.Length)
    '    ' + (($bytes[$offset..($end - 1)] | ForEach-Object { '0x{0:X2}' -f $_ }) -join ', ') + ','
}
$source = @"
#include "mspm0_loader_blob.h"
const uint8_t g_mspm0_loader_blob[] = {
$($rows -join "`r`n")
};
const size_t g_mspm0_loader_blob_size = sizeof(g_mspm0_loader_blob);
const uint32_t g_mspm0_loader_entry = 0x$($entry.ToString('X8'))U;
const char g_mspm0_loader_sha256[] = "$hash";
"@
[System.IO.File]::WriteAllText((Join-Path $Generated 'mspm0_loader_blob.h'), $header,
                               [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path $Generated 'mspm0_loader_blob.c'), $source,
                               [System.Text.UTF8Encoding]::new($false))
Write-Host ("MSPM0 loader: {0} bytes, entry 0x{1:X8}, SHA-256 {2}" -f $bytes.Length, $entry, $hash)
