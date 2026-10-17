# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [string]$ModuleDirectory,
    [string]$SoapyRoot = 'C:\Program Files\PothosSDR',
    [Parameter(ValueFromRemainingArguments)][string[]]$ApplicationArguments
)
$ErrorActionPreference='Stop'
if(!$ModuleDirectory) {
    $ModuleDirectory=Join-Path $PSScriptRoot 'soapy'
    if(!(Test-Path -LiteralPath $ModuleDirectory)) { $ModuleDirectory=Join-Path (Split-Path -Parent $PSScriptRoot) 'build-applications/Release' }
}
$ModuleDirectory=(Resolve-Path -LiteralPath $ModuleDirectory).Path
if(!(Test-Path -LiteralPath (Join-Path $ModuleDirectory 'birriaSupport.dll'))) { throw 'Choose the directory containing birriaSupport.dll.' }
$savedPath=$env:PATH
$savedModules=$env:SOAPY_SDR_PLUGIN_PATH
try {
    $env:SOAPY_SDR_PLUGIN_PATH=$ModuleDirectory+$(if($savedModules){";$savedModules"})
    $env:PATH="$ModuleDirectory;$(Join-Path $SoapyRoot 'bin');$savedPath"
    & $Executable @ApplicationArguments
    if($LASTEXITCODE) { throw "Application exited with code $LASTEXITCODE." }
} finally {
    $env:PATH=$savedPath
    $env:SOAPY_SDR_PLUGIN_PATH=$savedModules
}
