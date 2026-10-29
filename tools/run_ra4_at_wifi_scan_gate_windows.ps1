[CmdletBinding()]
param(
    [string]$BridgePort = 'COM34',
    [string]$ReferencePort = 'COM35',
    [int]$ScanTimeoutMilliseconds = 25000,
    [int]$StatusTimeoutMilliseconds = 2000,
    [int]$MaximumResponseBytes = 24576,
    [int]$RuntimeSettleMilliseconds = 2000,
    [switch]$UploadProxy
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

function Read-UntilTerminal {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [int]$TimeoutMilliseconds,
        [int]$MaximumBytes
    )

    $received = New-Object System.Collections.Generic.List[byte]
    $deadline = (Get-Date).AddMilliseconds($TimeoutMilliseconds)
    $terminal = [System.Text.Encoding]::ASCII.GetBytes("OK`r`n")
    while ((Get-Date) -lt $deadline) {
        $remaining = [int](($deadline - (Get-Date)).TotalMilliseconds)
        if ($remaining -le 0) {
            break
        }
        $Link.ReadTimeout = [Math]::Max(1, [Math]::Min(100, $remaining))
        try {
            $value = $Link.ReadByte()
        }
        catch [System.TimeoutException] {
            continue
        }
        if ($value -lt 0) {
            continue
        }
        $received.Add([byte]$value)
        if ($received.Count -gt $MaximumBytes) {
            throw "scan response exceeded $MaximumBytes byte limit"
        }
        if ($received.Count -ge $terminal.Length) {
            $matched = $true
            for ($offset = 0; $offset -lt $terminal.Length; ++$offset) {
                if ($received[$received.Count - $terminal.Length + $offset] -ne $terminal[$offset]) {
                    $matched = $false
                    break
                }
            }
            if ($matched) {
                return $received.ToArray()
            }
        }
    }
    throw "scan response timed out after $TimeoutMilliseconds ms with $($received.Count) byte(s)"
}

