$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "CMSIS-DAP helper source does not exist: $source"
}

Add-Type -Path $source
$result = [CmsisDapWindowsProbe]::RunD4HaltRunFinalResetNoReattach()
if ($result -ne 0) {
    throw "CMSIS-DAP D4 no-reattach runner returned $result"
}

Write-Host 'BL616CL_RA4_SWD_D4_FINAL_RESET_NO_REATTACH_PASS'
