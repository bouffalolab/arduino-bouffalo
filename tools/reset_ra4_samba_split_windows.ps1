[CmdletBinding()]
param(
    [string]$BridgePort = 'COM34',
    [string]$ReferencePort = 'COM35',
    [int]$BootstrapSettleMilliseconds = 500,
    [int]$RuntimeSettleMilliseconds = 300,
    [int]$ReadTimeoutMilliseconds = 1000,
    [int]$RuntimeCaptureMilliseconds = 3500,
    [switch]$ExplicitApplicationReset
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-SerialPort {
    param(
        [string]$Port,
        [string]$InstanceIdPattern
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
    if ($device.InstanceId -notmatch $InstanceIdPattern) {
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

function Write-All {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [byte[]]$Bytes
    )

    $Link.Write($Bytes, 0, $Bytes.Length)
    $Link.BaseStream.Flush()
}

function Read-Capture {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [int]$DurationMilliseconds,
        [int]$MaximumBytes
    )

    $bytes = [System.Collections.Generic.List[byte]]::new()
    $deadline = (Get-Date).AddMilliseconds($DurationMilliseconds)
    while ((Get-Date) -lt $deadline) {
        $remaining = [int][Math]::Max(1, ($deadline - (Get-Date)).TotalMilliseconds)
        $Link.ReadTimeout = $remaining
        try {
            $value = $Link.ReadByte()
        }
        catch [System.TimeoutException] {
            continue
        }

        if ($value -ge 0) {
            if ($bytes.Count -ge $MaximumBytes) {
                throw "serial capture exceeded $MaximumBytes byte(s)"
            }
            $bytes.Add([byte]$value)
        }
    }
    # Preserve an empty capture as an empty byte array rather than PowerShell
    # pipeline-enumerating it into $null at the call site.
    Write-Output -NoEnumerate $bytes.ToArray()
}

function Read-Exact {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [int]$Count,
        [int]$TimeoutMilliseconds,
        [string]$Description
    )

    $reply = New-Object byte[] $Count
    $offset = 0
    $deadline = (Get-Date).AddMilliseconds($TimeoutMilliseconds)
    while ($offset -lt $Count) {
        $remaining = [int](($deadline - (Get-Date)).TotalMilliseconds)
        if ($remaining -le 0) {
            throw "$Description timed out after $offset/$Count byte(s)"
        }
        $Link.ReadTimeout = [Math]::Max(1, $remaining)
        try {
            $value = $Link.ReadByte()
        }
        catch [System.TimeoutException] {
            continue
        }
        if ($value -ge 0) {
            $reply[$offset++] = [byte]$value
        }
    }
    return $reply
}

function Format-HexBytes {
    param([byte[]]$Bytes)

    if ($null -eq $Bytes -or $Bytes.Length -eq 0) {
        return '<none>'
    }
    return [System.BitConverter]::ToString($Bytes)
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

function Send-And-Capture {
    param(
        [System.IO.Ports.SerialPort]$Bridge,
        [System.IO.Ports.SerialPort]$Reference,
        [byte[]]$Request,
        [int]$DurationMilliseconds,
        [int]$MaximumBytes
    )

    $Reference.DiscardInBuffer()
    Write-All -Link $Bridge -Bytes $Request
    return Read-Capture `
        -Link $Reference `
        -DurationMilliseconds $DurationMilliseconds `
        -MaximumBytes $MaximumBytes
}

function Initialize-Samba {
    param(
        [System.IO.Ports.SerialPort]$Bridge,
        [System.IO.Ports.SerialPort]$Reference
    )

    Start-Sleep -Milliseconds 100
    $autoBaudFirstReply = Send-And-Capture `
        -Bridge $Bridge `
        -Reference $Reference `
        -Request ([byte[]](0x80)) `
        -DurationMilliseconds $ReadTimeoutMilliseconds `
        -MaximumBytes 16
    $autoBaudSecondReply = Send-And-Capture `
        -Bridge $Bridge `
        -Reference $Reference `
        -Request ([byte[]](0x80)) `
        -DurationMilliseconds $ReadTimeoutMilliseconds `
        -MaximumBytes 16
    $autoBaudConfirmReply = Send-And-Capture `
        -Bridge $Bridge `
        -Reference $Reference `
        -Request ([byte[]](0x23)) `
        -DurationMilliseconds $ReadTimeoutMilliseconds `
        -MaximumBytes 16
    $binaryModeReply = Send-And-Capture `
        -Bridge $Bridge `
        -Reference $Reference `
        -Request ([byte[]](0x4e, 0x23)) `
        -DurationMilliseconds $ReadTimeoutMilliseconds `
        -MaximumBytes 16
    $versionReply = Send-And-Capture `
        -Bridge $Bridge `
        -Reference $Reference `
        -Request ([byte[]](0x56, 0x23)) `
        -DurationMilliseconds $ReadTimeoutMilliseconds `
        -MaximumBytes 128

    Write-Host "SAMBA_AUTO_BAUD_1=$(Format-HexBytes -Bytes $autoBaudFirstReply)"
    Write-Host "SAMBA_AUTO_BAUD_2=$(Format-HexBytes -Bytes $autoBaudSecondReply)"
    Write-Host "SAMBA_AUTO_BAUD_CONFIRM=$(Format-HexBytes -Bytes $autoBaudConfirmReply)"
    Write-Host "SAMBA_BINARY_MODE=$(Format-HexBytes -Bytes $binaryModeReply)"
    Write-Host "SAMBA_VERSION_REPLY=$(Format-HexBytes -Bytes $versionReply)"

    if ($binaryModeReply.Length -ne 2 -or
        $binaryModeReply[0] -ne 0x0a -or
        $binaryModeReply[1] -ne 0x0d) {
        throw "SAM-BA binary-mode reply was $(Format-HexBytes -Bytes $binaryModeReply)"
    }

    $version = Get-PrintableAsciiPrefix -Bytes $versionReply
    if ($version -notmatch '^Arduino Bootloader \(SAM-BA extended\) [0-9]+\.[0-9]+ \[Arduino:IKXYZ\]$') {
        throw "unexpected SAM-BA version reply: $(Format-HexBytes -Bytes $versionReply)"
    }
    Write-Host "SAMBA_VERSION=$version"
}

function Request-Ra4ApplicationReset {
    $source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "missing CMSIS-DAP Windows source: $source"
    }

    Add-Type -Path $source
    [void][CmsisDapWindowsProbe]::RunRa4ApplicationReset()
}

if ($BootstrapSettleMilliseconds -le 0 -or
    $RuntimeSettleMilliseconds -le 0 -or
    $ReadTimeoutMilliseconds -le 0 -or
    $RuntimeCaptureMilliseconds -lt 2000) {
    throw 'all timeouts must be positive; RuntimeCaptureMilliseconds must be at least 2000'
}

$bridgeDevice = Require-SerialPort -Port $BridgePort -InstanceIdPattern 'VID_2341&PID_1002'
$referenceDevice = Require-SerialPort -Port $ReferencePort -InstanceIdPattern 'VID_0403.*PID_6010'
Write-Host "Bridge:    $BridgePort $($bridgeDevice.InstanceId)"
Write-Host "Reference: $ReferencePort $($referenceDevice.InstanceId)"
Write-Host 'Target: RA4M1 reset only. This tool does not erase, write, read, lock, or otherwise modify RA4M1 Flash.'
Write-Host 'Relay route: RA4M1 UART (RtsEnable=false, physical RTS# HIGH, relay released).'

$reference = [System.IO.Ports.SerialPort]::new($ReferencePort, 230400)
$reference.DtrEnable = $false
$reference.RtsEnable = $false
$reference.Open()
$reference.RtsEnable = $false
$bridge = $null

try {
    Start-Sleep -Milliseconds 50
    Set-BridgeLineCoding `
        -Port $BridgePort `
        -BaudRate 1200 `
        -SettleMilliseconds $BootstrapSettleMilliseconds

    $bridge = [System.IO.Ports.SerialPort]::new($BridgePort, 230400)
    $bridge.DtrEnable = $false
    $bridge.RtsEnable = $false
    $bridge.Open()

    Initialize-Samba -Bridge $bridge -Reference $reference
    $reference.DiscardInBuffer()
    Write-All -Link $bridge -Bytes ([System.Text.Encoding]::ASCII.GetBytes('K#'))
    Write-Host 'SAMBA_RESET_COMMAND=K#'

    # On UNO R4 WiFi, 2400 bps enters RA4 ROM DFU. Do not use it as a
    # normal-runtime reset after K#: switch both ends directly to 115200.
    $reference.BaudRate = 115200
    $bridge.BaudRate = 115200
    Start-Sleep -Milliseconds $RuntimeSettleMilliseconds
    Write-Host 'RA4 runtime requested by K#; user UART restored directly to 115200.'
}
finally {
    if ($null -ne $bridge) {
        $bridge.Close()
    }
}

try {
    if ($ExplicitApplicationReset) {
        Request-Ra4ApplicationReset
        Start-Sleep -Milliseconds $RuntimeSettleMilliseconds
        Write-Host 'RA4 application reset feature requested after K# (explicit A/B mode).'
    } else {
        Write-Host 'RA4 application reset feature skipped; relying on SAM-BA K# handoff.'
    }

    $runtimeBytes = Read-Capture `
        -Link $reference `
        -DurationMilliseconds $RuntimeCaptureMilliseconds `
        -MaximumBytes 8192
    $runtime = [System.Text.Encoding]::ASCII.GetString($runtimeBytes)
    Write-Host 'RA4_RUNTIME_CAPTURE_BEGIN'
    Write-Host $runtime
    Write-Host 'RA4_RUNTIME_CAPTURE_END'

    if ($runtime -notmatch '(?m)^RA4_HEARTBEAT:BOOT\r?$') {
        throw 'RA4M1 heartbeat boot marker was not observed after K# plus normal-runtime reset'
    }
    if ([regex]::Matches($runtime, '(?m)^RA4_HEARTBEAT:[0-9]+\r?$').Count -lt 2) {
        throw 'fewer than two RA4M1 heartbeat markers were observed after K# plus normal-runtime reset'
    }
    Write-Host 'RA4_SAMBA_RESET_AND_RUNTIME_PASS'
}
finally {
    $reference.RtsEnable = $false
    $reference.Close()
    Write-Host 'Relay state: RA4M1 UART runtime route retained (RTS# HIGH, relay released).'
}
