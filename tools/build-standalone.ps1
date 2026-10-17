# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$Python = 'python',
    [string]$NativeDirectory = 'build-windows/Release',
    [string]$OutputDirectory = 'build-applications/standalone',
    [switch]$SkipNativeBuild,
    [switch]$SkipExecutableBuild
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$SkipNativeBuild) {
    & "$PSScriptRoot/build-windows.ps1" -SkipTests
}
if (![IO.Path]::IsPathRooted($NativeDirectory)) { $NativeDirectory = Join-Path $repo $NativeDirectory }
if (![IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory = Join-Path $repo $OutputDirectory }
$NativeDirectory = (Resolve-Path -LiteralPath $NativeDirectory).Path
if ([version](Get-Item -LiteralPath (Join-Path $NativeDirectory 'libusb-1.0.dll')).VersionInfo.FileVersion -lt [version]'1.0.30.0') {
    throw 'Standalone streaming requires the qualified libusb 1.0.30 runtime or newer.'
}
$packager = Join-Path $repo 'build-tools/standalone-packager'
if (!(Test-Path -LiteralPath (Join-Path $packager 'PyInstaller/__main__.py'))) {
    & $Python -m pip install --target $packager 'pyinstaller==6.16.0'
    if ($LASTEXITCODE) { throw 'Could not install the project-local packager.' }
}
$previousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $packager
    & $Python -c 'import numpy; print(numpy.__version__)'
    if ($LASTEXITCODE) { throw 'Install NumPy for the selected Python interpreter.' }
    $work = Join-Path $repo 'build-applications/standalone-work'
    New-Item -ItemType Directory -Force $work | Out-Null
    if (!$SkipExecutableBuild) {
      & $Python -m PyInstaller --noconfirm --clean --onedir --console --name BirriaSDR `
        --distpath $OutputDirectory --workpath (Join-Path $work 'build') --specpath $work `
        --add-data "$(Join-Path $repo 'integrations/standalone/web');web" `
        --add-binary "$(Join-Path $NativeDirectory 'birriasdr.dll');native" `
        --add-binary "$(Join-Path $NativeDirectory 'birria_display.dll');native" `
        --add-binary "$(Join-Path $NativeDirectory 'libusb-1.0.dll');native" `
        (Join-Path $repo 'integrations/standalone/app.py')
      if ($LASTEXITCODE) { throw 'Standalone executable build failed.' }
    } elseif (!(Test-Path -LiteralPath (Join-Path $OutputDirectory 'BirriaSDR/BirriaSDR.exe'))) {
      throw 'No existing executable to package.'
    }
} finally { $env:PYTHONPATH = $previousPythonPath }
$package = Join-Path $OutputDirectory 'BirriaSDR'
$licenses = Join-Path $package 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item -LiteralPath "$repo/LICENSE","$repo/NOTICE","$repo/LICENSES/BSD-3-Clause-Clear.txt" -Destination $licenses
Copy-Item -LiteralPath "$repo/docs/standalone.md" -Destination $package
$usbSource = Join-Path $repo 'build-tools/libusb-1.0.30.tar.bz2'
$usbLicense = Join-Path $repo 'build-tools/COPYING.libusb'
New-Item -ItemType Directory -Force (Split-Path -Parent $usbSource) | Out-Null
if (!(Test-Path -LiteralPath $usbSource)) {
    Invoke-WebRequest 'https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2' -OutFile $usbSource
}
if ((Get-FileHash -LiteralPath $usbSource -Algorithm SHA256).Hash -ne 'FEA36F34F9156400209595E300840767AB1A385EDE1DC7EE893015AEA9C6DBAF') { throw 'Unexpected libusb source archive hash.' }
if (!(Test-Path -LiteralPath $usbLicense)) {
    Invoke-WebRequest 'https://raw.githubusercontent.com/libusb/libusb/v1.0.30/COPYING' -OutFile $usbLicense
}
if ((Get-FileHash -LiteralPath $usbLicense -Algorithm SHA256).Hash -ne '5DF07007198989C622F5D41DE8D703E7BEF3D0E79D62E24332EE739A452AF62A') { throw 'Unexpected libusb license hash.' }
Copy-Item -LiteralPath $usbSource -Destination $package
Copy-Item -LiteralPath $usbLicense -Destination $licenses
Copy-Item -LiteralPath "$repo/docs/windows.md","$repo/docs/bandwidth-validation.md","$repo/docs/tuning-research.md","$repo/docs/usb-streaming.md" -Destination $package
$licenseScript = @'
from pathlib import Path
import importlib.metadata as m, shutil, sys, json, ssl
out=Path(sys.argv[1])
versions={'python':sys.version,'openssl':ssl.OPENSSL_VERSION}
for name in ['numpy','pyinstaller','charset-normalizer','typing_extensions']:
    try:distribution=m.distribution(name)
    except m.PackageNotFoundError:
        if name in ['numpy','pyinstaller']:raise
        continue
    versions[name]=distribution.version
    for file in distribution.files:
        if 'license' in str(file).lower() or Path(file).name.lower().startswith('copying'):
            src=Path(distribution.locate_file(file))
            if src.is_file():
                target=out/name/str(file).replace('../','')
                target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(src,target)
for name in ['LICENSE.txt','LICENSE']:
    src=Path(sys.base_prefix)/name
    if src.exists():shutil.copyfile(src,out/'LICENSE.Python.txt');break
(out.parent/'runtime-versions.json').write_text(json.dumps(versions,indent=2)+'\n')
'@
$env:PYTHONPATH = $packager
try {
    & $Python -c $licenseScript $licenses
    if ($LASTEXITCODE) { throw 'Could not collect runtime license notices.' }
} finally { $env:PYTHONPATH = $previousPythonPath }
Get-ChildItem -LiteralPath "$repo/integrations/standalone/licenses" -File | Copy-Item -Destination $licenses -Force
$source = Join-Path $package 'source'
New-Item -ItemType Directory -Force $source | Out-Null
Get-ChildItem -LiteralPath "$repo/integrations/standalone" -File | Copy-Item -Destination $source -Force
Copy-Item -LiteralPath "$repo/integrations/standalone/web" -Destination $source -Recurse -Force
$manifest = @{}
Get-ChildItem -LiteralPath $package -File -Recurse | Where-Object { $_.Name -ne 'sha256.json' } | ForEach-Object {
    $relative = $_.FullName.Substring($package.Length + 1).Replace('\','/')
    $manifest[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'sha256.json') -Encoding UTF8
$zip = Join-Path $OutputDirectory 'BirriaSDR-standalone-windows-x64.zip'
Compress-Archive -LiteralPath $package -DestinationPath $zip -Force
Write-Host "Application: $(Join-Path $package 'BirriaSDR.exe')"
Write-Host "Portable package: $zip"
