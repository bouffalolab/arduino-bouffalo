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

    [int]$IoTimeoutMs = 10000
)

$ErrorActionPreference = "Stop"

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
    throw "Windows WLAN did not join the temporary BL616CL SoftAP"
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
            throw "TCP read timed out"
        }
        if (-not $Stream.DataAvailable) {
            Start-Sleep -Milliseconds 5
            continue
        }
        $count = $Stream.Read($actual, $offset, $actual.Length - $offset)
        if ($count -le 0) {
            throw "TCP peer closed before the expected response"
        }
        $offset += $count
    }
    for ($index = 0; $index -lt $Expected.Length; ++$index) {
        if ($actual[$index] -ne $Expected[$index]) {
            throw "TCP response content mismatch"
        }
    }
}

function Connect-Tcp {
    param(
        [string]$Address,
        [int]$DestinationPort,
        [int]$TimeoutMs
    )

    $script:tcpConnectFailure = $null
    $client = [System.Net.Sockets.TcpClient]::new()
    $connect = $client.ConnectAsync($Address, $DestinationPort)
    try {
        if (-not $connect.Wait($TimeoutMs)) {
            $script:tcpConnectFailure = 'TIMEOUT'
            throw "TCP connect timed out"
        }
        [void]$connect.GetAwaiter().GetResult()
    }
    catch {
        $exception = $_.Exception
        while ($null -ne $exception.InnerException) {
            $exception = $exception.InnerException
        }
        if ($exception -is [System.Net.Sockets.SocketException]) {
            $script:tcpConnectFailure = $exception.SocketErrorCode.ToString()
        }
        elseif ($null -eq $script:tcpConnectFailure) {
            $script:tcpConnectFailure = $exception.GetType().Name
        }
        $client.Dispose()
        throw "TCP connect failed"
    }
    $client.ReceiveTimeout = $TimeoutMs
    $client.SendTimeout = $TimeoutMs
    return $client
}

$originalProfile = Get-ConnectedProfileName $Interface
$profilePath = [System.IO.Path]::GetTempFileName()
$profileInstalled = $false
$broadcastClient = $null
$acceptedClient = $null
$script:tcpStreamFailure = $null
$stage = 'PROFILE_CREATE'

Write-Output "WINDOWS_SOFTAP_CLIENT_HELPER_STARTED"

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
        throw "netsh wlan add profile failed"
    }
    $profileInstalled = $true

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
        throw "netsh wlan connect failed"
    }
    $stage = 'WLAN_ASSOCIATION'
    Wait-ForProfile $Interface $Ssid $ConnectTimeoutMs
    Write-Output "WINDOWS_SOFTAP_CLIENT_ASSOCIATED"

    $stage = 'TCP_BROADCAST_CONNECT'
    $broadcastClient = Connect-Tcp $Address $Port $IoTimeoutMs
    $stage = 'TCP_BROADCAST_STREAM'
    try {
        $broadcastStream = $broadcastClient.GetStream()
    }
    catch {
        $script:tcpStreamFailure = $_.Exception.GetType().Name
        throw
    }
    $broadcastProbe = [System.Text.Encoding]::ASCII.GetBytes("broadcast-probe")
    $broadcastReply = [System.Text.Encoding]::ASCII.GetBytes("broadcast-reply")
    $stage = 'TCP_BROADCAST_WRITE'
    $broadcastStream.Write($broadcastProbe, 0, $broadcastProbe.Length)
    $stage = 'TCP_BROADCAST_FLUSH'
    $broadcastStream.Flush()
    $stage = 'TCP_BROADCAST_IO'
    Read-Exact $broadcastStream $broadcastReply $IoTimeoutMs
    Write-Output "WINDOWS_SOFTAP_CLIENT_BROADCAST_PASS"

    $stage = 'TCP_ACCEPT_CONNECT'
    $acceptedClient = Connect-Tcp $Address $Port $IoTimeoutMs
    $stage = 'TCP_ACCEPT_STREAM'
    try {
        $acceptedStream = $acceptedClient.GetStream()
    }
    catch {
        $script:tcpStreamFailure = $_.Exception.GetType().Name
        throw
    }
    $acceptedGreeting = [System.Text.Encoding]::ASCII.GetBytes("accepted-greeting")
    $acceptedAck = [System.Text.Encoding]::ASCII.GetBytes("accepted-ack")
    $acceptedReply = [System.Text.Encoding]::ASCII.GetBytes("accepted-reply")
    $stage = 'TCP_ACCEPT_GREETING'
    Read-Exact $acceptedStream $acceptedGreeting $IoTimeoutMs
    $stage = 'TCP_ACCEPT_WRITE'
    $acceptedStream.Write($acceptedAck, 0, $acceptedAck.Length)
    $stage = 'TCP_ACCEPT_FLUSH'
    $acceptedStream.Flush()
    $stage = 'TCP_ACCEPT_IO'
    Read-Exact $acceptedStream $acceptedReply $IoTimeoutMs
    Write-Output "WINDOWS_SOFTAP_CLIENT_ACCEPT_PASS"
    Write-Output "WINDOWS_SOFTAP_CLIENT_TCP_CONCURRENT_SESSIONS_PASS"
}
catch {
    Write-Output "WINDOWS_SOFTAP_CLIENT_FAILURE_STAGE=$stage"
    if ($null -ne $script:tcpConnectFailure) {
        Write-Output "WINDOWS_SOFTAP_CLIENT_TCP_CONNECT_ERROR=$script:tcpConnectFailure"
    }
    if ($null -ne $script:tcpStreamFailure) {
        Write-Output "WINDOWS_SOFTAP_CLIENT_TCP_STREAM_ERROR=$script:tcpStreamFailure"
    }
    $message = $_.Exception.Message -replace '[\r\n]+', ' '
    if (-not [string]::IsNullOrWhiteSpace($message)) {
        Write-Output "WINDOWS_SOFTAP_CLIENT_ERROR_MESSAGE=$message"
    }
    throw
}
finally {
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
            Write-Output "WINDOWS_SOFTAP_CLIENT_PROFILE_REMOVED"
        }
    }
    if ($null -ne $originalProfile -and $originalProfile -ne $Ssid) {
        $stage = 'WLAN_RESTORE_REQUEST'
        & netsh.exe wlan connect "name=$originalProfile" "interface=$Interface" |
            Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Output "WINDOWS_SOFTAP_CLIENT_RESTORE_REQUESTED"
        }
    }
}
