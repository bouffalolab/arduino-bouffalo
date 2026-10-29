[CmdletBinding()]
param(
    [string]$BridgePort = 'COM34',
    [string]$ReferencePort = 'COM35',
    [int]$BootstrapSettleMilliseconds = 500,
    [int]$RuntimeSettleMilliseconds = 300,
    [int]$SambaReadTimeoutMilliseconds = 500,
    [int]$MaxCaptureBytes = 128
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

function Set-BridgeLineCoding {
    param(
        [string]$Port,
        [int]$BaudRate,
        [int]$SettleMilliseconds
    )

    $link = [System.IO.Ports.SerialPort]::new($Port, 115200)
    $link.DtrEnable = $false
    $link.RtsEnable = $false
    $link.Open()
    try {
        $link.BaudRate = $BaudRate
        Start-Sleep -Milliseconds $SettleMilliseconds
        Write-Host "CDC_SET_LINE_CODING=$BaudRate"
    }
    finally {
        $link.Close()
    }
}

function Read-SerialCapture {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [int]$DurationMilliseconds,
        [int]$MaxBytes
    )

    $bytes = [System.Collections.Generic.List[byte]]::new()
    $discardedByteCount = 0
    $deadline = (Get-Date).AddMilliseconds($DurationMilliseconds)
    do {
        try {
            $value = $Link.ReadByte()
            if ($value -ge 0) {
                if ($bytes.Count -lt $MaxBytes) {
                    $bytes.Add([byte]$value)
                } else {
                    $discardedByteCount++
                }
            }
        }
        catch [System.TimeoutException] {
            continue
        }
    } while ((Get-Date) -lt $deadline)

    return [pscustomobject]@{
        Bytes = $bytes.ToArray()
        DiscardedByteCount = $discardedByteCount
    }
}

function Format-HexCapture {
    param([pscustomobject]$Capture)

    if ($null -eq $Capture.Bytes -or $Capture.Bytes.Length -eq 0) {
        return '<none>'
    }

    $text = [System.BitConverter]::ToString($Capture.Bytes)
    if ($Capture.DiscardedByteCount -gt 0) {
        $text += "...(+$($Capture.DiscardedByteCount) bytes)"
    }
    return $text
}

function Get-PrintableAsciiPrefix {
    param([byte[]]$Bytes)

    if ($null -eq $Bytes -or $Bytes.Length -eq 0) {
        return ''
    }

    $prefix = [System.Collections.Generic.List[byte]]::new()
    foreach ($value in $Bytes) {
        if ($value -lt 0x20 -or $value -gt 0x7e) {
            break
        }
        $prefix.Add($value)
    }
    return [System.Text.Encoding]::ASCII.GetString($prefix.ToArray())
}

function Test-UnoR4SambaVersion {
    param(
        [string]$Version,
        [pscustomobject]$Capture
    )

    if ($Capture.DiscardedByteCount -ne 0) {
        return $false
    }

    return $Version -match '^Arduino Bootloader \(SAM-BA extended\) [0-9]+\.[0-9]+ \[Arduino:[A-Z]+\]$'
}

