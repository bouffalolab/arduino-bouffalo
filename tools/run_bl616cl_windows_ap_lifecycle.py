#!/usr/bin/env python3
"""Read dedicated BL AP diagnostics through native Windows HID.

Use while the dedicated device is Shared to Windows. Copy only the C# probe to
an owned temporary Windows directory so execution does not require a working
WSL UNC file server. This does not reset, flash or change network settings.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix='bl616cl-hid-',dir='/mnt/c/Users/Public') as directory:
        work=Path(directory)
        shutil.copyfile(ROOT/'tools/verify_bl616cl_cmsis_dap_windows.cs',work/'probe.cs')
        windows='C:\\Users\\Public\\'+work.name+'\\probe.cs'
        code=("$ErrorActionPreference='Stop'; "
              "$env:CMSIS_DAP_TARGET_SERIAL='BL616CL-PROVISIONAL'; "
              "$env:CMSIS_DAP_TARGET_INTERFACE='02'; "
              "$env:BL616CL_CMSIS_DAP_ALLOW_UNSERIALIZED_TARGET='0'; "
              "Add-Type -Path '"+windows+"'; "
              "[void][CmsisDapWindowsProbe]::RunApStartTelemetry(); "
              "[void][CmsisDapWindowsProbe]::RunApStartHostapdLifecycleTelemetry(); "
              "[void][CmsisDapWindowsProbe]::RunApStartHostapdStateUpdateTelemetry(); "
              "[void][CmsisDapWindowsProbe]::RunApStartStatusCodeTelemetry()")
        subprocess.run(['powershell.exe','-NoProfile','-Command',code],check=True,timeout=30)
    print('WINDOWS_NATIVE_AP_LIFECYCLE_PASS',flush=True)


if __name__=='__main__':main()
