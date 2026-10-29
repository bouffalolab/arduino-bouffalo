[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Boot2,

    [Parameter(Mandatory = $true)]
    [string]$Partition,

    [Parameter(Mandatory = $true)]
    [string]$Application,

    [string]$BootRomPort = 'auto',
    [string]$FlashTool = $env:BL616CL_FLASH_TOOL,
    [int]$BaudRate = 2000000,
    [int]$BootRomTimeoutSeconds = 15,
    [int]$BootRomSettleMilliseconds = 1000,
    [int]$FlashTimeoutSeconds = 45,
    [int]$ApplicationChunkBytes = 0,
    [ValidateRange(1, 3)]
    [int]$SegmentAttempts = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-BootRomPort {
    param(
        [string]$RequestedPort,
        [int]$TimeoutSeconds
    )

    if ($RequestedPort -ne 'auto') {
        $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
        do {
            $requestedMatch = Get-PnpDevice -PresentOnly |
                Where-Object {
                    $_.Status -eq 'OK' -and
                    $_.InstanceId -match 'VID_FFFF&PID_FFFF' -and
                    $_.FriendlyName -match ('\(' + [regex]::Escape($RequestedPort) + '\)')
                } |
                Select-Object -First 1
            if ($null -ne $requestedMatch -and
                [System.IO.Ports.SerialPort]::GetPortNames() -contains $RequestedPort) {
                return $RequestedPort
            }
            Start-Sleep -Milliseconds 200
        } while ((Get-Date) -lt $deadline)
        throw "BootROM port is not healthy after $TimeoutSeconds seconds: $RequestedPort"
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $matches = @(
            Get-PnpDevice -PresentOnly |
                Where-Object {
                    $_.Status -eq 'OK' -and
                    $_.InstanceId -match 'VID_FFFF&PID_FFFF'
                } |
                ForEach-Object {
                    if ($_.FriendlyName -match '\((COM\d+)\)') {
                        [pscustomobject]@{
                            Port = $Matches[1]
                            Device = $_
                        }
                    }
                }
        )
        if ($matches.Count -eq 1) {
            $port = $matches[0].Port
            if ([System.IO.Ports.SerialPort]::GetPortNames() -contains $port) {
                return $port
            }
        }
        if ($matches.Count -gt 1) {
            throw "expected exactly one ffff:ffff BL616CL BootROM port, found $($matches.Count)"
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)

    throw "ffff:ffff BL616CL BootROM port did not enumerate within $TimeoutSeconds seconds"
}

function Require-Image {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "required image does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Stage-Image {
    param(
        [string]$Source,
        [string]$DestinationDirectory
    )

    $destination = Join-Path $DestinationDirectory (Split-Path -Leaf $Source)
    Copy-Item -LiteralPath $Source -Destination $destination -Force

    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Source).Hash
    $destinationHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    if ($sourceHash -ne $destinationHash) {
        throw "staged image SHA-256 mismatch: $Source"
    }
    Write-Host "STAGED_IMAGE_SHA256=$destinationHash name=$(Split-Path -Leaf $Source)"
    return $destination
}

function Split-ApplicationImage {
    param(
        [string]$Source,
        [string]$DestinationDirectory,
        [int]$ChunkBytes
    )

    if ($ChunkBytes -eq 0) {
        return @(
            [pscustomobject]@{
                Name = 'application'
                Address = '0x10000'
                Path = $Source
            }
        )
    }

    $segments = @()
    $stream = [System.IO.File]::OpenRead($Source)
    try {
        [long]$offset = 0
        [int]$index = 0
        while ($offset -lt $stream.Length) {
            [long]$remaining = $stream.Length - $offset
            [int]$length = [Math]::Min($ChunkBytes, [int]$remaining)
            $buffer = New-Object byte[] $length
            [int]$read = 0
            while ($read -lt $length) {
                [int]$received = $stream.Read($buffer, $read, $length - $read)
                if ($received -le 0) {
                    throw "unexpected end of application image at offset $offset"
                }
                $read += $received
            }

            $name = 'application-{0:D3}.bin' -f $index
            $destination = Join-Path $DestinationDirectory $name
            [System.IO.File]::WriteAllBytes($destination, $buffer)
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
            $address = '0x{0:X}' -f (0x10000 + $offset)
            Write-Host "STAGED_APPLICATION_CHUNK_SHA256=$hash address=$address bytes=$length"
            $segments += [pscustomobject]@{
                Name = $name
                Address = $address
                Path = $destination
            }
            $offset += $length
            ++$index
        }
    }
    finally {
        $stream.Dispose()
    }
    return $segments
}

function Invoke-FlashCubeSegment {
    param(
        [string]$FlashToolPath,
        [string]$FlashToolDirectory,
        [string]$BootRomPort,
        [int]$BaudRate,
        [int]$TimeoutSeconds,
        [string]$StageRoot,
        [object[]]$Segments,
        [int]$Index
    )

    $stdout = Join-Path $StageRoot ('flashcube-{0:D3}.stdout.log' -f $Index)
    $stderr = Join-Path $StageRoot ('flashcube-{0:D3}.stderr.log' -f $Index)
    $segmentNames = ($Segments | ForEach-Object { $_.Name }) -join '+'
    $arguments = @(
        '--interface=uart',
        '--chipname=bl616cl',
        "--port=$BootRomPort",
        "--baudrate=$BaudRate",
        'write_flash_files'
    )
    foreach ($segment in $Segments) {
        $arguments += $segment.Address
        $arguments += $segment.Path
    }
    $flashProcess = Start-Process `
        -FilePath $FlashToolPath `
        -ArgumentList $arguments `
        -WorkingDirectory $FlashToolDirectory `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -PassThru `
        -NoNewWindow
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not $flashProcess.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $flashProcess.Refresh()
    }
    if (-not $flashProcess.HasExited) {
        & "$env:SystemRoot\System32\taskkill.exe" /PID $flashProcess.Id /T /F | Out-Null
        throw "FlashCube segment group $segmentNames exceeded $TimeoutSeconds seconds and was terminated"
    }

    $flashProcess.Refresh()
    $flashOutput = @(
        if (Test-Path -LiteralPath $stdout) {
            Get-Content -Raw -LiteralPath $stdout
        }
        if (Test-Path -LiteralPath $stderr) {
            Get-Content -Raw -LiteralPath $stderr
        }
    ) -join [Environment]::NewLine
    Write-Host $flashOutput
    # Some FlashCube child-process trees leave Start-Process with a null
    # ExitCode despite the launcher having completed and emitted all logs.
    # The device-side verification records below remain the success criterion.
    $exitCode = $flashProcess.ExitCode
    if ($null -ne $exitCode -and $exitCode -ne 0) {
        throw "FlashCube segment group $segmentNames failed with exit code $exitCode"
    }

    $verificationCount = [regex]::Matches(
        $flashOutput,
        '(?im)verification succeeded'
    ).Count
    if ($verificationCount -ne $Segments.Count) {
        throw "FlashCube segment group $segmentNames expected $($Segments.Count) device-side verification records (found $verificationCount)"
    }

    Write-Host "FINAL_CARRIER_FLASH_SEGMENT_PASS names=$segmentNames count=$($Segments.Count)"
    return $verificationCount
}

if (
    $BaudRate -le 0 -or
    $BootRomTimeoutSeconds -le 0 -or
    $BootRomSettleMilliseconds -lt 0 -or
    $FlashTimeoutSeconds -le 0 -or
    $ApplicationChunkBytes -lt 0
) {
    throw 'BaudRate, BootRomTimeoutSeconds, and FlashTimeoutSeconds must be positive; BootRomSettleMilliseconds and ApplicationChunkBytes cannot be negative'
}

$boot2Path = Require-Image -Path $Boot2
$partitionPath = Require-Image -Path $Partition
$applicationPath = Require-Image -Path $Application
if (-not (Test-Path -LiteralPath $FlashTool -PathType Leaf)) {
    throw "FlashCube executable does not exist: $FlashTool"
}

$bootRom = Get-BootRomPort -RequestedPort $BootRomPort -TimeoutSeconds $BootRomTimeoutSeconds
Write-Host "BL616CL_BOOTROM_PORT=$bootRom"
if ($BootRomSettleMilliseconds -gt 0) {
    Start-Sleep -Milliseconds $BootRomSettleMilliseconds
}
if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $bootRom) {
    throw "BL616CL BootROM port disappeared before FlashCube launch: $bootRom"
}
Write-Host "BL616CL_BOOTROM_PORT_STABLE=$bootRom settle_ms=$BootRomSettleMilliseconds"

$stageRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'uno-r4-bl616cl-flash-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
Write-Host "BL616CL_FLASH_STAGE=$stageRoot"

$stagedBoot2 = Stage-Image -Source $boot2Path -DestinationDirectory $stageRoot
$stagedPartition = Stage-Image -Source $partitionPath -DestinationDirectory $stageRoot
$stagedApplication = Stage-Image -Source $applicationPath -DestinationDirectory $stageRoot

$flashToolPath = (Resolve-Path -LiteralPath $FlashTool).Path
$flashToolDirectory = Split-Path -Parent $flashToolPath
$segments = @(
    [pscustomobject]@{
        Name = 'boot2'
        Address = '0x0'
        Path = $stagedBoot2
    },
    [pscustomobject]@{
        Name = 'partition'
        Address = '0xE000'
        Path = $stagedPartition
    }
)
$segments += Split-ApplicationImage `
    -Source $stagedApplication `
    -DestinationDirectory $stageRoot `
    -ChunkBytes $ApplicationChunkBytes

$verificationCount = 0
$groups = @()
if ($ApplicationChunkBytes -eq 0) {
    $groups += ,$segments
} else {
    $groups += ,@($segments[0], $segments[1], $segments[2])
    for ($index = 3; $index -lt $segments.Count; ++$index) {
        $groups += ,@($segments[$index])
    }
}

for ($index = 0; $index -lt $groups.Count; ++$index) {
    for ($attempt = 0; $attempt -lt $SegmentAttempts; ++$attempt) {
        # Recheck the chip identity before each write, including retries.
        $bootRom = Get-BootRomPort -RequestedPort $bootRom -TimeoutSeconds $BootRomTimeoutSeconds
        try {
            $verified = Invoke-FlashCubeSegment `
                -FlashToolPath $flashToolPath `
                -FlashToolDirectory $flashToolDirectory `
                -BootRomPort $bootRom `
                -BaudRate $BaudRate `
                -TimeoutSeconds $FlashTimeoutSeconds `
                -StageRoot $stageRoot `
                -Segments $groups[$index] `
                -Index ($index * $SegmentAttempts + $attempt)
            # Count only the successful attempt's device-side SHA checks.
            $verificationCount += $verified
            break
        } catch {
            if ($attempt + 1 -ge $SegmentAttempts) { throw }
            Write-Host "FINAL_CARRIER_FLASH_SEGMENT_RETRY group=$index attempt=$($attempt + 1) reason=$($_.Exception.Message)"
            Start-Sleep -Milliseconds 500
        }
    }
}

Write-Host "FINAL_CARRIER_FLASH_PASS bootrom=$bootRom verifications=$verificationCount chunks=$ApplicationChunkBytes"
