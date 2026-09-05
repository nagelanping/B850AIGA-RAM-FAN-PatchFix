# collect-resource-info.ps1 - read-only Windows PnP resource and status report
# Does not install drivers, access I/O ports, or change system settings.
[CmdletBinding()]
param(
    [string[]]$InstanceId = @('PCI\VEN_1022&DEV_790B*', 'ACPI\PNP0C02*')
)

$ErrorActionPreference = 'Stop'

$devices = foreach ($pattern in $InstanceId) {
    Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like $pattern }
}

if (-not $devices) {
    throw 'No matching PCI SMBus or ACPI PNP0C02 device was found.'
}

foreach ($device in ($devices | Sort-Object InstanceId -Unique)) {
    Write-Output "=== $($device.InstanceId) ==="
    Write-Output "Class=$($device.Class) Status=$($device.Status) FriendlyName=$($device.FriendlyName)"

    try {
        $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
        Write-Output "DEVPKEY_Device_Driver=$($property.Data)"
    }
    catch {
        Write-Output "DEVPKEY_Device_Driver=<unavailable: $($_.Exception.Message)>"
    }

    try {
        $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_LocationInfo'
        Write-Output "DEVPKEY_Device_LocationInfo=$($property.Data)"
    }
    catch {
        Write-Output "DEVPKEY_Device_LocationInfo=<unavailable: $($_.Exception.Message)>"
    }

    Write-Output '--- pnputil device resources ---'
    pnputil /enum-devices /instanceid "$($device.InstanceId)" /resources 2>&1
    Write-Output ''
}
