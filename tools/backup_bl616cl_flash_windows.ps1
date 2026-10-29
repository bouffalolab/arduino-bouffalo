[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$FlashTool='D:\developer\tools\flash_cube\v1.4.0\BLFlashCommand.exe'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# The caller has already identified the dedicated board and holds it in ISP.
$ports=@(Get-PnpDevice -PresentOnly | Where-Object {
    $_.Status -eq 'OK' -and $_.InstanceId -match 'VID_FFFF&PID_FFFF'
} | ForEach-Object { if ($_.FriendlyName -match '\((COM\d+)\)') {$Matches[1]} })
if ($ports.Count -ne 1) {throw 'Expected exactly one BL616CL BootROM port'}
if (Test-Path -LiteralPath $Output) {throw 'Backup destination already exists'}
$toolDirectory=Split-Path -Parent $FlashTool
Push-Location $toolDirectory
try {
    & $FlashTool '--interface=uart' '--chipname=bl616cl' "--port=$($ports[0])" `
        '--baudrate=2000000' '--flash' '--read' '--start=0x0' '--len=0x400000' "--file=$Output"
    if ($LASTEXITCODE -ne 0) {throw 'Flash read failed'}
    if ((Get-Item -LiteralPath $Output).Length -ne 0x400000) {throw 'Flash read size differs from 4 MiB'}
    $hash=(Get-FileHash -Algorithm SHA256 -LiteralPath $Output).Hash
    Write-Host "BL616CL_PRIVATE_FLASH_BACKUP_PASS bytes=4194304 sha256=$hash"
} finally {Pop-Location}
