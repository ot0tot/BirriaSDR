# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build-windows',
    [switch]$RunCapture
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repoRoot $BuildDirectory }
$devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0E8D&PID_7961&MI_*' })
if (!$devices.Count) { throw 'No attached 0e8d:7961 composite adapter found.' }
$wifi = @($devices | Where-Object { $_.InstanceId -like '*&MI_03\*' })
if ($wifi.Count -ne 1) { throw 'Expected exactly one WiFi_If interface 3. Inspect the adapter descriptors before continuing.' }
$devices | Select-Object Status, Class, FriendlyName, InstanceId | Format-List
$service = (Get-PnpDeviceProperty -InstanceId $wifi[0].InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
$problem = (Get-PnpDeviceProperty -InstanceId $wifi[0].InstanceId -KeyName 'DEVPKEY_Device_ProblemCode').Data
Write-Host "Wi-Fi service: $service; problem code: $problem"
if ($service -ne 'libusbK' -or $problem -ne 0) {
    throw 'Install libusbK using Zadig for WiFi_If (Interface 3), ID 0e8d:7961. Keep the composite parent and Bluetooth interfaces unchanged.'
}
$receiver = Join-Path $BuildDirectory 'Release\birria_rx.exe'
if (!(Test-Path -LiteralPath $receiver)) { throw 'Build first with tools\build-windows.ps1.' }
$listing = @(& $receiver --list)
if ($LASTEXITCODE) { throw 'Device enumeration failed.' }
$listing | Write-Output
if ($RunCapture) {
    if ($service -ne 'libusbK') {
        throw 'WinUSB rejects the required MT7961 host-wrapper request. Select libusbK for WiFi_If (Interface 3) in Zadig before capturing.'
    }
    if ($listing.Count -ne 1 -or $listing[0] -notmatch '^0: 0e8d:7961 ') {
        throw 'The smoke test requires the MT7961 to be the only recognized receiver. Disconnect other SDR Wi-Fi adapters before testing.'
    }
    # This test resets device 0, checks finite captures and a one-second stream.
    $hardwareTest = Join-Path $BuildDirectory 'Release\test_hardware.exe'
    & $hardwareTest --run --experimental-mt7961
    if ($LASTEXITCODE) { throw 'Hardware smoke test failed. Preserve its diagnostics before retrying.' }
}
