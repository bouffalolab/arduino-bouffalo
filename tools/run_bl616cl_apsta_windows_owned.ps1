[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$BridgePort,

    [Parameter(Mandatory = $true)]
    [string]$UpstreamSsidBase64,

    [Parameter(Mandatory = $true)]
    [string]$UpstreamPasswordBase64,

    [Parameter(Mandatory = $true)]
    [string]$SoftApSsidBase64,

    [Parameter(Mandatory = $true)]
    [string]$SoftApPasswordBase64,

    [string]$WlanInterface = 'WLAN',

    [int]$ReadyTimeoutMs = 45000,

    [int]$TrafficTimeoutMs = 60000,

    [int]$WlanConnectTimeoutMs = 30000,

    [int]$IoTimeoutMs = 10000,

    [int]$CdcRecoveryTimeoutMs = 10000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Decode-AsciiBase64 {
    param([string]$Value)

    return [System.Text.Encoding]::ASCII.GetString(
        [System.Convert]::FromBase64String($Value)
    )
}

function Encode-HexAscii {
    param([string]$Value)

    return [System.BitConverter]::ToString(
        [System.Text.Encoding]::ASCII.GetBytes($Value)
    ).Replace('-', '').ToLowerInvariant()
}

function Require-BridgePort {
    param([string]$Port)

    if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $Port) {
        throw "$Port is not currently enumerated"
    }

    $portPattern = '\(' + [regex]::Escape($Port) + '\)'
    $device = Get-PnpDevice -PresentOnly |
        Where-Object { $_.FriendlyName -match $portPattern } |
        Select-Object -First 1
    if ($null -eq $device -or
        $device.Status -ne 'OK' -or
        $device.InstanceId -notmatch 'VID_2341&PID_1002&MI_00\\') {
        throw "$Port is not the healthy final-carrier BL616CL CDC interface"
    }
}

function Print-Ra4Markers {
    param([string]$Text)

    foreach ($line in $Text -split "`r?`n") {
        if ($line.StartsWith('RA4_APSTA:')) {
            Write-Output $line
        }
    }
}

function Invoke-ApTelemetry {
    param([string]$Reason)

    Write-Output "WINDOWS_APSTA_TELEMETRY_BEGIN=$Reason"
    try {
        [void][CmsisDapWindowsProbe]::RunApStartTelemetry()
        [void][CmsisDapWindowsProbe]::RunApStartWatchdogTelemetry()
        [void][CmsisDapWindowsProbe]::RunApStartWpaEventPathTelemetry()
        [void][CmsisDapWindowsProbe]::RunApStartCreateApTelemetry()
        [void][CmsisDapWindowsProbe]::RunApStartHostapdLifecycleTelemetry()
        [void][CmsisDapWindowsProbe]::RunApStartHostapdStateUpdateTelemetry()
        [void][CmsisDapWindowsProbe]::RunApStartStatusCodeTelemetry()
        Write-Output "WINDOWS_APSTA_TELEMETRY_PASS=$Reason"
    }
    catch {
        Write-Output "WINDOWS_APSTA_TELEMETRY_FAILED=$Reason"
        Write-Output (
            'WINDOWS_APSTA_TELEMETRY_ERROR=' +
            $_.Exception.GetType().FullName + ': ' +
            ($_.Exception.Message -replace '[\r\n]+', ' ')
        )
    }
}

function Wait-ForApStartWatchdogRecovery {
    param(
        [string]$Port,
        [int]$TimeoutMs
    )

    $portPattern = '\(' + [regex]::Escape($Port) + '\)'
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $cdc = @(
            Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Status -eq 'OK' -and
                $_.FriendlyName -match $portPattern -and
                $_.InstanceId -match 'VID_2341&PID_1002&MI_00\\'
            }
        )
        $hid = @(
            Get-PnpDevice -PresentOnly -Class HIDClass -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Status -eq 'OK' -and
                $_.InstanceId -match '^HID\\VID_2341&PID_1002&MI_02\\'
            }
        )
        if ($cdc.Count -eq 1 -and $hid.Count -eq 1) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    return $false
}

