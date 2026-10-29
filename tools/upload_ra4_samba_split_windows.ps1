[CmdletBinding()]
param(
    [string]$ImagePath = 'build/ra4-uart-heartbeat/ra4-uart-heartbeat.ino.bin',
    [string]$BridgePort = 'COM34',
    [string]$ReferencePort = 'COM35',
    [int]$BootstrapSettleMilliseconds = 500,
    [int]$RuntimeSettleMilliseconds = 300,
    [int]$ReadTimeoutMilliseconds = 1000,
    [int]$LongReadTimeoutMilliseconds = 5000,
    [int]$RuntimeCaptureMilliseconds = 3500,
    [int]$PayloadPacketBytes = 64,
    [int]$PayloadInterPacketDelayMilliseconds = 5,
    [switch]$SkipRuntimeSmoke,
    [switch]$ExplicitApplicationReset,
    [switch]$Direct
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Arduino's SAM-BA extended bootloader exposes application Flash from logical
# address zero. The RA4M1 CPU maps the application's vector table at 0x4000,
# but BOSSA's erase/write commands use this bootloader address.
$applicationFlashAddress = [uint32]0x00000000
$flashEndAddress = [uint32]0x00040000
$flashPageSize = 256
$writeBufferSize = 4096
$writeBufferAddress = [uint32]0x00000034
$wordCopyStackAddress = [uint32]0x00000020
$wordCopyWordsAddress = [uint32]0x00000030

# BOSSA uses this 52-byte compatibility applet and the immediately following
# 4 KiB staging buffer at 0x34. The UNO R4 bootloader accepts S payloads as
# direct binary data when accessed through its CDC bridge transport.
$wordCopyApplet = [byte[]]@(
    0x09, 0x48, 0x0a, 0x49, 0x0a, 0x4a, 0x02, 0xe0,
    0x08, 0xc9, 0x08, 0xc0, 0x01, 0x3a, 0x00, 0x2a,
    0xfa, 0xd1, 0x04, 0x48, 0x00, 0x28, 0x01, 0xd1,
    0x01, 0x48, 0x85, 0x46, 0x70, 0x47, 0xc0, 0x46,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
)

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

    if ($Bytes.Length -eq 0) {
        return
    }
    $Link.Write($Bytes, 0, $Bytes.Length)
    $Link.BaseStream.Flush()
}

function Write-Paced {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [byte[]]$Bytes,
        [int]$PacketBytes,
        [int]$InterPacketDelayMilliseconds
    )

    if ($Bytes.Length -eq 0) {
        return
    }

    $offset = 0
    while ($offset -lt $Bytes.Length) {
        $count = [Math]::Min($PacketBytes, $Bytes.Length - $offset)
        $Link.Write($Bytes, $offset, $count)
        $Link.BaseStream.Flush()
        $offset += $count

        if ($offset -lt $Bytes.Length) {
            Start-Sleep -Milliseconds $InterPacketDelayMilliseconds
        }
    }
}

function Read-Exact {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [int]$Count,
        [int]$TimeoutMilliseconds,
        [string]$Description
    )

    $result = New-Object byte[] $Count
    $offset = 0
    $deadline = (Get-Date).AddMilliseconds($TimeoutMilliseconds)
    while ($offset -lt $Count) {
        $remaining = [int](($deadline - (Get-Date)).TotalMilliseconds)
        if ($remaining -le 0) {
            throw "$Description timed out after receiving $offset/$Count byte(s)"
        }

        $Link.ReadTimeout = [Math]::Max(1, $remaining)
        try {
            $value = $Link.ReadByte()
        }
        catch [System.TimeoutException] {
            continue
        }

        if ($value -ge 0) {
            $result[$offset++] = [byte]$value
        }
    }
    return $result
}

function Read-Capture {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [int]$DurationMilliseconds,
        [int]$MaximumBytes
    )

    $bytes = [System.Collections.Generic.List[byte]]::new()
    $discardedByteCount = 0
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
            if ($bytes.Count -lt $MaximumBytes) {
                $bytes.Add([byte]$value)
            } else {
                $discardedByteCount++
            }
        }
    }

    return [pscustomobject]@{
        Bytes = $bytes.ToArray()
        DiscardedByteCount = $discardedByteCount
    }
}

function ConvertTo-ByteSequenceText {
    param([byte[]]$Bytes)

    if ($null -eq $Bytes -or $Bytes.Length -eq 0) {
        return '<none>'
    }
    return [System.BitConverter]::ToString($Bytes)
}

