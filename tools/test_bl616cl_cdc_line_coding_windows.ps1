[CmdletBinding()]
param(
    [string]$BridgePort = 'COM34',
    [string]$RelayPort = 'COM35',
    [int]$SettleMilliseconds = 500
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

    $portPattern = '\(' + [regex]::Escape($Port) + '\)'
    $device = Get-PnpDevice -PresentOnly |
        Where-Object { $_.FriendlyName -match $portPattern } |
        Select-Object -First 1
    if ($null -eq $device) {
        throw "$Port has no matching present PnP device"
    }
    if ($device.InstanceId -notmatch $DescriptionPattern) {
        throw "$Port has unexpected identity: $($device.InstanceId)"
    }
    return $device
}

$bridge = Require-SerialPort `
    -Port $BridgePort `
    -DescriptionPattern 'VID_2341&PID_1002'
$relay = Require-SerialPort `
    -Port $RelayPort `
    -DescriptionPattern 'VID_0403.*PID_6010'

Write-Host "Bridge: $BridgePort $($bridge.InstanceId)"
Write-Host "Relay:  $RelayPort $($relay.InstanceId)"

$relayLink = [System.IO.Ports.SerialPort]::new($RelayPort, 115200)
$bridgeLink = [System.IO.Ports.SerialPort]::new($BridgePort, 115200)
$relayLink.DtrEnable = $false
$relayLink.RtsEnable = $false
$bridgeLink.DtrEnable = $false
$bridgeLink.RtsEnable = $false

$relayLink.Open()
$relayLink.RtsEnable = $false
Start-Sleep -Milliseconds 50
try {
    $bridgeLink.Open()
    try {
        foreach ($baud in @(1200, 2400, 115200)) {
            $bridgeLink.BaudRate = $baud
            Start-Sleep -Milliseconds $SettleMilliseconds
            Write-Host "CDC_SET_LINE_CODING=$baud"
        }

        $received = $bridgeLink.ReadExisting()
        Write-Host "CDC_RX_BYTES=$($received.Length)"
        Write-Host 'CDC_LINE_CODING_HOST_SEQUENCE_PASS'
    }
    finally {
        $bridgeLink.Close()
    }
}
finally {
    $relayLink.RtsEnable = $true
    $relayLink.Close()
    Write-Host 'Relay state: CKLink download route restored (RTS=LOW)'
}
