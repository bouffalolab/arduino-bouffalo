[CmdletBinding()]
param(
    [string]$BridgePort = 'COM34',
    [string]$RelayPort = 'COM35',
    [int]$BootstrapSettleMilliseconds = 500,
    [int]$RuntimeSettleMilliseconds = 300,
    [int]$SambaReadTimeoutMilliseconds = 500,
    [int]$MaxCaptureBytes = 128,
    [bool]$UseRa4Uart = $true
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
        if ($value -lt 0x20 -or $value -gt 0x7E) {
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

function Invoke-SambaVersionProbe {
    param(
        [string]$Port,
        [int]$ReadTimeoutMilliseconds,
        [int]$MaxBytes
    )

    $link = [System.IO.Ports.SerialPort]::new($Port, 230400)
    $link.DtrEnable = $false
    $link.RtsEnable = $false
    $link.ReadTimeout = $ReadTimeoutMilliseconds
    $link.WriteTimeout = $ReadTimeoutMilliseconds
    $link.Open()

    try {
        Start-Sleep -Milliseconds 100
        $idleReply = Read-SerialCapture `
            -Link $link `
            -DurationMilliseconds $ReadTimeoutMilliseconds `
            -MaxBytes $MaxBytes

        # UART SAM-BA requires the BOSSA serial auto-baud exchange before its
        # normal commands. All commands below are non-persistent and do not
        # access RA4M1 Flash.
        $link.Write([byte[]](0x80), 0, 1)
        $link.BaseStream.Flush()
        $autoBaudFirstReply = Read-SerialCapture `
            -Link $link `
            -DurationMilliseconds $ReadTimeoutMilliseconds `
            -MaxBytes $MaxBytes

        $link.Write([byte[]](0x80), 0, 1)
        $link.BaseStream.Flush()
        $autoBaudSecondReply = Read-SerialCapture `
            -Link $link `
            -DurationMilliseconds $ReadTimeoutMilliseconds `
            -MaxBytes $MaxBytes

        $link.Write([byte[]](0x23), 0, 1) # #
        $link.BaseStream.Flush()
        $autoBaudConfirmReply = Read-SerialCapture `
            -Link $link `
            -DurationMilliseconds $ReadTimeoutMilliseconds `
            -MaxBytes $MaxBytes

        $link.Write([byte[]](0x4E, 0x23), 0, 2) # N#
        $link.BaseStream.Flush()
        $binaryModeReply = Read-SerialCapture `
            -Link $link `
            -DurationMilliseconds $ReadTimeoutMilliseconds `
            -MaxBytes $MaxBytes

        $link.Write([byte[]](0x56, 0x23), 0, 2) # V#
        $link.BaseStream.Flush()
        $versionReply = Read-SerialCapture `
            -Link $link `
            -DurationMilliseconds $ReadTimeoutMilliseconds `
            -MaxBytes $MaxBytes

        $printable = Get-PrintableAsciiPrefix -Bytes $versionReply.Bytes
        return [pscustomobject]@{
            IdleReply = $idleReply
            AutoBaudFirstReply = $autoBaudFirstReply
            AutoBaudSecondReply = $autoBaudSecondReply
            AutoBaudConfirmReply = $autoBaudConfirmReply
            BinaryModeReply = $binaryModeReply
            VersionReply = $versionReply
            Version = $printable
        }
    }
    finally {
        $link.Close()
    }
}

$bridge = Require-SerialPort -Port $BridgePort -DescriptionPattern 'VID_2341&PID_1002'
$relay = Require-SerialPort -Port $RelayPort -DescriptionPattern 'VID_0403.*PID_6010'

Write-Host "Bridge: $BridgePort $($bridge.InstanceId)"
Write-Host "Relay:  $RelayPort $($relay.InstanceId)"
Write-Host 'RA4 SAM-BA probe sends only N# and V#; it does not erase, write, read, lock, or reset RA4M1 Flash.'

$relayLink = [System.IO.Ports.SerialPort]::new($RelayPort, 115200)
$relayLink.DtrEnable = $false
$relayLink.RtsEnable = $false
$relayLink.Open()
# FTDI exposes RTS# (active low). With the current relay wiring, release
# selects the RA4M1 route and activation selects the CKLink route.
$relayLink.RtsEnable = -not $UseRa4Uart
Start-Sleep -Milliseconds 50

try {
    if ($UseRa4Uart) {
        Write-Host 'Relay route: RA4M1 UART (RTS=HIGH, relay released)'
    } else {
        Write-Host 'Relay route: CKLink UART (RTS=LOW, relay activated)'
    }
    Set-BridgeLineCoding `
        -Port $BridgePort `
        -BaudRate 1200 `
        -SettleMilliseconds $BootstrapSettleMilliseconds

    $probe = Invoke-SambaVersionProbe `
        -Port $BridgePort `
        -ReadTimeoutMilliseconds $SambaReadTimeoutMilliseconds `
        -MaxBytes $MaxCaptureBytes
    Write-Host "RA4_SAMBA_IDLE_REPLY_HEX=$(Format-HexCapture -Capture $probe.IdleReply)"
    Write-Host "RA4_SAMBA_AUTO_BAUD_1_REPLY_HEX=$(Format-HexCapture -Capture $probe.AutoBaudFirstReply)"
    Write-Host "RA4_SAMBA_AUTO_BAUD_2_REPLY_HEX=$(Format-HexCapture -Capture $probe.AutoBaudSecondReply)"
    Write-Host "RA4_SAMBA_AUTO_BAUD_CONFIRM_REPLY_HEX=$(Format-HexCapture -Capture $probe.AutoBaudConfirmReply)"
    Write-Host "RA4_SAMBA_N_REPLY_HEX=$(Format-HexCapture -Capture $probe.BinaryModeReply)"
    Write-Host "RA4_SAMBA_V_REPLY_HEX=$(Format-HexCapture -Capture $probe.VersionReply)"
    Write-Host "RA4_SAMBA_VERSION=$($probe.Version)"
    if (-not (Test-UnoR4SambaVersion -Version $probe.Version -Capture $probe.VersionReply)) {
        throw 'SAM-BA V# did not return an untruncated UNO R4 Arduino Bootloader version'
    }
    Write-Host 'RA4_SAMBA_READ_ONLY_PROBE_PASS'
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
        $relayLink.RtsEnable = $true
        $relayLink.Close()
        Write-Host 'Relay state: CKLink download route restored (RTS=LOW)'
    }
}