function Send-And-CaptureReference {
    param(
        [System.IO.Ports.SerialPort]$Bridge,
        [System.IO.Ports.SerialPort]$Reference,
        [byte[]]$Request,
        [int]$ReadTimeoutMilliseconds,
        [int]$MaxBytes
    )

    $Reference.DiscardInBuffer()
    $Bridge.Write($Request, 0, $Request.Length)
    $Bridge.BaseStream.Flush()
    return Read-SerialCapture `
        -Link $Reference `
        -DurationMilliseconds $ReadTimeoutMilliseconds `
        -MaxBytes $MaxBytes
}

$bridgeDevice = Require-SerialPort -Port $BridgePort -DescriptionPattern 'VID_2341&PID_1002'
$referenceDevice = Require-SerialPort -Port $ReferencePort -DescriptionPattern 'VID_0403.*PID_6010'

Write-Host "Bridge:    $BridgePort $($bridgeDevice.InstanceId)"
Write-Host "Reference: $ReferencePort $($referenceDevice.InstanceId)"
Write-Host 'Reference-RX SAM-BA probe sends only UART bootstrap and version commands; it does not erase, write, read, lock, or reset RA4M1 Flash.'
Write-Host 'Required wiring: RA4 TX / ESP-IO44 is connected to the FTDI RX pin for the Reference port; GPIO23 is not part of this setup.'

$reference = [System.IO.Ports.SerialPort]::new($ReferencePort, 230400)
$reference.DtrEnable = $false
$reference.RtsEnable = $false
$reference.ReadTimeout = $SambaReadTimeoutMilliseconds
$reference.WriteTimeout = $SambaReadTimeoutMilliseconds
$reference.Open()

# FTDI RTS# is active low. Keep it high so the relay is released and selects
# the RA4M1 path while this same FTDI channel samples its RX pin.
$reference.RtsEnable = $false
Start-Sleep -Milliseconds 50

try {
    Write-Host 'Relay route: RA4M1 UART (RTS=HIGH, relay released); FTDI RX is the reference receiver.'
    Set-BridgeLineCoding `
        -Port $BridgePort `
        -BaudRate 1200 `
        -SettleMilliseconds $BootstrapSettleMilliseconds

    $bridge = [System.IO.Ports.SerialPort]::new($BridgePort, 230400)
    $bridge.DtrEnable = $false
    $bridge.RtsEnable = $false
    $bridge.ReadTimeout = $SambaReadTimeoutMilliseconds
    $bridge.WriteTimeout = $SambaReadTimeoutMilliseconds
    $bridge.Open()

    try {
        Start-Sleep -Milliseconds 100
        $idleReply = Read-SerialCapture `
            -Link $reference `
            -DurationMilliseconds $SambaReadTimeoutMilliseconds `
            -MaxBytes $MaxCaptureBytes

        $autoBaudFirstReply = Send-And-CaptureReference `
            -Bridge $bridge `
            -Reference $reference `
            -Request ([byte[]](0x80)) `
            -ReadTimeoutMilliseconds $SambaReadTimeoutMilliseconds `
            -MaxBytes $MaxCaptureBytes

        $autoBaudSecondReply = Send-And-CaptureReference `
            -Bridge $bridge `
            -Reference $reference `
            -Request ([byte[]](0x80)) `
            -ReadTimeoutMilliseconds $SambaReadTimeoutMilliseconds `
            -MaxBytes $MaxCaptureBytes

        $autoBaudConfirmReply = Send-And-CaptureReference `
            -Bridge $bridge `
            -Reference $reference `
            -Request ([byte[]](0x23)) `
            -ReadTimeoutMilliseconds $SambaReadTimeoutMilliseconds `
            -MaxBytes $MaxCaptureBytes

        $binaryModeReply = Send-And-CaptureReference `
            -Bridge $bridge `
            -Reference $reference `
            -Request ([byte[]](0x4e, 0x23)) `
            -ReadTimeoutMilliseconds $SambaReadTimeoutMilliseconds `
            -MaxBytes $MaxCaptureBytes

        $versionReply = Send-And-CaptureReference `
            -Bridge $bridge `
            -Reference $reference `
            -Request ([byte[]](0x56, 0x23)) `
            -ReadTimeoutMilliseconds $SambaReadTimeoutMilliseconds `
            -MaxBytes $MaxCaptureBytes

        $version = Get-PrintableAsciiPrefix -Bytes $versionReply.Bytes
        Write-Host "RA4_REFERENCE_IDLE_REPLY_HEX=$(Format-HexCapture -Capture $idleReply)"
        Write-Host "RA4_REFERENCE_AUTO_BAUD_1_REPLY_HEX=$(Format-HexCapture -Capture $autoBaudFirstReply)"
        Write-Host "RA4_REFERENCE_AUTO_BAUD_2_REPLY_HEX=$(Format-HexCapture -Capture $autoBaudSecondReply)"
        Write-Host "RA4_REFERENCE_AUTO_BAUD_CONFIRM_REPLY_HEX=$(Format-HexCapture -Capture $autoBaudConfirmReply)"
        Write-Host "RA4_REFERENCE_N_REPLY_HEX=$(Format-HexCapture -Capture $binaryModeReply)"
        Write-Host "RA4_REFERENCE_V_REPLY_HEX=$(Format-HexCapture -Capture $versionReply)"
        Write-Host "RA4_REFERENCE_VERSION=$version"
        if (-not (Test-UnoR4SambaVersion -Version $version -Capture $versionReply)) {
            throw 'Reference RX did not capture an untruncated UNO R4 Arduino Bootloader version'
        }
        Write-Host 'RA4_REFERENCE_RX_READ_ONLY_PROBE_PASS'
    }
    finally {
        $bridge.Close()
    }
}
finally {
    try {
        Set-BridgeLineCoding `
            -Port $BridgePort `
            -BaudRate 2400 `
            -SettleMilliseconds $RuntimeSettleMilliseconds
        Set-BridgeLineCoding `
            -Port $BridgePort `
            -BaudRate 115200 `
            -SettleMilliseconds $RuntimeSettleMilliseconds
        Write-Host 'RA4 runtime reset requested and user UART restored to 115200'
    }
    finally {
        $reference.RtsEnable = $true
        $reference.Close()
        Write-Host 'Relay state: CKLink download route restored (RTS=LOW)'
    }
}
