# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([string]$BuildDirectory='build-applications', [string]$StandaloneDirectory='build-applications/standalone/BirriaSDR')
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory=Join-Path $repoRoot $BuildDirectory }
if (![IO.Path]::IsPathRooted($StandaloneDirectory)) { $StandaloneDirectory=Join-Path $repoRoot $StandaloneDirectory }
if (!(Test-Path -LiteralPath (Join-Path $StandaloneDirectory 'BirriaSDR.exe'))) { throw 'Build the standalone app first.' }
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$package=Join-Path $BuildDirectory "BirriaSDR-MT7961-Windows-$stamp"
if (Test-Path -LiteralPath $package) { throw 'Package directory already exists.' }
New-Item -ItemType Directory $package | Out-Null
Copy-Item -LiteralPath $StandaloneDirectory -Destination (Join-Path $package 'standalone') -Recurse
$sdr=Join-Path $BuildDirectory 'sdrsharp'
$sdrPackage=Join-Path $package 'sdrsharp'
$sdrNative=Join-Path $sdrPackage 'birriasdr'
New-Item -ItemType Directory -Force $sdrNative | Out-Null
# Ship runtime files only. Reference assemblies, test executables and build
# intermediates do not belong in a receiver installation.
Copy-Item -LiteralPath (Join-Path $sdr 'SDRSharp.BirriaSDR.dll') -Destination $sdrPackage
foreach($name in 'birriasdr.dll','birriasdr-usb-1.0.dll','COPYING.libusb','LICENSE','NOTICE','BSD-3-Clause-Clear.txt') {
    Copy-Item -LiteralPath (Join-Path $sdr "birriasdr/$name") -Destination $sdrNative
}
Copy-Item -LiteralPath (Join-Path $sdr 'birriasdr/libusb-1.0.30.tar.bz2') -Destination $package
$hasSoapy=Test-Path -LiteralPath (Join-Path $BuildDirectory 'Release/birriaSupport.dll')
if ($hasSoapy) {
$soapy=Join-Path $package 'soapy'
New-Item -ItemType Directory $soapy | Out-Null
foreach($name in 'birriaSupport.dll','birriasdr-usb-1.0.dll') {
    $file=Join-Path $BuildDirectory "Release/$name"
    if (!(Test-Path -LiteralPath $file)) { throw "Build the Soapy module first: $name is missing." }
    Copy-Item -LiteralPath $file -Destination $soapy
}
foreach($name in 'COPYING.libusb','LICENSE','NOTICE','BSD-3-Clause-Clear.txt') {
    Copy-Item -LiteralPath (Join-Path $sdr "birriasdr/$name") -Destination $soapy
}
}
$recorder=Join-Path $package 'recorder'
New-Item -ItemType Directory $recorder | Out-Null
foreach($name in 'birria_rx.exe','birriasdr.dll','birriasdr-usb-1.0.dll') {
    Copy-Item -LiteralPath (Join-Path $BuildDirectory "Release/$name") -Destination $recorder
}
foreach($name in 'COPYING.libusb','LICENSE','NOTICE','BSD-3-Clause-Clear.txt') {
    Copy-Item -LiteralPath (Join-Path $sdr "birriasdr/$name") -Destination $recorder
}
Copy-Item -LiteralPath "$PSScriptRoot/install-sdrsharp.ps1","$PSScriptRoot/start-soapy-app.ps1" -Destination $package
Copy-Item -LiteralPath "$repoRoot/docs" -Destination $package -Recurse
Copy-Item -LiteralPath "$repoRoot/LICENSE","$repoRoot/NOTICE" -Destination $package
@('@echo off','powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-sdrsharp.ps1" %*','if errorlevel 1 (pause & exit /b 1)') |
    Set-Content -LiteralPath (Join-Path $package 'install-sdrsharp.cmd') -Encoding ascii
$files=& git -C $repoRoot ls-files
if ($LASTEXITCODE) { throw 'Could not enumerate the current source tree.' }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$sourceZip=[IO.Compression.ZipFile]::Open((Join-Path $package 'birriasdr-source.zip'),[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach($file in $files) {
        if ($file.StartsWith('.agents/')) { continue }
        $path=Join-Path $repoRoot $file
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($sourceZip,$path,$file,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
} finally { $sourceZip.Dispose() }
@'
# BirriaSDR MT7961 receive package

Read docs/windows.md to install libusbK for WiFi_If (Interface 3) only.
Run standalone/BirriaSDR.exe for the included spectrum receiver.
Keep its _internal directory beside the executable.

Use the SDR# source with the checked x64 .NET 8 production build 1921.
Close SDR# and run this from the extracted directory:

```powershell
.\install-sdrsharp.cmd -SdrSharpDirectory "C:\SDRSharp"
```

Start SDRSharp.dotnet8.exe and select BirriaSDR (MT7961).
Read docs/applications.md for removal, build instructions and Soapy usage.
Read docs/tuning-research.md for measured frequencies and the 5/6 LO mapping.
Read docs/bandwidth-validation.md for gain, spurs and bandwidth limits.
Use 40 MSps first. The 80 MSps mode and wider tuning remain experimental.
The programming windows do not establish continuous RF coverage.
The archive includes source, dependency notices and a SHA-256 manifest.
'@ | Set-Content -LiteralPath (Join-Path $package 'README.md') -Encoding utf8
$metadata=@{
    schema=1
    platform='windows-x64'
    sdrsharp='.NET 8, actual host 1.0.0.1921, checked private registration adapter'
    sdrsharp_executable_sha256='4fa94d895eae710c86709122206e0153f2a43d797f0e6f16bbd861af3e92dbd8'
    soapy_abi='0.8'
    usb_runtime='1.0.30'
    usb_driver='libusbK, WiFi_If Interface 3 only'
    base_commit=(& git -C $repoRoot rev-parse HEAD)
    source_dirty=[bool](& git -C $repoRoot status --porcelain)
    soapy_included=[bool]$hasSoapy
}
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'package.json') -Encoding utf8
$manifest=@{}
Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
    $relative=$_.FullName.Substring($package.Length+1).Replace('\','/')
    $manifest[$relative]=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'sha256.json') -Encoding utf8
$zip="$package.zip"
[IO.Compression.ZipFile]::CreateFromDirectory($package,$zip,[IO.Compression.CompressionLevel]::Optimal,$true)
Write-Host "Package: $zip"
