$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
Add-Type -Path $source
[void][CmsisDapWindowsProbe]::RunCdcInTelemetry()
