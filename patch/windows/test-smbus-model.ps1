# 只读 SMBus/DIMM 逻辑自检。不会打开设备、访问端口或加载驱动。
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$HstStsBusy = 0x01
$HstStsOk2 = 0x02
$HstStsErr = 0x04
$SpdAddresses = [byte[]](0x53, 0x52, 0x51, 0x50)

function Get-SmbusStatusClass {
    param([byte]$Status)

    if (($Status -band $HstStsBusy) -ne 0) { return 'Busy' }
    if (($Status -band $HstStsErr) -ne 0) { return 'Error' }
    if (($Status -band $HstStsOk2) -ne 0) { return 'Success' }
    return 'Error'
}

function Convert-RawTemperature {
    param([uint16]$Raw)

    $scaled = ([uint32]$Raw -shl 3) -shr 5
    return [uint32][math]::Floor(($scaled * 25) / 100)
}

function Test-ValidTemperature {
    param([uint32]$Celsius)
    return $Celsius -le 120
}

function Assert-Equal {
    param($Expected, $Actual, [string]$Name)
    if ($Expected -ne $Actual) {
        throw "$Name：期望 '$Expected'，实际 '$Actual'"
    }
}

Assert-Equal 'Success' (Get-SmbusStatusClass 0x02) 'HST_STS=0x02'
Assert-Equal 'Success' (Get-SmbusStatusClass 0x12) 'HST_STS=0x12'
Assert-Equal 'Error' (Get-SmbusStatusClass 0x04) 'HST_STS=0x04'
Assert-Equal 'Busy' (Get-SmbusStatusClass 0x01) 'HST_STS BUSY'
Assert-Equal 'Busy' (Get-SmbusStatusClass 0x07) 'BUSY 优先于错误位'
Assert-Equal 0 (Convert-RawTemperature 0) 'raw=0 -> 0°C'
if (-not (Test-ValidTemperature 0)) {
    throw '0°C 应为有效温度'
}
Assert-Equal 30 (Convert-RawTemperature 480) 'raw=480 -> 30°C'
Assert-Equal 40 (Convert-RawTemperature 640) 'raw=640 -> 40°C'
Assert-Equal 2 (Convert-RawTemperature 44) 'raw=44 的整数截断'
if (-not (Test-ValidTemperature (Convert-RawTemperature 1920))) {
    throw '120°C 应为有效温度'
}
if (Test-ValidTemperature (Convert-RawTemperature 1984)) {
    throw '超过 120°C 的结果必须拒绝'
}
Assert-Equal '83,82,81,80' (($SpdAddresses -join ',')) 'SPD 地址顺序'

Write-Output 'SMBus model checks passed; no hardware access performed.'
