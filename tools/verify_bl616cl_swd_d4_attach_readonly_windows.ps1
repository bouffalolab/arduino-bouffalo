$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "CMSIS-DAP helper source does not exist: $source"
}

Add-Type -Path $source
$result = [CmsisDapWindowsProbe]::RunD4AttachReadOnlyNoFinalReset()
if ($result -ne 0) {
    throw "CMSIS-DAP D4 attach/read-only runner returned $result"
}

Write-Host 'BL616CL_RA4_SWD_D4_ATTACH_READONLY_NO_FINAL_RESET_PASS'