function Format-Command {
    param(
        [char]$Command,
        [uint32]$Address,
        [uint32]$Size
    )

    return [System.Text.Encoding]::ASCII.GetBytes(
        ('{0}{1:X8},{2:X8}#' -f $Command, $Address, $Size))
}

function Send-SambaWord {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [uint32]$Address,
        [uint32]$Value,
        [string]$Description
    )

    # SAM-BA W has no reply. Keep BOSSA's inter-command delay so the next
    # transfer does not overrun the bootloader's command parser.
    Write-All `
        -Link $Link `
        -Bytes ([System.Text.Encoding]::ASCII.GetBytes(
            ('W{0:X8},{1:X8}#' -f $Address, $Value)))
    Start-Sleep -Milliseconds 10
    Write-Host ("{0} address=0x{1:X8} value=0x{2:X8}" -f $Description, $Address, $Value)
}

function Send-SambaWrite {
    param(
        [System.IO.Ports.SerialPort]$Bridge,
        [System.IO.Ports.SerialPort]$Reference,
        [uint32]$Address,
        [byte[]]$Payload,
        [string]$Description
    )

    $Reference.DiscardInBuffer()
    Write-All -Link $Bridge -Bytes (Format-Command -Command 'S' -Address $Address -Size $Payload.Length)
    # Match BOSSA's USB transport separation so the command and data do not
    # reach the bootloader in one coalesced CDC transfer.
    Start-Sleep -Milliseconds 1
    Write-Paced `
        -Link $Bridge `
        -Bytes $Payload `
        -PacketBytes $PayloadPacketBytes `
        -InterPacketDelayMilliseconds $PayloadInterPacketDelayMilliseconds
    $packetCount = [int][Math]::Ceiling($Payload.Length / [double]$PayloadPacketBytes)
    Write-Host "$Description direct_payload_bytes=$($Payload.Length) packets=$packetCount packet_bytes=$PayloadPacketBytes delay_ms=$PayloadInterPacketDelayMilliseconds"
}

function Expect-SambaResponse {
    param(
        [System.IO.Ports.SerialPort]$Bridge,
        [System.IO.Ports.SerialPort]$Reference,
        [byte[]]$Command,
        [byte]$ExpectedFirstByte,
        [int]$TimeoutMilliseconds,
        [string]$Description
    )

    $Reference.DiscardInBuffer()
    Write-All -Link $Bridge -Bytes $Command
    $reply = Read-Exact `
        -Link $Reference `
        -Count 3 `
        -TimeoutMilliseconds $TimeoutMilliseconds `
        -Description $Description
    if ($reply[0] -ne $ExpectedFirstByte -or
        $reply[1] -ne 0x0a -or
        $reply[2] -ne 0x0d) {
        throw "$Description returned unexpected reply $(ConvertTo-ByteSequenceText -Bytes $reply)"
    }
}