if (
    $ReadyTimeoutMs -le 0 -or
    $TrafficTimeoutMs -le 0 -or
    $WlanConnectTimeoutMs -le 0 -or
    $IoTimeoutMs -le 0 -or
    $CdcRecoveryTimeoutMs -le 0
) {
    throw 'all AP+STA timeouts must be positive'
}

Require-BridgePort $BridgePort

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
$trafficHelper = Join-Path $PSScriptRoot 'run_bl616cl_apsta_wlan_client_windows.ps1'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "CMSIS-DAP helper source does not exist: $source"
}
if (-not (Test-Path -LiteralPath $trafficHelper -PathType Leaf)) {
    throw "AP+STA WLAN helper does not exist: $trafficHelper"
}

Add-Type -Path $source

$upstreamSsid = Decode-AsciiBase64 $UpstreamSsidBase64
$upstreamPassword = Decode-AsciiBase64 $UpstreamPasswordBase64
$softApSsid = Decode-AsciiBase64 $SoftApSsidBase64
$softApPassword = Decode-AsciiBase64 $SoftApPasswordBase64

if (
    [string]::IsNullOrWhiteSpace($upstreamSsid) -or
    [string]::IsNullOrWhiteSpace($upstreamPassword) -or
    [string]::IsNullOrWhiteSpace($softApSsid) -or
    [string]::IsNullOrWhiteSpace($softApPassword)
) {
    throw 'AP+STA credentials must not be empty'
}

$start = 'START ' + (Encode-HexAscii $upstreamSsid) + ' ' +
    (Encode-HexAscii $upstreamPassword) + ' ' +
    (Encode-HexAscii $softApSsid) + ' ' +
    (Encode-HexAscii $softApPassword) + "`n"

$readyMarkers = @(
    'RA4_APSTA:STA_CONNECTED',
    'RA4_APSTA:UPSTREAM_PING_BEFORE_AP_PASS',
    'RA4_APSTA:SOFTAP_READY',
    'RA4_APSTA:UPSTREAM_PING_AFTER_AP_PASS',
    'RA4_APSTA:READY'
)

$serial = [System.IO.Ports.SerialPort]::new($BridgePort, 115200)
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.ReadTimeout = 50
$serial.WriteTimeout = 1000
$received = [System.Text.StringBuilder]::new()
$sawSoftApWait = $false
$capturedWaitTelemetry = $false
$cdcStoppedDuringApStart = $false

Write-Output "WINDOWS_APSTA_OWNER_STARTED=$BridgePort"
Write-Output 'WINDOWS_APSTA_PRIVACY=credentials_and_raw_AT_frames_stay_in_process_memory'

