#!/usr/bin/env python3
"""Flash only the dedicated BL through the existing FTDI/BootROM fixture."""
import os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]; sys.path.insert(0,str(ROOT/'tools'))
from run_ra4_wifis3_tls_gate import wsl_unc_path
from run_bl616cl_generic_at_final_carrier import restore_usb
build=ROOT/'build/bridge-image'; ev=ROOT/'build/migration-evidence'; hold=ev/'flash.hold'; hold.touch()
helper=None
try:
 # Inspect live Windows identities first; helper independently validates exactly one FTDI.
 subprocess.run(['usbipd.exe','list'],check=True,timeout=30)
 output=(ev/'ftdi-flash.log').open('w')
 helper=subprocess.Popen(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',wsl_unc_path(ROOT/'tools/reset_bl616cl_ftdi_windows.ps1'),'-DownloadMode','-HoldUntilReleasedPath',wsl_unc_path(hold)],stdout=output,stderr=subprocess.STDOUT)
 deadline=time.monotonic()+25
 while 'BL616CL_FTDI_HOLD_ACTIVE' not in (ev/'ftdi-flash.log').read_text():
  if helper.poll() is not None or time.monotonic()>deadline: raise RuntimeError('dedicated FTDI download hold not ready')
  time.sleep(.2)
 subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',wsl_unc_path(ROOT/'tools/run_bl616cl_final_carrier_flash_windows.ps1'),'-Boot2',wsl_unc_path(build/'UNOR4BL616CLBridge.ino.boot2.bin'),'-Partition',wsl_unc_path(build/'UNOR4BL616CLBridge.ino.partition.bin'),'-Application',wsl_unc_path(build/'UNOR4BL616CLBridge.ino.bin')],check=True,timeout=240)
finally:
 hold.unlink(missing_ok=True)
 if helper is not None:
  helper.wait(timeout=12); output.close()
restore_usb(physical_reset=True)
print('TARGET_REBUILT_BL_FLASH_AND_NORMAL_USB_PASS')