function Initialize-Samba {
    param(
        [System.IO.Ports.SerialPort]$Bridge,
        [System.IO.Ports.SerialPort]$Reference
    )

    Start-Sleep -Milliseconds 100
    $Reference.DiscardInBuffer()

    foreach ($index in 1..2) {
        $Reference.DiscardInBuffer()
        Write-All -Link $Bridge -Bytes ([byte[]](0x80))
        $reply = Read-Capture `
            -Link $Reference `
            -DurationMilliseconds $ReadTimeoutMilliseconds `
            -MaximumBytes 16
        Write-Host "SAMBA_AUTO_BAUD_$index=$(ConvertTo-ByteSequenceText -Bytes $reply.Bytes)"
    }

    $Reference.DiscardInBuffer()
    Write-All -Link $Bridge -Bytes ([byte[]](0x23))
    $confirm = Read-Capture `
        -Link $Reference `
        -DurationMilliseconds $ReadTimeoutMilliseconds `
        -MaximumBytes 16
    Write-Host "SAMBA_AUTO_BAUD_CONFIRM=$(ConvertTo-ByteSequenceText -Bytes $confirm.Bytes)"

    $Reference.DiscardInBuffer()
    Write-All -Link $Bridge -Bytes ([byte[]](0x4e, 0x23))
    $binaryModeReply = Read-Exact `
        -Link $Reference `
        -Count 2 `
        -TimeoutMilliseconds $ReadTimeoutMilliseconds `
        -Description 'SAM-BA binary mode'
    if ($binaryModeReply[0] -ne 0x0a -or $binaryModeReply[1] -ne 0x0d) {
        throw "SAM-BA binary mode returned unexpected reply $(ConvertTo-ByteSequenceText -Bytes $binaryModeReply)"
    }
    Write-Host "SAMBA_BINARY_MODE=$(ConvertTo-ByteSequenceText -Bytes $binaryModeReply)"
}

function Get-SambaVersion {
    param(
        [System.IO.Ports.SerialPort]$Bridge,
        [System.IO.Ports.SerialPort]$Reference
    )

    $Reference.DiscardInBuffer()
    Write-All -Link $Bridge -Bytes ([byte[]](0x56, 0x23))
    $capture = Read-Capture `
        -Link $Reference `
        -DurationMilliseconds $ReadTimeoutMilliseconds `
        -MaximumBytes 128
    if ($capture.DiscardedByteCount -ne 0) {
        throw 'SAM-BA version reply exceeded the capture limit'
    }

    $version = [System.Text.Encoding]::ASCII.GetString($capture.Bytes).TrimEnd("`r", "`n")
    if ($version -notmatch '^Arduino Bootloader \(SAM-BA extended\) [0-9]+\.[0-9]+ \[Arduino:IKXYZ\]$') {
        throw "unexpected SAM-BA version reply: $(ConvertTo-ByteSequenceText -Bytes $capture.Bytes)"
    }
    return $version
}

function Read-RuntimeText {
    param(
        [System.IO.Ports.SerialPort]$Reference,
        [int]$DurationMilliseconds
    )

    $capture = Read-Capture `
        -Link $Reference `
        -DurationMilliseconds $DurationMilliseconds `
        -MaximumBytes 8192
    if ($capture.DiscardedByteCount -ne 0) {
        throw "runtime capture exceeded the buffer by $($capture.DiscardedByteCount) byte(s)"
    }
    return [System.Text.Encoding]::ASCII.GetString($capture.Bytes)
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
    $LongReadTimeoutMilliseconds -le 0 -or
    $RuntimeCaptureMilliseconds -lt 2000 -or
    $PayloadPacketBytes -le 0 -or
    $PayloadInterPacketDelayMilliseconds -lt 0) {
    throw 'timeouts must be positive; RuntimeCaptureMilliseconds must be at least 2000; PayloadPacketBytes must be positive; PayloadInterPacketDelayMilliseconds must not be negative'
}
$image = [System.IO.Path]::GetFullPath($ImagePath)
if (-not (Test-Path -LiteralPath $image -PathType Leaf)) {
    throw "compiled RA4M1 image does not exist: $image"
}

[byte[]]$imageBytes = [System.IO.File]::ReadAllBytes($image)
if ($imageBytes.Length -eq 0) {
    throw 'compiled RA4M1 image is empty'
}
if (($imageBytes.Length + $applicationFlashAddress) -gt $flashEndAddress) {
    throw "image is too large for the RA4M1 application Flash range: $($imageBytes.Length) bytes"
}
if ($wordCopyApplet.Length -ne $writeBufferAddress) {
    throw 'BOSSA compatibility staging layout is inconsistent'
}
if (($writeBufferSize % 4) -ne 0) {
    throw 'BOSSA staging buffer size must be word aligned'
}

$bridgeDevice = Require-SerialPort -Port $BridgePort -InstanceIdPattern 'VID_2341&PID_1002'
$referenceDevice = if ($Direct) {
    $bridgeDevice
} else {
    Require-SerialPort -Port $ReferencePort -InstanceIdPattern 'VID_0403.*PID_6010'
}
$imageHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $image).Hash.ToLowerInvariant()

Write-Host "Bridge:    $BridgePort $($bridgeDevice.InstanceId)"
if ($Direct) {
    Write-Host "Reference: $BridgePort direct CDC RX"
} else {
    Write-Host "Reference: $ReferencePort $($referenceDevice.InstanceId)"
}
Write-Host "Image:     $image"
Write-Host "Image size: $($imageBytes.Length)"
Write-Host "Image SHA256: $imageHash"
Write-Host 'Target: RA4M1 application Flash only. Upload erases and replaces the current RA4M1 Sketch.'
Write-Host 'SAM-BA application address is logical 0x00000000; the bootloader maps it to the RA4M1 application region.'
Write-Host 'Relay route: RA4M1 UART (RtsEnable=false, physical RTS# HIGH, relay released).'

$reference = $null
if (-not $Direct) {
    $reference = [System.IO.Ports.SerialPort]::new($ReferencePort, 230400)
    $reference.DtrEnable = $false
    $reference.RtsEnable = $false
    $reference.ReadTimeout = $ReadTimeoutMilliseconds
    $reference.WriteTimeout = $ReadTimeoutMilliseconds
    $reference.Open()
    $reference.RtsEnable = $false
}

$bridge = $null
$runtimeRequested = $false

