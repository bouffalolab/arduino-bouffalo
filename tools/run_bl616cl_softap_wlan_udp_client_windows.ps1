param(
    [Parameter(Mandatory = $true)]
    [string]$Interface,

    [Parameter(Mandatory = $true)]
    [string]$Ssid,

    [Parameter(Mandatory = $true)]
    [string]$Password,

    [Parameter(Mandatory = $true)]
    [string]$Address,

    [Parameter(Mandatory = $true)]
    [int]$Port,

    [int]$ConnectTimeoutMs = 30000,

    [int]$IoTimeoutMs = 10000,

    [ValidateRange(0, 1000)]
    [int]$FragmentRounds = 0,

    [string]$HoldUntilReleasedPath = '',

    [string]$ConnectWhenPath = ''
)

$ErrorActionPreference = 'Stop'

function Write-DiscoverySnapshot {
    param([string]$Label)
    try {
        [uint32]$count = 0
        [uint32]$matches = 0
        $result = [Bl616clWlanScan]::AvailableNetworks($script:scanInterface, $Ssid, [ref]$count, [ref]$matches)
        Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_NATIVE_VISIBILITY=$Label,$result,$count,$matches," + [DateTime]::UtcNow.ToString('o'))
    } catch {
        # This diagnostic must preserve the original connection outcome.
        Write-Output "WINDOWS_SOFTAP_UDP_CLIENT_NATIVE_VISIBILITY_UNAVAILABLE=$Label"
    }
}

function Get-ConnectedProfileName {
    param([string]$Alias)

    $profile = Get-NetConnectionProfile -InterfaceAlias $Alias -ErrorAction SilentlyContinue
    if ($null -eq $profile) {
        return $null
    }
    return $profile.Name
}

