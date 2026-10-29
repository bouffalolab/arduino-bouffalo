[CmdletBinding()]
param(
    [switch]$DownloadMode,
    [string]$HoldUntilReleasedPath = '',
    [string]$TriggerPath = '',
    [string]$ReadyPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The caller creates this sentinel before reset and removes it after the
# download or runtime check. Keep DTR/RTS ownership until that work completes.
if ($HoldUntilReleasedPath -and
    -not (Test-Path -LiteralPath $HoldUntilReleasedPath -PathType Leaf)) {
    throw "FTDI hold sentinel does not exist: $HoldUntilReleasedPath"
}

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.Status -eq 'OK' -and
    $_.InstanceId -match '^USB\\VID_0403&PID_6001\\'
})
if ($devices.Count -ne 1) {
    throw "expected exactly one final-carrier FTDI 0403:6001, found $($devices.Count)"
}
Write-Host "BL616CL_FTDI_IDENTITY=$($devices[0].InstanceId)"

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class Bl616clFtdiReset {
    [DllImport("ftd2xx.dll")]
    public static extern uint FT_Open(int index, out IntPtr handle);
    [DllImport("ftd2xx.dll")]
    public static extern uint FT_SetDtr(IntPtr handle);
    [DllImport("ftd2xx.dll")]
    public static extern uint FT_ClrDtr(IntPtr handle);
    [DllImport("ftd2xx.dll")]
    public static extern uint FT_SetRts(IntPtr handle);
    [DllImport("ftd2xx.dll")]
    public static extern uint FT_ClrRts(IntPtr handle);
    [DllImport("ftd2xx.dll")]
    public static extern uint FT_Close(IntPtr handle);
}
'@

$handle = [IntPtr]::Zero
$status = [Bl616clFtdiReset]::FT_Open(0, [ref]$handle)
if ($status -ne 0 -or $handle -eq [IntPtr]::Zero) {
    throw "FT_Open failed: $status"
}
try {
    if ($TriggerPath) {
        if ($DownloadMode -or -not $ReadyPath -or -not $HoldUntilReleasedPath) {
            throw 'Triggered reset requires runtime mode, ready path and hold sentinel'
        }
        if (Test-Path -LiteralPath $TriggerPath) {throw 'Reset trigger already exists'}
        if ([Bl616clFtdiReset]::FT_SetDtr($handle) -ne 0 -or
            [Bl616clFtdiReset]::FT_ClrRts($handle) -ne 0) {throw 'FTDI prearm failed'}
        [System.IO.File]::WriteAllText($ReadyPath, 'ready')
        Write-Host 'BL616CL_FTDI_TRIGGER_READY'
        while (-not (Test-Path -LiteralPath $TriggerPath)) {
            if (-not (Test-Path -LiteralPath $HoldUntilReleasedPath)) {
                Write-Host 'BL616CL_FTDI_TRIGGER_CANCELLED'
                return
            }
            Start-Sleep -Milliseconds 10
        }
    }
    # Match the proven Flash-boot control sequence in run_bl616cl_final_carrier_flash.py.
    if ($DownloadMode) {
        # DTR# high selects the native BootROM download path.
        $status = [Bl616clFtdiReset]::FT_ClrDtr($handle)
    } else {
        $status = [Bl616clFtdiReset]::FT_SetDtr($handle)
    }
    if ($status -ne 0) { throw "FT boot selection failed: $status" }
    $status = [Bl616clFtdiReset]::FT_SetRts($handle)
    if ($status -ne 0) { throw "FT_SetRts failed: $status" }
    Start-Sleep -Milliseconds 150
    $status = [Bl616clFtdiReset]::FT_ClrRts($handle)
    if ($status -ne 0) { throw "FT_ClrRts failed: $status" }
    if ($DownloadMode) {
        Write-Host 'BL616CL_FTDI_DOWNLOAD_RESET_PASS'
    } else {
        Write-Host 'BL616CL_FTDI_FLASH_BOOT_RESET_PASS'
    }
    if ($HoldUntilReleasedPath) {
        Write-Host 'BL616CL_FTDI_HOLD_ACTIVE'
        while (Test-Path -LiteralPath $HoldUntilReleasedPath -PathType Leaf) {
            Start-Sleep -Milliseconds 500
        }
        Write-Host 'BL616CL_FTDI_HOLD_RELEASED'
    }
}
finally {
    $status = [Bl616clFtdiReset]::FT_Close($handle)
    if ($status -ne 0) { Write-Warning "FT_Close failed: $status" }
}
