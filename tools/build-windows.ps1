# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$LibusbRoot,
    [string]$BuildDirectory = 'build-windows',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmakePath = $cmakeCommand.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ Build Tools and CMake.' }
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vsRoot) { throw 'No Visual Studio C++ toolchain found.' }
    $cmakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (!(Test-Path -LiteralPath $cmakePath)) { throw 'CMake executable not found.' }
if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repoRoot $BuildDirectory }
if (!$LibusbRoot) {
    # Pin the official x64 release. Older bundled runtimes crashed with libusbK.
    $deps = Join-Path $BuildDirectory 'deps'
    $LibusbRoot = Join-Path $deps 'libusb-1.0.30'
    $archive = Join-Path $deps 'libusb-1.0.30.7z'
    $unpacked = Join-Path $deps 'libusb-1.0.30-unpacked'
    if (!(Test-Path -LiteralPath (Join-Path $LibusbRoot 'bin\libusb-1.0.dll'))) {
        New-Item -ItemType Directory -Force $deps, $unpacked | Out-Null
        if (!(Test-Path -LiteralPath $archive)) {
            Invoke-WebRequest 'https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.7z' -OutFile $archive
        }
        $expected = '7FB1DFEC805B97983763D7D0AE244320DA12ADD1003D4249C96CC4D586398C79'
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
            throw 'The libusb download does not match the pinned SHA256.'
        }
        tar -xf $archive -C $unpacked VS2022/MS64/dll/libusb-1.0.dll VS2022/MS64/dll/libusb-1.0.lib include/libusb.h
        if ($LASTEXITCODE) { throw 'Could not extract libusb. Windows tar with 7z support is required.' }
        New-Item -ItemType Directory -Force (Join-Path $LibusbRoot 'include\libusb-1.0'), (Join-Path $LibusbRoot 'lib'), (Join-Path $LibusbRoot 'bin') | Out-Null
        Copy-Item -LiteralPath (Join-Path $unpacked 'include\libusb.h') -Destination (Join-Path $LibusbRoot 'include\libusb-1.0\libusb.h')
        Copy-Item -LiteralPath (Join-Path $unpacked 'VS2022\MS64\dll\libusb-1.0.lib') -Destination (Join-Path $LibusbRoot 'lib\libusb-1.0.lib')
        Copy-Item -LiteralPath (Join-Path $unpacked 'VS2022\MS64\dll\libusb-1.0.dll') -Destination (Join-Path $LibusbRoot 'bin\libusb-1.0.dll')
    }
}
$LibusbRoot = (Resolve-Path -LiteralPath $LibusbRoot).Path
$includePath = Join-Path $LibusbRoot 'include\libusb-1.0'
$libraryPath = Join-Path $LibusbRoot 'lib\libusb-1.0.lib'
$runtimePath = Join-Path $LibusbRoot 'bin\libusb-1.0.dll'
foreach ($required in @((Join-Path $includePath 'libusb.h'), $libraryPath, $runtimePath)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing dependency: $required. Supply an x64 libusb prefix with -LibusbRoot." }
}
$runtimeVersion = (Get-Item -LiteralPath $runtimePath).VersionInfo
if ([version]$runtimeVersion.FileVersion -lt [version]'1.0.30.0') {
    throw 'Use libusb 1.0.30 or newer on Windows. Omit -LibusbRoot to fetch the pinned working runtime.'
}
& $cmakePath -S $repoRoot -B $BuildDirectory -A x64 "-DLIBUSB_INCLUDE_DIR=$includePath" "-DLIBUSB_LIBRARY=$libraryPath" "-DLIBUSB_RUNTIME=$runtimePath"
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& $cmakePath --build $BuildDirectory --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
if (!$SkipTests) {
    $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
    & $ctestPath --test-dir $BuildDirectory -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'Offline tests failed.' }
}
Write-Host "Receiver: $(Join-Path $BuildDirectory 'Release\birria_rx.exe')"