try {
    Start-Sleep -Milliseconds 50
    Set-BridgeLineCoding `
        -Port $BridgePort `
        -BaudRate 1200 `
        -SettleMilliseconds $BootstrapSettleMilliseconds

    $bridge = [System.IO.Ports.SerialPort]::new($BridgePort, 230400)
    $bridge.DtrEnable = $false
    $bridge.RtsEnable = $false
    $bridge.ReadTimeout = $ReadTimeoutMilliseconds
    $bridge.WriteTimeout = $ReadTimeoutMilliseconds
    $bridge.Open()
    if ($Direct) {
        $reference = $bridge
    }

    Initialize-Samba -Bridge $bridge -Reference $reference
    $version = Get-SambaVersion -Bridge $bridge -Reference $reference
    Write-Host "SAMBA_VERSION=$version"

    Send-SambaWrite `
        -Bridge $bridge `
        -Reference $reference `
        -Address 0 `
        -Payload $wordCopyApplet `
        -Description 'BOSSA compatibility applet'
    Send-SambaWord `
        -Link $bridge `
        -Address $wordCopyWordsAddress `
        -Value ([uint32]($writeBufferSize / 4)) `
        -Description 'BOSSA compatibility applet words'
    Send-SambaWord `
        -Link $bridge `
        -Address $wordCopyStackAddress `
        -Value 0 `
        -Description 'BOSSA compatibility applet stack'

    Expect-SambaResponse `
        -Bridge $bridge `
        -Reference $reference `
        -Command ([System.Text.Encoding]::ASCII.GetBytes(('X{0:X8}#' -f $applicationFlashAddress))) `
        -ExpectedFirstByte ([byte][char]'X') `
        -TimeoutMilliseconds $LongReadTimeoutMilliseconds `
        -Description 'RA4M1 application Flash erase'
    Write-Host "RA4_FLASH_ERASED_FROM=0x$('{0:X8}' -f $applicationFlashAddress)"

    [uint32]$flashAddress = $applicationFlashAddress
    $chunkNumber = 0
    for ($offset = 0; $offset -lt $imageBytes.Length; $offset += $writeBufferSize) {
        $actualSize = [Math]::Min($writeBufferSize, $imageBytes.Length - $offset)
        $programSize = [int]([Math]::Ceiling($actualSize / [double]$flashPageSize) * $flashPageSize)
        $staged = New-Object byte[] $programSize
        [Array]::Copy($imageBytes, $offset, $staged, 0, $actualSize)

        Send-SambaWrite `
            -Bridge $bridge `
            -Reference $reference `
            -Address $writeBufferAddress `
            -Payload $staged `
            -Description ("Flash chunk $chunkNumber staging")

        Expect-SambaResponse `
            -Bridge $bridge `
            -Reference $reference `
            -Command ([System.Text.Encoding]::ASCII.GetBytes(('Y{0:X8},0#' -f $writeBufferAddress))) `
            -ExpectedFirstByte ([byte][char]'Y') `
            -TimeoutMilliseconds $ReadTimeoutMilliseconds `
            -Description ("Flash chunk $chunkNumber select staging buffer")

        Expect-SambaResponse `
            -Bridge $bridge `
            -Reference $reference `
            -Command (Format-Command -Command 'Y' -Address $flashAddress -Size $programSize) `
            -ExpectedFirstByte ([byte][char]'Y') `
            -TimeoutMilliseconds $LongReadTimeoutMilliseconds `
            -Description ("Flash chunk $chunkNumber write 0x$('{0:X8}' -f $flashAddress)")

        Write-Host ("RA4_FLASH_CHUNK_PASS index={0} address=0x{1:X8} programmed={2} payload={3}" -f $chunkNumber, $flashAddress, $programSize, $actualSize)
        $flashAddress += $programSize
        $chunkNumber++
    }

    Write-Host "RA4_FLASH_WRITE_VERIFY_PASS chunks=$chunkNumber"

    $reference.DiscardInBuffer()
    Write-All -Link $bridge -Bytes ([System.Text.Encoding]::ASCII.GetBytes('K#'))
    Write-Host 'SAMBA_RESET_COMMAND=K#'
    Start-Sleep -Milliseconds 50

    # The UNO R4 WiFi bootloader reserves 2400 bps for RA4 ROM DFU. A
    # successful BOSSA-style upload must reset with K# and switch directly
    # from SAM-BA 230400 to the Sketch's 115200 user-UART baud.
    $reference.BaudRate = 115200
    $bridge.BaudRate = 115200
    Start-Sleep -Milliseconds $RuntimeSettleMilliseconds
    if ($Direct) {
        # Reopen CDC after the SAM-BA-to-Sketch baud handoff. Keeping the
        # bootloader-era SerialPort open can leave a stale NUL in its input
        # buffer and miss the application's first CDC data transfer.
        $bridge.Close()
        $bridge = $null
        Start-Sleep -Milliseconds 100
        $reference = [System.IO.Ports.SerialPort]::new($BridgePort, 115200)
        $reference.DtrEnable = $false
        $reference.RtsEnable = $false
        $reference.ReadTimeout = $ReadTimeoutMilliseconds
        $reference.WriteTimeout = $ReadTimeoutMilliseconds
        $reference.Open()
        $reference.DiscardInBuffer()
        $bridge = $reference
        Start-Sleep -Milliseconds $RuntimeSettleMilliseconds
        Write-Host 'CDC_DIRECT_RUNTIME_REOPEN=115200'
    }
    $runtimeRequested = $true
    Write-Host 'RA4 runtime requested by K#; user UART restored directly to 115200.'
}
finally {
    if (-not $Direct -and $null -ne $bridge) {
        $bridge.Close()
        $bridge = $null
    }
}

try {
    if (-not $runtimeRequested) {
        throw 'RA4M1 runtime recovery was not requested'
    }

    if ($SkipRuntimeSmoke) {
        Write-Host 'RA4 runtime requested by K#; runtime verification delegated to caller.'
        return
    }

    if ($ExplicitApplicationReset) {
        Request-Ra4ApplicationReset
        Start-Sleep -Milliseconds $RuntimeSettleMilliseconds
        Write-Host 'RA4 application reset feature requested after K# (explicit A/B mode).'
    } else {
        Write-Host 'RA4 application reset feature skipped; relying on SAM-BA K# handoff.'
    }

    $runtime = Read-RuntimeText `
        -Reference $reference `
        -DurationMilliseconds $RuntimeCaptureMilliseconds
    Write-Host "RA4_RUNTIME_CAPTURE_BEGIN"
    Write-Host $runtime
    Write-Host "RA4_RUNTIME_CAPTURE_END"

    if ($runtime -notmatch '(?m)^RA4_HEARTBEAT:BOOT\r?$') {
        throw 'RA4M1 heartbeat boot marker was not observed on the reference RX path'
    }
    if ([regex]::Matches($runtime, '(?m)^RA4_HEARTBEAT:[0-9]+\r?$').Count -lt 2) {
        throw 'fewer than two RA4M1 heartbeat markers were observed on the reference RX path'
    }

    $runtimeTx = $reference
    if (-not $Direct) {
        $bridge = [System.IO.Ports.SerialPort]::new($BridgePort, 115200)
        $bridge.DtrEnable = $false
        $bridge.RtsEnable = $false
        $bridge.ReadTimeout = $ReadTimeoutMilliseconds
        $bridge.WriteTimeout = $ReadTimeoutMilliseconds
        $bridge.Open()
        $runtimeTx = $bridge
    }
    try {
        $noncePrefix = if ($Direct) { 'bl616cl-direct-' } else { 'bl616cl-split-' }
        $nonce = $noncePrefix + [Guid]::NewGuid().ToString('N')
        $reference.DiscardInBuffer()
        Write-All -Link $runtimeTx -Bytes ([System.Text.Encoding]::ASCII.GetBytes($nonce + "`n"))
        $echo = Read-RuntimeText -Reference $reference -DurationMilliseconds $ReadTimeoutMilliseconds
        Write-Host "RA4_ECHO_CAPTURE=$echo"
        if ($echo -notmatch ('(?m)^RA4_ECHO:' + [regex]::Escape($nonce) + '\r?$')) {
            throw "RA4M1 nonce echo was not observed: expected RA4_ECHO:$nonce"
        }
        if ($Direct) {
            Write-Host "RA4_DIRECT_SAMBA_UPLOAD_AND_RUNTIME_PASS nonce=$nonce"
        } else {
            Write-Host "RA4_SPLIT_SAMBA_UPLOAD_AND_RUNTIME_PASS nonce=$nonce"
        }
    }
    finally {
        if (-not $Direct -and $null -ne $bridge -and $bridge.IsOpen) {
            $bridge.Close()
            $bridge = $null
        }
    }
}
finally {
    if ($null -ne $bridge -and $bridge.IsOpen) {
        $bridge.Close()
    }
    if ($null -ne $reference -and $reference.IsOpen) {
        # Keep the relay released so COM35 remains the runtime reference RX path.
        if (-not $Direct) {
            $reference.RtsEnable = $false
        }
        $reference.Close()
        if (-not $Direct) {
            Write-Host 'Relay state: RA4M1 UART runtime route retained (RTS# HIGH, relay released).'
        }
    }
}
