#!/usr/bin/env python3
"""Temporarily install the controlled AP on the explicitly authorized ESP baseline.

The immutable full Flash backup is private build data. Restore verifies every
byte, including the original partition table, NVS and application. No reference
source or generated library is written.
"""
import argparse,array,fcntl,hashlib,json,os,subprocess,sys,time,zlib
from pathlib import Path
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from uno_r4_usb_identity import resolve_baseline_esp32_port,tty_properties
from read_bl616cl_windows_telemetry import resolve_wsl_usbip_host
from run_esp32_swd_d4_baseline import reattach_to_wsl
REFERENCE=Path('/home/developer/git/arduino/uno-r4-wifi-usb-bridge')
sys.path.insert(0,str(REFERENCE/'hardware/esp32-patched/esp32/tools/esptool'))
import esptool
SERIAL='TESTESP000001'
PRIVATE=ROOT/'build/esp32-baseline-private'
FIXTURE=ROOT/'build/esp32-static-ap-fixture/compiled/UNOR4USBBridge.ino.bin'
def sha(b):return hashlib.sha256(b).hexdigest()
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
 subprocess.run(['usbipd.exe','wsl','attach','--busid',bus,'--distribution','Ubuntu-22.04'],check=True,timeout=45)
 deadline=time.monotonic()+20
 while time.monotonic()<deadline:
  ports=[]
  for t in Path('/sys/class/tty').glob('ttyACM*'):
   dev=(t/'device').resolve()
   for parent in dev.parents:
    if (parent/'idVendor').exists():
     if (parent/'idVendor').read_text().strip()=='303a' and (parent/'idProduct').read_text().strip()=='1001':ports.append('/dev/'+t.name)
     break
  if len(ports)==1 and Path(ports[0]).exists():break
  time.sleep(.2)
 else:raise RuntimeError('S3 BootROM serial unavailable')
 esp=esptool.detect_chip(port=ports[0],connect_mode='no_reset')
 assert esp.CHIP_NAME=='ESP32-S3' and bytes(esp.read_mac()).hex().upper()==SERIAL,'BootROM silicon identity mismatch'
 print('ESP_FIXTURE_BOOTROM_MAC_VERIFIED',flush=True)
 return esp.run_stub(),bus

def write(esp,offset,data):
 packed=zlib.compress(data,9);esp.flash_defl_begin(len(data),len(packed),offset)
 for i in range(0,len(packed),esp.FLASH_WRITE_SIZE):
  esp.flash_defl_block(packed[i:i+esp.FLASH_WRITE_SIZE],i//esp.FLASH_WRITE_SIZE)
 assert esp.flash_md5sum(offset,len(data)).lower()==hashlib.md5(data).hexdigest(),'Flash MD5 mismatch'
 assert sha(esp.read_flash(offset,len(data)))==sha(data),'Flash SHA256 readback mismatch'
 esp.flash_begin(0,0)
 esp.flash_defl_finish(reboot=False)
 print(f'ESP_FIXTURE_WRITE_VERIFY_PASS offset=0x{offset:x} bytes={len(data)} sha256={sha(data)}',flush=True)

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=['install','restore']);args=parser.parse_args()
 PRIVATE.mkdir(mode=0o700,parents=True,exist_ok=True);os.chmod(PRIVATE,0o700)
 backup=PRIVATE/'original-full-flash.bin';manifest=PRIVATE/'manifest.json'
 if args.mode=='restore':
  m=json.loads(manifest.read_text());data=backup.read_bytes();assert len(data)==0x400000 and sha(data)==m['sha256']
 else:
  assert FIXTURE.is_file(),'compile target fixture first'
  assert not backup.exists() and not manifest.exists(),'preserve existing immutable backup; restore before a new installation'
 esp,bus=enter()
 try:
  if args.mode=='install':
   data=esp.read_flash(0,0x400000)
   assert len(data)==0x400000
   # app0 in the live huge_app table, never infer from source defaults.
   table=data[0x8000:0x9000];entries=[]
   import struct
   for i in range(0,len(table),32):
    if table[i:i+2]!=b'\xaaP':break
    _,typ,sub,off,size,label,flags=struct.unpack('<HBBII16sI',table[i:i+32]);entries.append((typ,sub,off,size,label.rstrip(b'\0').decode()))
   assert (0,0x10,0x10000,0x300000,'app0') in entries,entries
   with backup.open('xb') as f:f.write(data)
   os.chmod(backup,0o600)
   m={'serial':SERIAL,'bytes':len(data),'sha256':sha(data),'partitions':entries,'fixture_sha256':sha(FIXTURE.read_bytes())}
   manifest.write_text(json.dumps(m,indent=2)+'\n');os.chmod(manifest,0o600)
   print('ESP_FIXTURE_PRIVATE_BACKUP_VERIFIED sha256='+sha(data),flush=True)
   image=FIXTURE.read_bytes();assert len(image)<=0x300000
   write(esp,0x10000,image)
  else:
   write(esp,0,data)
   print('ESP_BASELINE_FULL_FLASH_RESTORED',flush=True)
 finally:
  # HID software download flag survives an ordinary RTS reset.
  reg=0x6000812c;esp.write_reg(reg,esp.read_reg(reg)&~1)
  esp.hard_reset();esp._port.close()
  time.sleep(2)
  reattach_to_wsl(resolve_wsl_usbip_host('auto'),bus,20)
 print('ESP_FIXTURE_NORMAL_USB_RESTORED',flush=True)
if __name__=='__main__':main()