function Read-Exact {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [int]$Count,
        [int]$TimeoutMilliseconds
    )

    $reply = New-Object byte[] $Count
    $offset = 0
    $deadline = (Get-Date).AddMilliseconds($TimeoutMilliseconds)
    while ($offset -lt $Count) {
        $remaining = [int](($deadline - (Get-Date)).TotalMilliseconds)
        if ($remaining -le 0) {
            break
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
    if ($offset -eq $Count) {
        return $reply
    }
    if ($offset -eq 0) {
        return [byte[]]@()
    }
    return $reply[0..($offset - 1)]
}

function Test-ByteSequenceEqual {
    param(
        [byte[]]$Actual,
        [byte[]]$Expected
    )

    if ($Actual.Length -ne $Expected.Length) {
        return $false
    }
    for ($index = 0; $index -lt $Actual.Length; ++$index) {
        if ($Actual[$index] -ne $Expected[$index]) {
            return $false
        }
    }
    return $true
}

function Get-WifiScanRowCount {
    param([byte[]]$Response)

    $terminal = [System.Text.Encoding]::ASCII.GetBytes("OK`r`n")
    if (Test-ByteSequenceEqual -Actual $Response -Expected $terminal) {
        return 0
    }

    $prefix = [System.Text.Encoding]::ASCII.GetBytes('+WIFISCAN: ')
    if ($Response.Length -le ($prefix.Length + 2 + $terminal.Length)) {
        throw 'scan response did not match the expected AT framing'
    }
    for ($index = 0; $index -lt $prefix.Length; ++$index) {
        if ($Response[$index] -ne $prefix[$index]) {
            throw 'scan response did not match the expected AT framing'
        }
    }
    for ($index = 0; $index -lt $terminal.Length; ++$index) {
        if ($Response[$Response.Length - $terminal.Length + $index] -ne $terminal[$index]) {
            throw 'scan response did not match the expected AT framing'
        }
    }

    $separatorAt = -1
    for ($index = $prefix.Length; $index -lt ($Response.Length - $terminal.Length); ++$index) {
        if ($Response[$index] -eq [byte][char]'|') {
            $separatorAt = $index
            break
        }
    }
    if ($separatorAt -lt 0) {
        throw 'scan response did not contain a payload length'
    }
    $lengthText = [System.Text.Encoding]::ASCII.GetString(
        $Response,
        $prefix.Length,
        $separatorAt - $prefix.Length
    )
    $declaredLength = 0
    if (-not [int]::TryParse($lengthText, [ref]$declaredLength) -or $declaredLength -lt 0) {
        throw 'scan response payload length was not decimal'
    }
    $payloadLength = $Response.Length - $separatorAt - 1 - $terminal.Length
    if ($declaredLength -ne $payloadLength) {
        throw 'scan response payload length did not match its body'
    }
    $payload = [System.Text.Encoding]::ASCII.GetString($Response, $separatorAt + 1, $payloadLength)
    $rows = $payload -split "`r`n"
    if ($rows.Length -lt 2 -or $rows[$rows.Length - 1] -ne '') {
        throw 'scan response rows were not CRLF terminated'
    }
    foreach ($row in $rows[0..($rows.Length - 2)]) {
        $fields = $row -split ' \| '
        if ($fields.Length -ne 5 -or [string]::IsNullOrWhiteSpace($fields[0])) {
            throw 'scan response contained an invalid row shape'
        }
        if ($fields[1] -notmatch '^[0-9a-f]{2}(:[0-9a-f]{2}){5}$') {
            throw 'scan response contained an invalid BSSID field'
        }
        [void][int]::Parse($fields[2])
        [void][int]::Parse($fields[3])
        if ([string]::IsNullOrWhiteSpace($fields[4])) {
            throw 'scan response contained an invalid encryption field'
        }
    }
    return $rows.Length - 1
}

if (
    $ScanTimeoutMilliseconds -le 0 -or
    $StatusTimeoutMilliseconds -le 0 -or
    $MaximumResponseBytes -lt 4 -or
    $RuntimeSettleMilliseconds -lt 0
) {
    throw 'invalid timeout or response size argument'
}

$bridgeDevice = Require-SerialPort -Port $BridgePort -InstanceIdPattern 'VID_2341&PID_1002'
$referenceDevice = Require-SerialPort -Port $ReferencePort -InstanceIdPattern 'VID_0403.*PID_6010'
Write-Host "Bridge:    $BridgePort $($bridgeDevice.InstanceId)"
Write-Host "Reference: $ReferencePort $($referenceDevice.InstanceId)"
Write-Host 'Relay route: RA4M1 UART (RtsEnable=false, physical RTS# HIGH, relay released).'
Write-Host 'Privacy: scan rows are validated in memory and never printed or written.'

if ($UploadProxy) {
    $proxyGate = Join-Path $PSScriptRoot 'run_ra4_at_uart_gate_windows.ps1'
    & $proxyGate `
        -BridgePort $BridgePort `
        -ReferencePort $ReferencePort `
        -RuntimeSettleMilliseconds $RuntimeSettleMilliseconds
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

    $scanRequest = [System.Text.Encoding]::ASCII.GetBytes("AT+WIFISCAN`r`n")
    $startedAt = Get-Date
    $bridge.Write($scanRequest, 0, $scanRequest.Length)
    $bridge.BaseStream.Flush()
    $scanResponse = Read-UntilTerminal `
        -Link $reference `
        -TimeoutMilliseconds $ScanTimeoutMilliseconds `
        -MaximumBytes $MaximumResponseBytes
    $elapsedMs = ((Get-Date) - $startedAt).TotalMilliseconds
    $rowCount = Get-WifiScanRowCount -Response $scanResponse
    Write-Host "WIFISCAN_ROWS=$rowCount"
    Write-Host "WIFISCAN_RESPONSE_BYTES=$($scanResponse.Length)"
    Write-Host ('WIFISCAN_ELAPSED_MS={0:F3}' -f $elapsedMs)
    Write-Host 'WIFISCAN_TERMINAL=OK'

    $reference.DiscardInBuffer()
    $statusRequest = [System.Text.Encoding]::ASCII.GetBytes("AT+GETSTATUS?`r`n")
    $expectedStatus = [System.Text.Encoding]::ASCII.GetBytes("+GETSTATUS: 2`r`nOK`r`n")
    $bridge.Write($statusRequest, 0, $statusRequest.Length)
    $bridge.BaseStream.Flush()
    $statusResponse = Read-Exact `
        -Link $reference `
        -Count $expectedStatus.Length `
        -TimeoutMilliseconds $StatusTimeoutMilliseconds
    if (-not (Test-ByteSequenceEqual -Actual $statusResponse -Expected $expectedStatus)) {
        throw "unexpected post-scan GETSTATUS response: received $($statusResponse.Length) byte(s), expected scan-complete status"
    }
    Write-Host 'GETSTATUS_AFTER_SCAN=2'
    Write-Host 'RA4_AT_WIFISCAN_GPIO6_GPIO7_PASS'
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
