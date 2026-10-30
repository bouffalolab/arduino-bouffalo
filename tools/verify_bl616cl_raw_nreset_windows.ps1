$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "CMSIS-DAP helper source does not exist: $source"
}

Add-Type -Path $source
$result = [CmsisDapWindowsProbe]::RunRawNresetPulse()
if ($result -ne 0) {
    throw "BL616CL raw nRESET runner returned $result"
}

Write-Host 'BL616CL_RA4_RAW_NRESET_PULSE_PASS'
