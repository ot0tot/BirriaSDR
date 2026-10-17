# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SdrSharpDirectory,
    [string]$PackageDirectory,
    [switch]$Uninstall
)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
if (!$PackageDirectory) {
    $PackageDirectory=Join-Path $PSScriptRoot 'sdrsharp'
    if (!(Test-Path -LiteralPath $PackageDirectory)) { $PackageDirectory=Join-Path $repoRoot 'build-applications/sdrsharp' }
}
if (![IO.Path]::IsPathRooted($PackageDirectory)) { $PackageDirectory=Join-Path $repoRoot $PackageDirectory }
$SdrSharpDirectory=(Resolve-Path -LiteralPath $SdrSharpDirectory).Path
if (Get-Process -Name 'SDRSharp*' -ErrorAction SilentlyContinue) { throw 'Close SDR# before installing its source plugin.' }
$application=Join-Path $SdrSharpDirectory 'SDRSharp.dotnet8.exe'
$expectedHost='4FA94D895EAE710C86709122206E0153F2A43D797F0E6F16BBD861AF3E92DBD8'
if (!$Uninstall) {
    if (!(Test-Path -LiteralPath $application -PathType Leaf) -or
        (Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash -ne $expectedHost) {
        throw 'This package requires the inspected x64 .NET 8 SDR# 1921 executable (SDRSharp.dotnet8.exe). The receiver registration adapter uses a checked private host API. Other builds, including Next/Beta 1922, need separate validation. No files were changed.'
    }
    foreach ($file in 'SDRSharp.BirriaSDR.dll','birriasdr/birriasdr.dll','birriasdr/birriasdr-usb-1.0.dll') {
        if (!(Test-Path -LiteralPath (Join-Path $PackageDirectory $file))) { throw "Incomplete package: $file" }
    }
}
$config=Join-Path $SdrSharpDirectory 'SDRSharp.config'
[xml]$settings=Get-Content -LiteralPath $config -Raw
if (!$settings.configuration) { throw 'Unexpected SDR# settings format. No files were changed.' }
$pluginSetting=@($settings.configuration.add | Where-Object key -eq 'core.pluginsDirectory')
if($pluginSetting.Count -gt 1) { throw 'Duplicate core.pluginsDirectory settings. No files were changed.' }
$pluginRoot='Plugins'
if($pluginSetting.Count -eq 1) { $pluginRoot=$pluginSetting[0].value }
if([string]::IsNullOrWhiteSpace($pluginRoot) -or [IO.Path]::IsPathRooted($pluginRoot)) {
    throw 'The plugin directory must be relative to the SDR# application directory. No files were changed.'
}
$appPrefix=$SdrSharpDirectory.TrimEnd('\')+'\'
$plugins=[IO.Path]::GetFullPath((Join-Path $SdrSharpDirectory $pluginRoot))
if(!$plugins.StartsWith($appPrefix,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'The plugin directory must stay inside the SDR# application directory. No files were changed.'
}
$relative=$plugins.Substring($appPrefix.Length).Replace('\','/')+'/BirriaSDR'
$runtime=@("$relative/SDRSharp.BirriaSDR.dll","$relative/birriasdr/birriasdr.dll","$relative/birriasdr/birriasdr-usb-1.0.dll")
$record="$relative/birriasdr/installed-files.json"
$legacy=@('SDRSharp.BirriaSDR.dll','birriasdr/birriasdr.dll','birriasdr/birriasdr-usb-1.0.dll')
$legacyRecord='birriasdr/installed-files.json'
$frontends=Join-Path $SdrSharpDirectory 'FrontEnds.xml'
$xml=$null
if(Test-Path -LiteralPath $frontends) {
    [xml]$xml=Get-Content -LiteralPath $frontends -Raw
    if(!$xml.DocumentElement -or $xml.DocumentElement.Name -ne 'frontendPlugins') { throw 'Unexpected legacy FrontEnds.xml format. No files were changed.' }
}
$backup=Join-Path $SdrSharpDirectory "birriasdr-backup-$(Get-Date -Format yyyyMMdd-HHmmss-fff)"
New-Item -ItemType Directory $backup | Out-Null
$owned=@($runtime)+@($record)+@($legacy)+@($legacyRecord)+@('FrontEnds.xml','SDRSharp.config')
foreach($file in $owned) {
    $old=Join-Path $SdrSharpDirectory $file
    if(Test-Path -LiteralPath $old -PathType Leaf) {
        $destination=Join-Path $backup $file
        New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $old -Destination $destination
    }
}
function Remove-RecordedFiles([string]$relativeRecord,[string[]]$allowed) {
    $path=Join-Path $SdrSharpDirectory $relativeRecord
    if(!(Test-Path -LiteralPath $path)) { return }
    $hashes=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    foreach($file in $allowed) {
        $target=Join-Path $SdrSharpDirectory $file
        if(Test-Path -LiteralPath $target -PathType Leaf) {
            if($hashes.$file -eq (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) { Remove-Item -LiteralPath $target }
            else { Write-Warning "Retaining changed or unrecorded file: $file" }
        }
    }
    Remove-Item -LiteralPath $path
}
# These builds ignore FrontEnds.xml. Remove only our obsolete entry, preserving
# other receivers and the user's original file. Never create FrontEnds.xml.
if($xml) {
    $entries=@($xml.frontendPlugins.add | Where-Object value -eq 'SDRSharp.BirriaSDR.BirriaIO,SDRSharp.BirriaSDR')
    foreach($entry in $entries) { $xml.frontendPlugins.RemoveChild($entry) | Out-Null }
    if($entries.Count) { $xml.Save($frontends) }
}
Remove-RecordedFiles $legacyRecord $legacy
if($Uninstall) {
    Remove-RecordedFiles $record $runtime
    Write-Host "Source removed. Backup: $backup"
    return
}
$destination=Join-Path $SdrSharpDirectory $relative
$native=Join-Path $destination 'birriasdr'
New-Item -ItemType Directory -Force $native | Out-Null
Copy-Item -LiteralPath (Join-Path $PackageDirectory 'SDRSharp.BirriaSDR.dll') -Destination $destination
Copy-Item -Path (Join-Path $PackageDirectory 'birriasdr/*') -Destination $native
$hashes=@{}
foreach($file in $runtime) { $hashes[$file]=(Get-FileHash -LiteralPath (Join-Path $SdrSharpDirectory $file) -Algorithm SHA256).Hash }
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $SdrSharpDirectory $record) -Encoding utf8
Write-Host 'Installed registration plugin. Start SDRSharp.dotnet8.exe and select BirriaSDR (MT7961).'
Write-Host "Plugin directory: $destination"
Write-Host "Previous configuration and binaries: $backup"
