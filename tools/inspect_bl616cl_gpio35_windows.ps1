[CmdletBinding()]
param(
    [ValidateSet(
        'read',
        'uart0-rx',
        'high-impedance-input',
        'uart0-rx-no-pullup',
        'pad-disabled'
    )]
    [string]$Mode = 'read'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "missing CMSIS-DAP Windows source: $source"
}

Add-Type -Path $source
[void][CmsisDapWindowsProbe]::RunUserUartGpio35Diagnostic($Mode)
