#!/usr/bin/env python3
"""Temporarily install the controlled AP on the explicitly authorized ESP baseline.

The immutable full Flash backup is private build data. Restore verifies every
byte, including the original partition table, NVS and application. No reference
source or generated library is written.
"""
import argparse,array,fcntl,json,os,subprocess,sys,time
from pathlib import Path
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from uno_r4_usb_identity import resolve_baseline_esp32_port,tty_properties
from read_bl616cl_windows_telemetry import resolve_wsl_usbip_host
from run_esp32_swd_d4_baseline import reattach_to_wsl
SERIAL='TESTESP000001'
PRIVATE=ROOT/'build/esp32-baseline-private'
def state():return json.loads(subprocess.check_output(['usbipd.exe','state'],text=True,timeout=30))['Devices']
def enter():
 port=resolve_baseline_esp32_port('auto');p=tty_properties(port)
 assert p['ID_SERIAL_SHORT']==SERIAL and p['ID_USB_INTERFACE_NUM']=='01',p
 with open(port,'rb',buffering=0):pass
 devices=[d for d in state() if d.get('BusId') and d.get('InstanceId','').upper().endswith('\\'+SERIAL)]
 assert len(devices)==1,'expected exact baseline Windows serial'
 bus=devices[0]['BusId'];hid=[]
 for path in Path('/sys/class/hidraw').glob('hidraw*'):
  u=(path/'device/uevent').read_text()
  if 'HID_UNIQ='+SERIAL+'\n' in u and 'HID_ID=0003:00002341:00001002' in u:hid.append('/dev/'+path.name)
 assert len(hid)==1,'expected exact baseline HID'
 print('ESP_FIXTURE_TARGET chip=ESP32-S3 serial='+SERIAL+' bus='+bus,flush=True)
 with open(hid[0],'rb+',buffering=0) as f:
  request=array.array('B',[0,0xAA]+[0]*63)
  fcntl.ioctl(f.fileno(),0xC0414806,request,True)
 time.sleep(2)
 current=[d for d in state() if d.get('BusId')==bus]
 assert len(current)==1 and 'VID_303A&PID_1001' in current[0]['InstanceId'].upper(),'expected S3 BootROM on verified physical bus'
 return bus

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=['install','restore']);parser.add_argument('--resume-bootrom',action='store_true');args=parser.parse_args()
 if args.resume_bootrom:
  matches=[d for d in state() if d.get('BusId') and d['InstanceId'].upper()=='USB\\VID_303A&PID_1001\\00:00:00:00:00:01']
  assert len(matches)==1,'resume requires exact authorized silicon USB serial'
  bus=matches[0]['BusId']
 else:bus=enter()
 from run_ra4_wifis3_tls_gate import wsl_unc_path
 worker=ROOT/'tools/migration/esp_fixture_flash_windows.py'
 result=None
 try:
  result=subprocess.run(['powershell.exe','-NoProfile','-Command',f'& py.exe -3 -u "{wsl_unc_path(worker)}" {args.mode}'],timeout=1500)
 finally:
  # The normal-mode identity was already shared before entering BootROM.
  devices=[d for d in state() if d.get('BusId')==bus]
  if len(devices)==1 and devices[0]['InstanceId'].upper().endswith('\\'+SERIAL) and 'VID_2341&PID_1002' in devices[0]['InstanceId'].upper():
   reattach_to_wsl(resolve_wsl_usbip_host('auto'),bus,20)
 if result is None or result.returncode:raise RuntimeError('ESP fixture native COM operation failed; preserve private backup and inspect log')
 # chmod on Windows UNC may not retain Linux permission bits; enforce here.
 if PRIVATE.exists():
  os.chmod(PRIVATE,0o700)
  for p in PRIVATE.iterdir():
   if p.is_file():os.chmod(p,0o600)
 print('ESP_FIXTURE_NORMAL_USB_RESTORED',flush=True)
if __name__=='__main__':main()
