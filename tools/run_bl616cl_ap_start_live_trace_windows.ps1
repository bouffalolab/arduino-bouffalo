[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'verify_bl616cl_cmsis_dap_windows.cs'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "CMSIS-DAP helper source does not exist: $source"
}

Add-Type -Path $source
$result = [CmsisDapWindowsProbe]::RunApStartLiveTraceTelemetry($Port)
if ($result -ne 0) {
    throw "BL616CL live AP-start trace returned $result"
}
