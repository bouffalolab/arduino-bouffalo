param(
    [Parameter(Mandatory = $true)]
    [string]$Interface,

    [Parameter(Mandatory = $true)]
    [string]$Ssid,

    [Parameter(Mandatory = $true)]
    [string]$Password,

    [int]$ConnectTimeoutMs = 30000,

    [int]$IoTimeoutMs = 10000
)

$ErrorActionPreference = 'Stop'

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
        [int]$TimeoutMs
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Get-ConnectedProfileName $Alias) -eq $ExpectedName) {
            $ipv4 = @(
                Get-NetIPAddress -InterfaceAlias $Alias -AddressFamily IPv4 `
                    -ErrorAction SilentlyContinue |
                    Where-Object {
                        $_.IPAddress -match '^192\.168\.42\.' -and
                        $_.PrefixOrigin -ne 'WellKnown'
                    }
            )
            if ($ipv4.Count -gt 0) {
                return
            }
        }
        Start-Sleep -Milliseconds 250
    }
    throw 'Windows WLAN did not join the temporary BL616CL SoftAP'
}

function Wait-ForDeparture {
    param(
        [string]$Alias,
        [string]$DepartedName,
        [int]$TimeoutMs
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Get-ConnectedProfileName $Alias) -ne $DepartedName) {
            return
        }
        Start-Sleep -Milliseconds 250
    }
    throw 'Windows WLAN did not leave the BL616CL SoftAP'
}

function Wait-ForNetworkVisibility {
    param(
        [string]$Alias,
        [string]$ExpectedName,
        [int]$TimeoutMs
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $networks = & netsh.exe wlan show networks "interface=$Alias" mode=bssid
        if (
            $LASTEXITCODE -eq 0 -and
            $networks -match [regex]::Escape($ExpectedName)
        ) {
            return
        }
        Start-Sleep -Milliseconds 500
    }
    throw 'Windows WLAN could not see the temporary BL616CL SoftAP'
}

function Read-Exact {
    param(
        [System.IO.Stream]$Stream,
        [byte[]]$Expected,
        [int]$TimeoutMs
    )

    $actual = New-Object byte[] $Expected.Length
    $offset = 0
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ($offset -lt $actual.Length) {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw 'TCP read timed out'
        }
        if (-not $Stream.DataAvailable) {
            Start-Sleep -Milliseconds 5
            continue
        }
        $count = $Stream.Read($actual, $offset, $actual.Length - $offset)
        if ($count -le 0) {
            throw 'TCP peer closed before the expected response'
        }
        $offset += $count
    }
    for ($index = 0; $index -lt $Expected.Length; ++$index) {
        if ($actual[$index] -ne $Expected[$index]) {
            throw 'TCP response content mismatch'
        }
    }
}

function Connect-Tcp {
    param(
        [string]$Address,
        [int]$DestinationPort,
        [int]$TimeoutMs
    )

    $client = [System.Net.Sockets.TcpClient]::new()
    $connect = $client.ConnectAsync($Address, $DestinationPort)
    try {
        if (-not $connect.Wait($TimeoutMs)) {
            throw 'TCP connect timed out'
        }
        [void]$connect.GetAwaiter().GetResult()
    }
    catch {
        $client.Dispose()
        throw
    }
    $client.ReceiveTimeout = $TimeoutMs
    $client.SendTimeout = $TimeoutMs
    return $client
}

function Get-ExceptionDetails {
    param([System.Exception]$Exception)

    $pending = [System.Collections.Generic.Queue[System.Exception]]::new()
    $pending.Enqueue($Exception)
    $details = [System.Collections.Generic.List[string]]::new()

    while ($pending.Count -gt 0) {
        $current = $pending.Dequeue()
        $entry = $current.GetType().FullName + ': ' +
            ($current.Message -replace '[\r\n]+', ' ')
        if ($current -is [System.Net.Sockets.SocketException]) {
            $entry += ' [SocketErrorCode=' + $current.SocketErrorCode + ']'
        }
        [void]$details.Add($entry)

        if ($current -is [System.AggregateException]) {
            foreach ($inner in $current.Flatten().InnerExceptions) {
                $pending.Enqueue($inner)
            }
        }
        elseif ($null -ne $current.InnerException) {
            $pending.Enqueue($current.InnerException)
        }
    }

    return $details -join ' | '
}

$originalProfile = Get-ConnectedProfileName $Interface
$profilePath = [System.IO.Path]::GetTempFileName()
$profileInstalled = $false
$broadcastClient = $null
$acceptedClient = $null
$udp = $null
$stage = 'PROFILE_CREATE'

Write-Output 'WINDOWS_APSTA_CLIENT_HELPER_STARTED'

try {
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

    $stage = 'WLAN_NETWORK_VISIBILITY'
    Wait-ForNetworkVisibility $Interface $Ssid $ConnectTimeoutMs
    Write-Output 'WINDOWS_APSTA_CLIENT_SOFTAP_VISIBLE'

    $stage = 'WLAN_CONNECT_REQUEST'
    $connectRequested = $false
    $connectDeadline = [DateTime]::UtcNow.AddMilliseconds($ConnectTimeoutMs)
    while ([DateTime]::UtcNow -lt $connectDeadline) {
        & netsh.exe wlan connect "name=$Ssid" "ssid=$Ssid" "interface=$Interface" |
            Out-Null
        if ($LASTEXITCODE -eq 0) {
            $connectRequested = $true
            break
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $connectRequested) {
        throw 'netsh wlan connect failed'
    }
    $stage = 'WLAN_ASSOCIATION'
    Wait-ForProfile $Interface $Ssid $ConnectTimeoutMs
    Write-Output 'WINDOWS_APSTA_CLIENT_ASSOCIATED'

    $stage = 'TCP_BROADCAST_CONNECT'
    $broadcastClient = Connect-Tcp '192.168.42.1' 4243 $IoTimeoutMs
    $broadcastStream = $broadcastClient.GetStream()
    $broadcastProbe = [System.Text.Encoding]::ASCII.GetBytes('broadcast-probe')
    $broadcastReply = [System.Text.Encoding]::ASCII.GetBytes('broadcast-reply')
    $broadcastStream.Write($broadcastProbe, 0, $broadcastProbe.Length)
    $broadcastStream.Flush()
    Read-Exact $broadcastStream $broadcastReply $IoTimeoutMs
    Write-Output 'WINDOWS_APSTA_CLIENT_TCP_BROADCAST_PASS'

    $stage = 'TCP_ACCEPT_CONNECT'
    $acceptedClient = Connect-Tcp '192.168.42.1' 4243 $IoTimeoutMs
    $acceptedStream = $acceptedClient.GetStream()
    $acceptedGreeting = [System.Text.Encoding]::ASCII.GetBytes('accepted-greeting')
    $acceptedAck = [System.Text.Encoding]::ASCII.GetBytes('accepted-ack')
    $acceptedReply = [System.Text.Encoding]::ASCII.GetBytes('accepted-reply')
    Read-Exact $acceptedStream $acceptedGreeting $IoTimeoutMs
    Write-Output 'WINDOWS_APSTA_CLIENT_TCP_CONCURRENT_SESSIONS_PASS'

    $stage = 'UDP_CREATE'
    $udp = [System.Net.Sockets.UdpClient]::new()
    $udp.Client.ReceiveTimeout = $IoTimeoutMs
    $remote = [System.Net.IPEndPoint]::new(
        [System.Net.IPAddress]::Parse('192.168.42.1'),
        4244
    )
    $request = [System.Text.Encoding]::ASCII.GetBytes('bl616cl-udp-echo')
    $expected = [System.Text.Encoding]::ASCII.GetBytes('bl616cl-udp-reply')
    $stage = 'UDP_SEND'
    $sent = $udp.Send($request, $request.Length, $remote)
    if ($sent -ne $request.Length) {
        throw 'UDP send length mismatch'
    }
    $stage = 'UDP_RECEIVE'
    $source = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
    $reply = $udp.Receive([ref]$source)
    if (
        -not $source.Address.Equals([System.Net.IPAddress]::Parse('192.168.42.1')) -or
        $source.Port -ne 4244 -or
        $reply.Length -ne $expected.Length
    ) {
        throw 'UDP reply peer or length mismatch'
    }
    for ($index = 0; $index -lt $expected.Length; ++$index) {
        if ($reply[$index] -ne $expected[$index]) {
            throw 'UDP reply content mismatch'
        }
    }
    Write-Output 'WINDOWS_APSTA_CLIENT_UDP_PASS'

    $stage = 'TCP_ACCEPT_ACK'
    $acceptedStream.Write($acceptedAck, 0, $acceptedAck.Length)
    $acceptedStream.Flush()
    Read-Exact $acceptedStream $acceptedReply $IoTimeoutMs
    Write-Output 'WINDOWS_APSTA_CLIENT_TCP_ACCEPT_PASS'
    Write-Output 'WINDOWS_APSTA_CLIENT_TRAFFIC_PASS'

    $broadcastClient.Dispose()
    $broadcastClient = $null
    $acceptedClient.Dispose()
    $acceptedClient = $null
    $udp.Dispose()
    $udp = $null

    $stage = 'WLAN_DISCONNECT'
    & netsh.exe wlan disconnect "interface=$Interface" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'netsh wlan disconnect failed'
    }
    Wait-ForDeparture $Interface $Ssid $ConnectTimeoutMs
    Write-Output 'WINDOWS_APSTA_CLIENT_DEPARTED'

    $stage = 'WLAN_REASSOCIATION'
    & netsh.exe wlan connect "name=$Ssid" "ssid=$Ssid" "interface=$Interface" |
        Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'netsh wlan reconnect failed'
    }
    Wait-ForProfile $Interface $Ssid $ConnectTimeoutMs
    Write-Output 'WINDOWS_APSTA_CLIENT_REASSOCIATED'

    $stage = 'TCP_AFTER_REASSOCIATION'
    $broadcastClient = Connect-Tcp '192.168.42.1' 4243 $IoTimeoutMs
    $broadcastStream = $broadcastClient.GetStream()
    $recoveryProbe = [System.Text.Encoding]::ASCII.GetBytes('recovery-probe')
    $recoveryReply = [System.Text.Encoding]::ASCII.GetBytes('recovery-reply')
    $broadcastStream.Write($recoveryProbe, 0, $recoveryProbe.Length)
    $broadcastStream.Flush()
    Read-Exact $broadcastStream $recoveryReply $IoTimeoutMs
    Write-Output 'WINDOWS_APSTA_CLIENT_REASSOCIATION_TRAFFIC_PASS'
}
catch {
    Write-Output "WINDOWS_APSTA_CLIENT_FAILURE_STAGE=$stage"
    $details = Get-ExceptionDetails $_.Exception
    if (-not [string]::IsNullOrWhiteSpace($details)) {
        Write-Output "WINDOWS_APSTA_CLIENT_ERROR_DETAILS=$details"
    }
    throw
}
finally {
    if ($null -ne $udp) {
        $udp.Dispose()
    }
    if ($null -ne $acceptedClient) {
        $acceptedClient.Dispose()
    }
    if ($null -ne $broadcastClient) {
        $broadcastClient.Dispose()
    }
    Remove-Item -LiteralPath $profilePath -Force -ErrorAction SilentlyContinue
    if ($profileInstalled) {
        $stage = 'PROFILE_REMOVE'
        & netsh.exe wlan delete profile "name=$Ssid" "interface=$Interface" |
            Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Output 'WINDOWS_APSTA_CLIENT_PROFILE_REMOVED'
        }
    }
    if ($null -ne $originalProfile -and $originalProfile -ne $Ssid) {
        $stage = 'WLAN_RESTORE_REQUEST'
        & netsh.exe wlan connect "name=$originalProfile" "interface=$Interface" |
            Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Output 'WINDOWS_APSTA_CLIENT_RESTORE_REQUESTED'
        }
    }
}
