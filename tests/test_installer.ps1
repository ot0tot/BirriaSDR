# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SdrSharpDirectory,
    [Parameter(Mandatory)][string]$PackageDirectory
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$fixture=Join-Path $repo ('build-installer-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $fixture | Out-Null
$hostFile=Join-Path $fixture 'SDRSharp.dotnet8.exe'
Copy-Item -LiteralPath (Join-Path $SdrSharpDirectory 'SDRSharp.dotnet8.exe') -Destination $hostFile
$config=Join-Path $fixture 'SDRSharp.config'
'<configuration><add key="core.pluginsDirectory" value="Plugins" /></configuration>' | Set-Content -LiteralPath $config
$hostHash=(Get-FileHash -LiteralPath $hostFile).Hash
$configHash=(Get-FileHash -LiteralPath $config).Hash
& "$repo/tools/install-sdrsharp.ps1" -SdrSharpDirectory $fixture -PackageDirectory $PackageDirectory
$managed=Join-Path $fixture 'Plugins/BirriaSDR/SDRSharp.BirriaSDR.dll'
$native=Join-Path $fixture 'Plugins/BirriaSDR/birriasdr/birriasdr.dll'
if (!(Test-Path -LiteralPath $managed) -or !(Test-Path -LiteralPath $native)) { throw 'Missing installed source files.' }
if (Test-Path -LiteralPath (Join-Path $fixture 'FrontEnds.xml')) { throw 'Created obsolete FrontEnds.xml.' }
if ((Get-FileHash -LiteralPath $config).Hash -ne $configHash -or
    (Get-FileHash -LiteralPath $hostFile).Hash -ne $hostHash) { throw 'Changed host or configuration.' }
# A second install checks backup and update behavior without a legacy config.
& "$repo/tools/install-sdrsharp.ps1" -SdrSharpDirectory $fixture -PackageDirectory $PackageDirectory
Add-Content -LiteralPath $managed -Value 'changed file preservation check'
& "$repo/tools/install-sdrsharp.ps1" -SdrSharpDirectory $fixture -Uninstall
if (!(Test-Path -LiteralPath $managed) -or (Test-Path -LiteralPath $native)) { throw 'Uninstall ownership check failed.' }
$rejected=$false
Add-Content -LiteralPath $hostFile -Value 'unsupported host check'
try { & "$repo/tools/install-sdrsharp.ps1" -SdrSharpDirectory $fixture -PackageDirectory $PackageDirectory }
catch { $rejected=$_.Exception.Message.Contains('No files were changed') }
if (!$rejected) { throw 'Unsupported host was accepted.' }
Write-Host "Installer tests passed. Fixture retained at $fixture"
