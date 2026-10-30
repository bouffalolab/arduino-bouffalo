[CmdletBinding()]
param(
    [string]$ImagePath = 'build/ra4-at-transcript-proxy/ra4-at-transcript-proxy.ino.bin',
    [string]$BridgePort = 'COM34',
    [string]$ReferencePort = 'COM35',
    [string]$Uploader = '',
    [int]$ResponseTimeoutMilliseconds = 2000,
    [int]$RuntimeSettleMilliseconds = 2000,
    [switch]$SkipUpload
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
            $reply[$offset++] = [byte]$value
        }
    }
    return $reply
}

function Format-Bytes {
    param([byte[]]$Bytes)

    if ($null -eq $Bytes -or $Bytes.Length -eq 0) {
        return '<none>'
    }
    return [System.BitConverter]::ToString($Bytes)
}

function Test-ByteSequenceEqual {
    param(
        [byte[]]$Actual,
        [byte[]]$Expected
    )

    if ($Actual.Length -ne $Expected.Length) {
        return $false
    }
    for ($index = 0; $index -lt $Actual.Length; $index++) {
        if ($Actual[$index] -ne $Expected[$index]) {
            return $false
        }
    }
    return $true
}

function Send-ExactRequest {
    param(
        [System.IO.Ports.SerialPort]$Bridge,
        [System.IO.Ports.SerialPort]$Reference,
        [byte[]]$Request,
        [byte[]]$Expected,
        [int]$TimeoutMilliseconds,
        [string]$Name
    )

    $Reference.DiscardInBuffer()
    $Bridge.Write($Request, 0, $Request.Length)
    $Bridge.BaseStream.Flush()
    $actual = Read-Exact `
        -Link $Reference `
        -Count $Expected.Length `
        -TimeoutMilliseconds $TimeoutMilliseconds `
        -Description $Name

    Write-Host "$Name request=$(Format-Bytes -Bytes $Request)"
    Write-Host "$Name response=$(Format-Bytes -Bytes $actual)"
    if (-not (Test-ByteSequenceEqual -Actual $actual -Expected $Expected)) {
        throw "$Name returned unexpected response: expected $(Format-Bytes -Bytes $Expected), got $(Format-Bytes -Bytes $actual)"
    }
}

if ($ResponseTimeoutMilliseconds -le 0 -or $RuntimeSettleMilliseconds -lt 0) {
    throw 'ResponseTimeoutMilliseconds must be positive and RuntimeSettleMilliseconds must not be negative'
}

$image = [System.IO.Path]::GetFullPath($ImagePath)
if (-not $SkipUpload -and -not (Test-Path -LiteralPath $image -PathType Leaf)) {
    throw "compiled RA4M1 AT proxy image does not exist: $image"
}

$bridgeDevice = Require-SerialPort `
    -Port $BridgePort `
    -InstanceIdPattern 'VID_2341&PID_1002'
$referenceDevice = Require-SerialPort `
    -Port $ReferencePort `
    -InstanceIdPattern 'VID_0403.*PID_6010'

Write-Host "Bridge:    $BridgePort $($bridgeDevice.InstanceId)"
Write-Host "Reference: $ReferencePort $($referenceDevice.InstanceId)"
Write-Host 'Relay route: RA4M1 runtime (RtsEnable=false, physical RTS# HIGH, relay released).'

if (-not $SkipUpload) {
    if ([string]::IsNullOrWhiteSpace($Uploader)) {
        $Uploader = Join-Path $PSScriptRoot 'upload_ra4_samba_split_windows.ps1'
    } else {
        $Uploader = [System.IO.Path]::GetFullPath($Uploader)
    }
    if (-not (Test-Path -LiteralPath $Uploader -PathType Leaf)) {
        throw "RA4M1 split uploader does not exist: $Uploader"
    }

    Write-Host "UPLOAD_IMAGE=$image"
    & $Uploader `
        -ImagePath $image `
        -BridgePort $BridgePort `
        -ReferencePort $ReferencePort `
        -RuntimeSettleMilliseconds $RuntimeSettleMilliseconds `
        -SkipRuntimeSmoke
}

$bridge = [System.IO.Ports.SerialPort]::new($BridgePort, 115200)
$reference = [System.IO.Ports.SerialPort]::new($ReferencePort, 115200)
$bridge.DtrEnable = $false
$bridge.RtsEnable = $false
$reference.DtrEnable = $false
$reference.RtsEnable = $false
$bridge.ReadTimeout = 1000
$bridge.WriteTimeout = 1000
$reference.ReadTimeout = 100
$reference.WriteTimeout = 1000
$bridge.Open()
$reference.Open()
$reference.RtsEnable = $false

try {
    $bridge.DiscardInBuffer()
    $reference.DiscardInBuffer()

    Send-ExactRequest `
        -Bridge $bridge `
        -Reference $reference `
        -Request ([System.Text.Encoding]::ASCII.GetBytes("AT`r`n")) `
        -Expected ([System.Text.Encoding]::ASCII.GetBytes("OK`r`n")) `
        -TimeoutMilliseconds $ResponseTimeoutMilliseconds `
        -Name 'AT'

    Send-ExactRequest `
        -Bridge $bridge `
        -Reference $reference `
        -Request ([System.Text.Encoding]::ASCII.GetBytes("AT+FWVERSION?`r`n")) `
        -Expected ([System.Text.Encoding]::ASCII.GetBytes("+FWVERSION: 0.6.0`r`nOK`r`n")) `
        -TimeoutMilliseconds $ResponseTimeoutMilliseconds `
        -Name 'AT+FWVERSION?'

    Send-ExactRequest `
        -Bridge $bridge `
        -Reference $reference `
        -Request ([System.Text.Encoding]::ASCII.GetBytes("AT+GETSTATUS?`r`n")) `
        -Expected ([System.Text.Encoding]::ASCII.GetBytes("+GETSTATUS: 0`r`nOK`r`n")) `
        -TimeoutMilliseconds $ResponseTimeoutMilliseconds `
        -Name 'AT+GETSTATUS?'

    Write-Host 'RA4_AT_UART_GPIO6_GPIO7_PASS'
}
finally {
    if ($reference.IsOpen) {
        $reference.RtsEnable = $false
        $reference.Close()
    }
    if ($bridge.IsOpen) {
        $bridge.Close()
    }
    Write-Host 'Relay state: RA4M1 runtime route retained (RTS# HIGH, relay released).'
}