function Wait-ForProfile {
    param(
        [string]$Alias,
        [string]$ExpectedName,
        [string]$PeerAddress,
        [int]$TimeoutMs
    )

    $peerBytes = [System.Net.IPAddress]::Parse($PeerAddress).GetAddressBytes()
    if ($peerBytes.Length -ne 4) { throw 'SoftAP peer must use IPv4' }
    $prefix = ($peerBytes[0..2] -join '.') + '.'
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $nextDiagnostic = [DateTime]::UtcNow
    while ([DateTime]::UtcNow -lt $deadline) {
        if ([DateTime]::UtcNow -ge $nextDiagnostic) {
            $profileMatches = (Get-ConnectedProfileName $Alias) -eq $ExpectedName
            $nativeMatches = [Bl616clWlanScan]::ConnectedProfile($script:scanInterface) -eq $ExpectedName
            $addresses = @(Get-NetIPAddress -InterfaceAlias $Alias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                Where-Object { $_.PrefixOrigin -ne 'WellKnown' } |
                ForEach-Object { $_.IPAddress })
            Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_ASSOCIATION_SNAPSHOT=$([int]$profileMatches),$($addresses -join '|')," + [DateTime]::UtcNow.ToString('o'))
            Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_PROFILE_IDENTITY=$([int]$profileMatches),$([int]$nativeMatches)")
            $nextDiagnostic = [DateTime]::UtcNow.AddSeconds(1)
        }
        if ((Get-ConnectedProfileName $Alias) -eq $ExpectedName) {
            $ipv4 = @(
                Get-NetIPAddress -InterfaceAlias $Alias -AddressFamily IPv4 `
                    -ErrorAction SilentlyContinue |
                    Where-Object {
                        $_.IPAddress.StartsWith($prefix) -and
                        $_.IPAddress -ne $PeerAddress -and
                        $_.PrefixOrigin -ne 'WellKnown'
                    }
            )
            if ($ipv4.Count -gt 0) {
                return
            }
        }
        Start-Sleep -Milliseconds 250
    }
    if ((Get-ConnectedProfileName $Alias) -eq $ExpectedName) {
        $script:stage = 'WLAN_IPV4_READY'
        throw 'Windows test profile connected but has no SoftAP IPv4 address'
    }
    throw 'Windows WLAN did not join the temporary BL616CL SoftAP'
}

$originalProfile = Get-ConnectedProfileName $Interface
$profilePath = [System.IO.Path]::GetTempFileName()
$profileInstalled = $false
$udp = $null
$stage = 'PROFILE_CREATE'
$wlanStarted = [DateTime]::UtcNow

Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_HELPER_STARTED'

try {
    if (-not ('Bl616clWlanScan' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'bl616cl_windows_wlan_scan.cs')
    }
    $script:scanInterface = [Guid]((Get-NetAdapter -Name $Interface -ErrorAction Stop).InterfaceGuid)
    $xml = @(
        '<?xml version="1.0"?>'
        '<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">'
        "  <name>$Ssid</name>"
        '  <SSIDConfig>'
        '    <SSID>'
        "      <name>$Ssid</name>"
        '    </SSID>'
        '  </SSIDConfig>'
        '  <connectionType>ESS</connectionType>'
        '  <connectionMode>auto</connectionMode>'
        '  <autoSwitch>false</autoSwitch>'
        '  <MSM>'
        '    <security>'
        '      <authEncryption>'
        '        <authentication>WPA2PSK</authentication>'
        '        <encryption>AES</encryption>'
        '        <useOneX>false</useOneX>'
        '      </authEncryption>'
        '      <sharedKey>'
        '        <keyType>passPhrase</keyType>'
        '        <protected>false</protected>'
        "        <keyMaterial>$Password</keyMaterial>"
        '      </sharedKey>'
        '    </security>'
        '  </MSM>'
        '</WLANProfile>'
    ) -join [Environment]::NewLine
    Set-Content -LiteralPath $profilePath -Value $xml -Encoding UTF8
    & netsh.exe wlan add profile "filename=$profilePath" "interface=$Interface" user=current |
        Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'netsh wlan add profile failed'
    }
    $profileInstalled = $true

    if ($ConnectWhenPath) {
        if (-not ('Bl616clWlanScan' -as [type])) {
            Add-Type -Path (Join-Path $PSScriptRoot 'bl616cl_windows_wlan_scan.cs')
        }
        $scanInterface = [Guid]((Get-NetAdapter -Name $Interface -ErrorAction Stop).InterfaceGuid)
        $stage = 'WLAN_WAIT_AP_READY'
        Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_PROFILE_READY'
        $readyDeadline = [DateTime]::UtcNow.AddMilliseconds($ConnectTimeoutMs)
        while (-not (Test-Path -LiteralPath $ConnectWhenPath -PathType Leaf)) {
            if (($HoldUntilReleasedPath -and
                 -not (Test-Path -LiteralPath $HoldUntilReleasedPath -PathType Leaf)) -or
                [DateTime]::UtcNow -ge $readyDeadline) {
                throw 'AP readiness wait ended before connect was released'
            }
            Start-Sleep -Milliseconds 50
        }
    }

    $connectDeadline = [DateTime]::UtcNow.AddMilliseconds($ConnectTimeoutMs)
    if ($ConnectWhenPath) {
        $stage = 'WLAN_DISCOVERY'
        [uint32]$scanNotification = 0
        Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_SCAN_KIND=DIRECTED_BUSY_RETRY'
        for ($scanAttempt = 1; $scanAttempt -le 8; $scanAttempt++) {
            $scanResult = [Bl616clWlanScan]::ScanTarget($scanInterface, $Ssid, 5000, [ref]$scanNotification)
            Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_SCAN_ATTEMPT=$scanAttempt,$scanResult")
            if ($scanResult -ne 170 -or [DateTime]::UtcNow -ge $connectDeadline) { break }
            Start-Sleep -Milliseconds 1000
        }
        Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_SCAN_RESULT=$scanResult,$scanNotification," + [DateTime]::UtcNow.ToString('o'))
        Write-DiscoverySnapshot 'AFTER_SCAN'
        if ($scanResult -ne 0 -and $scanResult -ne 170) { throw 'Windows WLAN scan request did not complete' }
        # ERROR_BUSY may belong to Windows' own scan; still require target visibility below.
        $visible = $false
        while ([DateTime]::UtcNow -lt $connectDeadline) {
            $networks = & netsh.exe wlan show networks "interface=$Interface" mode=bssid
            if ($LASTEXITCODE -eq 0 -and $networks -match [regex]::Escape($Ssid)) {
                $visible = $true
                Write-Output ('WINDOWS_SOFTAP_UDP_CLIENT_NETWORK_VISIBLE_UTC=' + [DateTime]::UtcNow.ToString('o'))
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if (-not $visible) {
            Write-DiscoverySnapshot 'DISCOVERY_FAILURE'
            throw 'Windows WLAN could not see the temporary BL616CL SoftAP'
        }
    }
    $stage = 'WLAN_CONNECT_REQUEST'
    $connectRequested = $false
    $connectFailureReported = $false
    while ([DateTime]::UtcNow -lt $connectDeadline) {
        $connectOutput = & netsh.exe wlan connect "name=$Ssid" "ssid=$Ssid" "interface=$Interface" 2>&1
        $connectExit = $LASTEXITCODE
        Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_CONNECT_RESULT=" + $connectExit + "," + [DateTime]::UtcNow.ToString('o'))
        if ($connectExit -ne 0 -and -not $connectFailureReported) {
            $connectText = ($connectOutput -join ' ').Replace($Ssid, '<test-profile>').Replace($Password, '<test-key>')
            $connectText = $connectText -replace 'uno-r4-[0-9a-f]{8}', '<test-profile>'
            foreach ($name in @($originalProfile)) {
                if ($name) { $connectText = $connectText.Replace($name, '<original-profile>') }
            }
            Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_CONNECT_DETAIL=" + $connectText)
            $connectFailureReported = $true
        }
        if ($connectExit -eq 0) {
            $connectRequested = $true
            break
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $connectRequested) {
        throw 'netsh wlan connect failed'
    }
    $stage = 'WLAN_ASSOCIATION'
    Wait-ForProfile $Interface $Ssid $Address $ConnectTimeoutMs
    Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_ASSOCIATED'

    if ($HoldUntilReleasedPath) {
        $stage = 'WLAN_HOLD'
        $holdStarted = [DateTime]::UtcNow
        Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_HOLD_STARTED_UTC=" + $holdStarted.ToString("o"))
        $nextSnapshot = $holdStarted
        while (Test-Path -LiteralPath $HoldUntilReleasedPath -PathType Leaf) {
            if ([DateTime]::UtcNow -ge $nextSnapshot) {
                # Passive snapshots only: never log generated SSID/password or
                # reconnect during a pressure gate and hide a link failure.
                $matches = (Get-ConnectedProfileName $Interface) -eq $Ssid
                $addresses = @(Get-NetIPAddress -InterfaceAlias $Interface -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                    Where-Object { $_.PrefixOrigin -ne 'WellKnown' } |
                    ForEach-Object { $_.IPAddress })
                $elapsed = [int]([DateTime]::UtcNow - $holdStarted).TotalMilliseconds
                Write-Output "WINDOWS_SOFTAP_UDP_CLIENT_LINK_SNAPSHOT=$elapsed,$([int]$matches),$($addresses -join '|'),$([DateTime]::UtcNow.ToString('o'))"
                $nextSnapshot = [DateTime]::UtcNow.AddSeconds(1)
            }
            Start-Sleep -Milliseconds 250
        }
        Write-Output ('WINDOWS_SOFTAP_UDP_CLIENT_HOLD_RELEASED_UTC=' + [DateTime]::UtcNow.ToString('o'))
        Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_HOLD_RELEASED'
        return
    }

    $stage = 'UDP_CREATE'
    $udp = [System.Net.Sockets.UdpClient]::new()
    $udp.Client.ReceiveTimeout = $IoTimeoutMs
    $remote = [System.Net.IPEndPoint]::new(
        [System.Net.IPAddress]::Parse($Address),
        $Port
    )
    if ($FragmentRounds -gt 0) {
        for ($sequence = 1; $sequence -le $FragmentRounds; ++$sequence) {
            $stage = 'FRAGMENT_REQUEST'
            [byte[]]$control = @(0x51) + [BitConverter]::GetBytes([uint32]$sequence)
            if ($udp.Send($control, $control.Length, $remote) -ne 5) {
                throw 'Fragment request length mismatch'
            }
            $stage = 'FRAGMENT_RECEIVE'
            $source = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            $reply = $udp.Receive([ref]$source)
            if (-not $source.Equals($remote) -or $reply.Length -ne 16384 -or
                [BitConverter]::ToUInt32($reply, 0) -ne $sequence) {
                throw 'Fragment datagram peer, length or sequence mismatch'
            }
            for ($index = 4; $index -lt $reply.Length; ++$index) {
                if ($reply[$index] -ne 0) { throw 'Fragment datagram content mismatch' }
            }
            $control[0] = 0x41
            if ($udp.Send($control, $control.Length, $remote) -ne 5) {
                throw 'Fragment ACK length mismatch'
            }
            Write-Output "WINDOWS_SOFTAP_UDP_CLIENT_FRAGMENT_ROUND_PASS=$sequence"
        }
        Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_SEND_PASS'
        Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_REPLY_PASS'
        Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_DATAGRAM_PASS'
    } else {
    $request = [System.Text.Encoding]::ASCII.GetBytes('bl616cl-udp-echo')
    $expected = [System.Text.Encoding]::ASCII.GetBytes('bl616cl-udp-reply')

    $stage = 'UDP_SEND'
    $sent = $udp.Send($request, $request.Length, $remote)
    if ($sent -ne $request.Length) {
        throw 'UDP send length mismatch'
    }
    Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_SEND_PASS'

    $stage = 'UDP_RECEIVE'
    $source = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
    $reply = $udp.Receive([ref]$source)
    if (
        -not $source.Address.Equals([System.Net.IPAddress]::Parse($Address)) -or
        $source.Port -ne $Port
    ) {
        throw 'UDP reply peer mismatch'
    }
    if ($reply.Length -ne $expected.Length) {
        throw 'UDP reply length mismatch'
    }
    for ($index = 0; $index -lt $expected.Length; ++$index) {
        if ($reply[$index] -ne $expected[$index]) {
            throw 'UDP reply content mismatch'
        }
    }
    Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_REPLY_PASS'
    Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_DATAGRAM_PASS'
    }
}
catch {
    Write-Output "WINDOWS_SOFTAP_UDP_CLIENT_FAILURE_STAGE=$stage"
    $message = $_.Exception.Message -replace '[\r\n]+', ' '
    if (-not [string]::IsNullOrWhiteSpace($message)) {
        Write-Output "WINDOWS_SOFTAP_UDP_CLIENT_ERROR_MESSAGE=$message"
    }
    throw
}
finally {
    if ($null -ne $udp) {
        $udp.Dispose()
    }
    Remove-Item -LiteralPath $profilePath -Force -ErrorAction SilentlyContinue
    if ($profileInstalled) {
        $stage = 'PROFILE_REMOVE'
        & netsh.exe wlan delete profile "name=$Ssid" "interface=$Interface" |
            Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_PROFILE_REMOVED'
        }
    }
    if ($HoldUntilReleasedPath) {
        try {
            $events = @(Get-WinEvent -FilterHashtable @{
                LogName='Microsoft-Windows-WLAN-AutoConfig/Operational'
                Id=8000,8001,8002,8003
                StartTime=$wlanStarted.ToLocalTime()
            } -MaxEvents 200 -ErrorAction Stop)
            foreach ($event in ($events | Sort-Object TimeCreated)) {
                $eventXml = [xml]$event.ToXml()
                $fields = @{}
                foreach ($entry in $eventXml.Event.EventData.Data) {
                    $fields[$entry.Name] = [string]$entry.'#text'
                }
                # Never print profile names, SSIDs, BSSIDs, or event messages.
                $isTest = [int]($fields['ProfileName'] -eq $Ssid)
                $rawMode = [string]$fields['ConnectionMode']
                $mode = 'UNKNOWN'
                if ($rawMode -match '\u624b\u52a8|manual') { $mode = 'MANUAL' }
                elseif ($rawMode -match '\u81ea\u52a8|auto') { $mode = 'AUTO' }
                $reason = [string]$fields['ReasonCode'] -replace '[,\r\n]', ' '
                $connection = [string]$fields['ConnectionId'] -replace '[,\r\n]', ' '
                Write-Output ("WINDOWS_SOFTAP_UDP_CLIENT_WLAN_EVENT=" +
                    $event.TimeCreated.ToUniversalTime().ToString('o') + ',' +
                    $event.Id + ',' + $isTest + ',' + $connection + ',' + $reason + ',' + $mode)
            }
        } catch {
            Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_WLAN_EVENTS_UNAVAILABLE'
        }
    }
    if ($null -ne $originalProfile -and $originalProfile -ne $Ssid) {
        Write-Output ('WINDOWS_SOFTAP_UDP_CLIENT_RESTORE_REQUEST_UTC=' + [DateTime]::UtcNow.ToString('o'))
        $stage = 'WLAN_RESTORE_REQUEST'
        & netsh.exe wlan connect "name=$originalProfile" "interface=$Interface" |
            Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_RESTORE_REQUESTED'
            $restoreDeadline = [DateTime]::UtcNow.AddSeconds(10)
            $restoreConfirmed = $false
            do {
                if ((Get-ConnectedProfileName $Interface) -eq $originalProfile) {
                    $restoreConfirmed = $true
                    break
                }
                Start-Sleep -Milliseconds 250
            } while ([DateTime]::UtcNow -lt $restoreDeadline)
            if (-not $restoreConfirmed) {
                throw 'original WLAN profile restoration was not confirmed'
            }
            Write-Output 'WINDOWS_SOFTAP_UDP_CLIENT_RESTORE_CONFIRMED'
        }
    }
}