try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $serial.Write($start)
    $serial.BaseStream.Flush()
    Write-Output 'WINDOWS_APSTA_START_SENT'

    $deadline = [DateTime]::UtcNow.AddMilliseconds($ReadyTimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $available = $serial.ReadExisting()
        }
        catch {
            Write-Output (
                'WINDOWS_APSTA_CDC_STOPPED=' +
                $_.Exception.GetType().FullName + ': ' +
                ($_.Exception.Message -replace '[\r\n]+', ' ')
            )
            $cdcStoppedDuringApStart = $true
            break
        }
        if ($available.Length -ne 0) {
            [void]$received.Append($available)
            if ($received.Length -gt 16384) {
                throw 'RA4M1 AP+STA marker capture exceeded 16384 bytes'
            }
            Print-Ra4Markers $available
            $snapshot = $received.ToString()
            if ($snapshot.Contains('RA4_APSTA:RESULT=FAIL:')) {
                throw 'RA4M1 AP+STA Sketch reported a failure'
            }
            if ($snapshot.Contains('RA4_APSTA:STAGE=SOFTAP_WAIT')) {
                $sawSoftApWait = $true
            }
            if ($sawSoftApWait -and -not $capturedWaitTelemetry) {
                $capturedWaitTelemetry = $true
                Invoke-ApTelemetry 'SOFTAP_WAIT'
            }
            $missing = @($readyMarkers | Where-Object {
                -not $snapshot.Contains($_)
            })
            if ($missing.Count -eq 0) {
                break
            }
        }
        Start-Sleep -Milliseconds 10
    }

    if ($cdcStoppedDuringApStart) {
        if ($serial.IsOpen) {
            $serial.Close()
        }
        Write-Output (
            'WINDOWS_APSTA_CDC_RECOVERY_WAIT_MS=' + $CdcRecoveryTimeoutMs
        )
        if (Wait-ForApStartWatchdogRecovery `
            -Port $BridgePort `
            -TimeoutMs $CdcRecoveryTimeoutMs) {
            Write-Output 'WINDOWS_APSTA_CDC_RECOVERED_AFTER_AP_START'
            Invoke-ApTelemetry 'CDC_RECOVERED_AFTER_AP_START'
            throw (
                'BL616CL AP-start watchdog recovered after CDC loss; ' +
                'see retained HID telemetry for the liveness phase'
            )
        }
        throw (
            'BL616CL CDC/HID did not recover after AP-start within ' +
            "$CdcRecoveryTimeoutMs ms"
        )
    }

    $capture = $received.ToString()
    $missing = @($readyMarkers | Where-Object { -not $capture.Contains($_) })
    if ($missing.Count -ne 0) {
        Invoke-ApTelemetry 'READY_TIMEOUT'
        throw 'RA4M1 AP+STA Sketch did not reach READY while Windows owned USB'
    }

    Write-Output 'WINDOWS_APSTA_READY'
    Invoke-ApTelemetry 'READY'

    $serial.Write("TRAFFIC`n")
    $serial.BaseStream.Flush()
    Write-Output 'WINDOWS_APSTA_TRAFFIC_START_SENT'
    $trafficStartDeadline = [DateTime]::UtcNow.AddSeconds(5)
    $trafficStarted = $false
    while ([DateTime]::UtcNow -lt $trafficStartDeadline) {
        $available = $serial.ReadExisting()
        if ($available.Length -ne 0) {
            [void]$received.Append($available)
            Print-Ra4Markers $available
            $capture = $received.ToString()
            if ($capture.Contains('RA4_APSTA:RESULT=FAIL:')) {
                throw 'RA4M1 AP+STA Sketch rejected the traffic start command'
            }
            if ($capture.Contains('RA4_APSTA:TRAFFIC_START')) {
                $trafficStarted = $true
                break
            }
        }
        Start-Sleep -Milliseconds 10
    }
    if (-not $trafficStarted) {
        throw 'RA4M1 AP+STA Sketch did not acknowledge the traffic start command'
    }
    Write-Output 'WINDOWS_APSTA_TRAFFIC_START'

    & $trafficHelper `
        -Interface $WlanInterface `
        -Ssid $softApSsid `
        -Password $softApPassword `
        -ConnectTimeoutMs $WlanConnectTimeoutMs `
        -IoTimeoutMs $IoTimeoutMs
    if ($LASTEXITCODE -ne 0) {
        throw "Windows AP+STA traffic helper failed with exit code $LASTEXITCODE"
    }

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TrafficTimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $available = $serial.ReadExisting()
        }
        catch {
            Write-Output (
                'WINDOWS_APSTA_CDC_STOPPED_AFTER_TRAFFIC=' +
                $_.Exception.GetType().FullName + ': ' +
                ($_.Exception.Message -replace '[\r\n]+', ' ')
            )
            throw
        }
        if ($available.Length -ne 0) {
            [void]$received.Append($available)
            Print-Ra4Markers $available
            $capture = $received.ToString()
            if ($capture.Contains('RA4_APSTA:RESULT=FAIL:')) {
                throw 'RA4M1 AP+STA Sketch reported a traffic failure'
            }
            if ($capture.Contains('RA4_APSTA:RESULT=PASS')) {
                Write-Output 'WINDOWS_APSTA_TRAFFIC_PASS'
                exit 0
            }
        }
        Start-Sleep -Milliseconds 10
    }

    throw 'RA4M1 AP+STA Sketch did not report traffic completion'
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}
