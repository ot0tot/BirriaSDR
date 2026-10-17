# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SdrSharpDirectory,
    [string]$Python = 'python',
    [string]$BuildDirectory = 'build-applications',
    [string]$SoapyRoot
)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory=Join-Path $repoRoot $BuildDirectory }
$SdrSharpDirectory=(Resolve-Path -LiteralPath $SdrSharpDirectory).Path
$application=Join-Path $SdrSharpDirectory 'SDRSharp.dotnet8.exe'
if (!(Test-Path -LiteralPath $application) -or
    (Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash -ne '4FA94D895EAE710C86709122206E0153F2A43D797F0E6F16BBD861AF3E92DBD8') {
    throw 'The receiver registration adapter currently supports the inspected x64 .NET 8 SDR# 1921 executable only. Other host builds require adapter validation.'
}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Install Visual Studio C++ Build Tools and CMake.' }
$cmake=Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$msvc=Get-ChildItem -Directory (Join-Path $vsRoot 'VC\Tools\MSVC') | Sort-Object Name -Descending | Select-Object -First 1
$compilerBin=Join-Path $msvc.FullName 'bin\Hostx64\x64'
$dependencyBuild=Join-Path $repoRoot 'build-windows'
$archive=Join-Path $dependencyBuild 'deps\libusb-1.0.30.7z'
if (!(Test-Path -LiteralPath $archive)) { & "$PSScriptRoot/build-windows.ps1" -BuildDirectory $dependencyBuild -SkipTests }
$expected='7FB1DFEC805B97983763D7D0AE244320DA12ADD1003D4249C96CC4D586398C79'
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'Unexpected libusb archive hash.' }
$deps=Join-Path $BuildDirectory 'deps'
New-Item -ItemType Directory -Force $deps | Out-Null
tar -xf $archive -C $deps VS2022/MS64/dll/libusb-1.0.dll include/libusb.h
if ($LASTEXITCODE) { throw 'Could not extract the pinned libusb dependency.' }
$copying=Join-Path $deps 'COPYING'
if (!(Test-Path -LiteralPath $copying)) {
    Invoke-WebRequest 'https://raw.githubusercontent.com/libusb/libusb/v1.0.30/COPYING' -OutFile $copying
}
if ((Get-FileHash -LiteralPath $copying -Algorithm SHA256).Hash -ne '5DF07007198989C622F5D41DE8D703E7BEF3D0E79D62E24332EE739A452AF62A') { throw 'Unexpected libusb license hash.' }
$usbSource=Join-Path $deps 'libusb-1.0.30.tar.bz2'
if (!(Test-Path -LiteralPath $usbSource)) {
    Invoke-WebRequest 'https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2' -OutFile $usbSource
}
if ((Get-FileHash -LiteralPath $usbSource -Algorithm SHA256).Hash -ne 'FEA36F34F9156400209595E300840767AB1A385EDE1DC7EE893015AEA9C6DBAF') { throw 'Unexpected libusb source archive hash.' }
$usb=Join-Path $deps 'birriasdr-usb-1.0.dll'
Copy-Item -LiteralPath (Join-Path $deps 'VS2022\MS64\dll\libusb-1.0.dll') -Destination $usb
$exports=& (Join-Path $compilerBin 'dumpbin.exe') /nologo /exports $usb
if ($LASTEXITCODE) { throw 'Could not inspect libusb exports.' }
$names=@($exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(libusb_\w+)\s*$') { $Matches[1] } })
if ($names.Count -lt 80) { throw 'Unexpected libusb export table.' }
$definition=Join-Path $deps 'private-usb.def'
@('LIBRARY birriasdr-usb-1.0.dll','EXPORTS')+$names | Set-Content -LiteralPath $definition -Encoding ascii
$importLibrary=Join-Path $deps 'private-usb.lib'
& (Join-Path $compilerBin 'lib.exe') /nologo "/def:$definition" /machine:x64 "/out:$importLibrary"
if ($LASTEXITCODE) { throw 'Could not create the private-name import library.' }
$options=@('-DBUILD_TESTING=OFF',"-DLIBUSB_INCLUDE_DIR=$deps/include","-DLIBUSB_LIBRARY=$importLibrary","-DLIBUSB_RUNTIME=$usb")
if ($SoapyRoot) { $options+=@('-DBIRRIASDR_SOAPY=ON',"-DSoapySDR_DIR=$SoapyRoot/cmake") } else { $options+='-DBIRRIASDR_SOAPY=OFF' }
& $cmake -S $repoRoot -B $BuildDirectory -A x64 @options
if ($LASTEXITCODE) { throw 'Application CMake configuration failed.' }
& $cmake --build $BuildDirectory --config Release --parallel
if ($LASTEXITCODE) { throw 'Native application build failed.' }
$references=Join-Path $BuildDirectory 'sdrsharp-references'
& $Python "$PSScriptRoot/extract_sdrsharp_sdk.py" $application $references
if ($LASTEXITCODE) { throw 'Reference extraction failed. Supply a working Python 3 interpreter.' }
$plugin=Join-Path $BuildDirectory 'sdrsharp'
& dotnet build "$repoRoot/integrations/sdrsharp/SDRSharp.BirriaSDR.csproj" -c Release "-p:SdrSharpSdk=$references" -o $plugin
if ($LASTEXITCODE) { throw 'SDR# plugin build failed.' }
$native=Join-Path $plugin 'birriasdr'
New-Item -ItemType Directory -Force $native | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'Release\birriasdr.dll'),$usb -Destination $native
Copy-Item -LiteralPath (Join-Path $deps 'COPYING') -Destination (Join-Path $native 'COPYING.libusb')
Copy-Item -LiteralPath $usbSource -Destination $native
Copy-Item -LiteralPath "$repoRoot/LICENSE","$repoRoot/NOTICE","$repoRoot/LICENSES/BSD-3-Clause-Clear.txt" -Destination $native
if ($SoapyRoot) { Copy-Item -LiteralPath $usb -Destination (Join-Path $BuildDirectory 'Release') }
Write-Host "SDR# plugin: $plugin"
Write-Host "Soapy module: $(Join-Path $BuildDirectory 'Release')"
Write-Host 'The private USB DLL is the unmodified official 1.0.30 binary with a unique filename.'
