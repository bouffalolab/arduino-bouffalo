[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "CMSIS-DAP helper source does not exist: $source"
}

Add-Type -Path $source
[void][CmsisDapWindowsProbe]::RunRa4ApplicationReset()
Write-Host 'RA4_APPLICATION_RESET_HID_PASS'
