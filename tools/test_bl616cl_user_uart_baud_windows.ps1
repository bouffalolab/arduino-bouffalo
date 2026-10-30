[CmdletBinding()]
param(
    [string]$BridgePort = 'COM34',
    [string]$RelayPort = 'COM35',
    [int]$BaudRate = 230400,
    [int]$DurationMilliseconds = 1000
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

if ($BaudRate -le 0) {
    throw 'BaudRate must be positive'
}
if ($DurationMilliseconds -le 0) {
    throw 'DurationMilliseconds must be positive'
}

$bridge = Require-SerialPort -Port $BridgePort -DescriptionPattern 'VID_2341&PID_1002'
$relay = Require-SerialPort -Port $RelayPort -DescriptionPattern 'VID_0403.*PID_6010'
Write-Host "Bridge: $BridgePort $($bridge.InstanceId)"
Write-Host "Relay:  $RelayPort $($relay.InstanceId)"

$relayLink = [System.IO.Ports.SerialPort]::new($RelayPort, 115200)
$bridgeLink = [System.IO.Ports.SerialPort]::new($BridgePort, $BaudRate)
$relayLink.DtrEnable = $false
$relayLink.RtsEnable = $false
$bridgeLink.DtrEnable = $false
$bridgeLink.RtsEnable = $false

$payload = New-Object byte[] 256
for ($index = 0; $index -lt $payload.Length; $index++) {
    $payload[$index] = 0x55
}

$relayLink.Open()
$relayLink.RtsEnable = $false
Start-Sleep -Milliseconds 50

try {
    $bridgeLink.Open()
    try {
        Start-Sleep -Milliseconds 100
        $deadline = (Get-Date).AddMilliseconds($DurationMilliseconds)
        $bytesSent = 0
        Write-Host "USER_UART_BAUD_WAVEFORM_STARTED baud=$BaudRate pattern=0x55 duration_ms=$DurationMilliseconds"
        do {
            $bridgeLink.Write($payload, 0, $payload.Length)
            $bytesSent += $payload.Length
        } while ((Get-Date) -lt $deadline)
        $bridgeLink.BaseStream.Flush()
        Write-Host "USER_UART_BAUD_WAVEFORM_DONE bytes_sent=$bytesSent"
    }
    finally {
        $bridgeLink.BaudRate = 115200
        Start-Sleep -Milliseconds 100
        $bridgeLink.Close()
        Write-Host 'Bridge user UART restored to 115200'
    }
}
finally {
    $relayLink.RtsEnable = $true
    $relayLink.Close()
    Write-Host 'Relay state: CKLink download route restored (RTS=LOW)'
}
