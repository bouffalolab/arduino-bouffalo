#!/usr/bin/env python3
"""Run the actual PowerShell retry loop with mocked device/write operations."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/run_bl616cl_final_carrier_flash_windows.ps1'


def main():
    shell = shutil.which('powershell.exe')
    if shell is None:
        print('BL616CL_WINDOWS_FLASH_RETRY_SKIP powershell.exe unavailable')
        return
    source = SCRIPT.read_text()
    loop = source[source.index('for ($index = 0; $index -lt $groups.Count;'):
                  source.index('Write-Host "FINAL_CARRIER_FLASH_PASS')]
    prefix = r'''
$ErrorActionPreference = 'Stop'
$SegmentAttempts = 3
$BootRomTimeoutSeconds = 1
$bootRom = 'COM51'
$flashToolPath = $flashToolDirectory = $stageRoot = 'mock'
$BaudRate = $FlashTimeoutSeconds = 1
$groups = @(@('boot2', 'partition'), @('application'))
$verificationCount = 0
$script:writes = 0
$script:checks = 0
$script:attempts = @{}
function Start-Sleep { param([int]$Milliseconds) }
function Get-BootRomPort {
    param($RequestedPort, $TimeoutSeconds)
    ++$script:checks
    if ($script:scenario -eq 2) { throw 'wrong target identity' }
    return $RequestedPort
}
function Invoke-FlashCubeSegment {
    param($FlashToolPath, $FlashToolDirectory, $BootRomPort, $BaudRate,
          $TimeoutSeconds, $StageRoot, $Segments, [int]$Index)
    ++$script:writes
    $group = [int][Math]::Floor($Index / 3)
    if (-not $script:attempts.ContainsKey($group)) { $script:attempts[$group] = 0 }
    ++$script:attempts[$group]
    if ($script:scenario -eq 1 -or $script:attempts[$group] -eq 1) {
        throw 'missing device-side SHA verification'
    }
    return $Segments.Count
}
'''
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'retry.ps1'
        for scenario, checks in ((0, r'''
if ($verificationCount -ne 3 -or $script:writes -ne 4 -or $script:checks -ne 4) {
    throw 'retry counted failed checks or missed identity validation'
}
if ($failed) { throw 'recovery failed' }
'''), (1, r'''
if (-not $failed -or $verificationCount -ne 0 -or $script:writes -ne 3) {
    throw 'exhausted retries did not preserve failure'
}
'''), (2, r'''
if (-not $failed -or $verificationCount -ne 0 -or $script:writes -ne 0) {
    throw 'identity failure allowed a write'
}
''')):
            path.write_text(prefix + f'\n$script:scenario = {scenario}\n' +
                            '$failed = $false\ntry {\n' + loop +
                            '\n} catch { $failed = $true }\n' + checks)
            windows_path = subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()
            subprocess.run([shell, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                            '-File', windows_path], check=True, capture_output=True)
    print('BL616CL_WINDOWS_FLASH_RETRY_PASS')


if __name__ == '__main__':
    main()
