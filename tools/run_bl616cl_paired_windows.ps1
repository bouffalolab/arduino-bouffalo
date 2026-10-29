[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Boot2,

    [Parameter(Mandatory = $true)]
    [string]$Partition,

    [Parameter(Mandatory = $true)]
    [string]$Application,

    [string]$FlashTool = 'D:\developer\tools\flash_cube\v1.4.0\BLFlashCommand.exe',
    [string]$CkLinkPort = 'COM30',
    [string]$RelayPort = 'COM35',
    [int]$BaudRate = 2000000,
    [int]$PreFlashResetSettleMilliseconds = 300,
    [int]$UsbTimeoutSeconds = 20,
    [switch]$SkipFlash,
    [switch]$SkipRelay,
    [switch]$SkipPostFlashBoot,
    [switch]$KeepRelayConnected
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-SerialPort {
    param(
        [string]$Port,
        [string]$DescriptionPattern
    )

    if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $Port) {
        throw "$Port is not currently enumerated"
    }

    $portNamePattern = '\(' + [regex]::Escape($Port) + '\)'
    $device = Get-PnpDevice -PresentOnly |
        Where-Object { $_.FriendlyName -match $portNamePattern } |
        Select-Object -First 1
    if ($null -eq $device) {
        throw "$Port has no matching present PnP device"
    }
    if ($device.InstanceId -notmatch $DescriptionPattern) {
        throw "$Port has unexpected identity: $($device.InstanceId)"
    }
    return [pscustomobject]@{
        DeviceID = $Port
        PNPDeviceID = $device.InstanceId
    }
}

function Wait-BridgeUsb {
    param([int]$TimeoutSeconds)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $devices = Get-PnpDevice -PresentOnly |
            Where-Object { $_.InstanceId -match 'VID_2341&PID_1002' }
        if ($devices) {
            return $devices
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    throw "BL616CL bridge USB VID_2341:PID_1002 did not enumerate within $TimeoutSeconds seconds"
}

function Reset-Bl616cl {
    param(
        [string]$Port,
        [int]$Speed
    )

    $link = [System.IO.Ports.SerialPort]::new($Port, $Speed)
    $link.DtrEnable = $false
    $link.RtsEnable = $false
    $link.Open()
    try {
        [void]$link.Write('BOUFFALOLAB5555RTS0')
        $link.BaseStream.Flush()
        Start-Sleep -Milliseconds 50
        [void]$link.Write('BOUFFALOLAB5555RTS1')
        $link.BaseStream.Flush()
    }
    finally {
        $link.Close()
    }
}

foreach ($path in @($Boot2, $Partition, $Application)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required image does not exist: $path"
    }
}
if (-not (Test-Path -LiteralPath $FlashTool -PathType Leaf)) {
    throw "FlashCube executable does not exist: $FlashTool"
}

$cklink = Require-SerialPort -Port $CkLinkPort -DescriptionPattern 'VID_42BF&PID_B210'
Write-Host "CKLink: $($cklink.DeviceID) $($cklink.PNPDeviceID)"
if (-not $SkipRelay) {
    $relay = Require-SerialPort -Port $RelayPort -DescriptionPattern 'VID_0403.*PID_6010'
    Write-Host "Relay:  $($relay.DeviceID) $($relay.PNPDeviceID)"
}
Get-FileHash -Algorithm SHA256 -LiteralPath $Boot2, $Partition, $Application |
    Format-Table -AutoSize

$relayLink = $null
if (-not $SkipRelay) {
    $relayLink = [System.IO.Ports.SerialPort]::new($RelayPort, 115200)
    $relayLink.DtrEnable = $false
    $relayLink.RtsEnable = $false
    $relayLink.Open()
    $relayLink.RtsEnable = $true
    Write-Host 'Relay state: CKLink download route (RTS=LOW, relay activated)'
} else {
    Write-Host 'Relay state: externally controlled'
}

try {
    if (-not $SkipFlash) {
        # CKLink's control firmware enters the target download/reset sequence
        # through this VCOM command pair. Do this before FlashCube handshakes.
        Reset-Bl616cl -Port $CkLinkPort -Speed $BaudRate
        Start-Sleep -Milliseconds $PreFlashResetSettleMilliseconds
        Write-Host 'BL616CL pre-flash reset requested'
        & $FlashTool `
            '--interface=uart' `
            '--chipname=bl616cl' `
            "--port=$CkLinkPort" `
            "--baudrate=$BaudRate" `
            write_flash_files `
            0x0 $Boot2 `
            0xE000 $Partition `
            0x10000 $Application
        if ($LASTEXITCODE -ne 0) {
            throw "FlashCube failed with exit code $LASTEXITCODE"
        }
    }

    if (-not $SkipPostFlashBoot) {
        if ($null -ne $relayLink) {
            $relayLink.RtsEnable = $false
            Start-Sleep -Milliseconds 50
            Write-Host 'Relay state: RA4 runtime route (RTS=HIGH, relay released)'
        }
        Reset-Bl616cl -Port $CkLinkPort -Speed $BaudRate
        $usbDevices = Wait-BridgeUsb -TimeoutSeconds $UsbTimeoutSeconds
        $usbDevices | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize
        Write-Host 'PAIRED_BRIDGE_FLASH_AND_BOOT_PASS'
    } else {
        Write-Host 'PAIRED_BRIDGE_FLASH_PASS'
    }
}
finally {
    if ($null -ne $relayLink -and -not $KeepRelayConnected) {
        $relayLink.RtsEnable = $true
        Write-Host 'Relay state: CKLink download route restored (RTS=LOW)'
    }
    if ($null -ne $relayLink) {
        $relayLink.Close()
    }
}
