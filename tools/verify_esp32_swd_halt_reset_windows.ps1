$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "CMSIS-DAP helper source does not exist: $source"
}

if (-not $env:UNO_R4_BASELINE_SERIAL) { throw 'Set UNO_R4_BASELINE_SERIAL before hardware access' }
$env:CMSIS_DAP_TARGET_SERIAL = $env:UNO_R4_BASELINE_SERIAL
$env:CMSIS_DAP_TARGET_INTERFACE = '00'
$env:CMSIS_DAP_SKIP_SWJ_PIN_READBACK = '1'
try {
    Add-Type -Path $source
    $result = [CmsisDapWindowsProbe]::RunD4HaltRunResetRecovery()
    if ($result -ne 0) {
        throw "ESP32-S3 baseline CMSIS-DAP D4 halt/run/reset recovery returned $result"
    }
}
finally {
    Remove-Item Env:CMSIS_DAP_TARGET_SERIAL -ErrorAction SilentlyContinue
    Remove-Item Env:CMSIS_DAP_TARGET_INTERFACE -ErrorAction SilentlyContinue
    Remove-Item Env:CMSIS_DAP_SKIP_SWJ_PIN_READBACK -ErrorAction SilentlyContinue
}

Write-Host 'ESP32_RA4_SWD_D4_HALT_RUN_RESET_PASS'
