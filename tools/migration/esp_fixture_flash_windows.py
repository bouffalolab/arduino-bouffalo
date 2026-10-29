#!/usr/bin/env python3
"""Windows COM backend: exact ESP BootROM identity, private backup and readback."""
import argparse,hashlib,json,os,re,subprocess,sys,time,zlib
from pathlib import Path
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
REFERENCE=ROOT.parent/'uno-r4-wifi-usb-bridge'
sys.path.insert(0,str(REFERENCE/'hardware/esp32-patched/esp32/tools/esptool'))
import esptool
SERIAL='TESTESP000001'
PRIVATE=ROOT/'build/esp32-baseline-private'
FIXTURE=ROOT/'build/esp32-static-ap-fixture/compiled/UNOR4USBBridge.ino.bin'
def sha(b):return hashlib.sha256(b).hexdigest()
def enter():
 assert os.name=='nt','use the WSL identity-verifying wrapper'
 devices=json.loads(subprocess.check_output(['usbipd.exe','state']))['Devices']
 matches=[d for d in devices if d.get('BusId') and d['InstanceId'].upper()=='USB\\VID_303A&PID_1001\\00:00:00:00:00:01']
 assert len(matches)==1,'expected exactly authorized S3 BootROM identity'
 device=matches[0];ports=re.findall(r'\bCOM[0-9]+\b',device['Description'])
 assert len(ports)==1 and not device.get('IsWslAttached'),'expected one Windows-owned BootROM COM port'
 print('ESP_FIXTURE_WINDOWS_BOOTROM_TARGET bus='+device['BusId']+' port='+ports[0],flush=True)
 esp=esptool.detect_chip(port=ports[0],connect_mode='no_reset')
 assert esp.CHIP_NAME=='ESP32-S3' and bytes(esp.read_mac()).hex().upper()==SERIAL,'BootROM silicon identity mismatch'
 print('ESP_FIXTURE_BOOTROM_MAC_VERIFIED',flush=True)
 stub=esp.run_stub()
 stub.change_baud(921600)
 return stub,device['BusId']
def read(esp,offset,size):
 def progress(done,total):
  if done%0x40000==0 or done==total:print(f'ESP_FLASH_READ_PROGRESS={done}/{total}',flush=True)
 return esp.read_flash(offset,size,progress)

def write(esp,offset,data):
 packed=zlib.compress(data,9);esp.flash_defl_begin(len(data),len(packed),offset)
 for i in range(0,len(packed),esp.FLASH_WRITE_SIZE):
  esp.flash_defl_block(packed[i:i+esp.FLASH_WRITE_SIZE],i//esp.FLASH_WRITE_SIZE)
 assert esp.flash_md5sum(offset,len(data)).lower()==hashlib.md5(data).hexdigest(),'Flash MD5 mismatch'
 assert sha(read(esp,offset,len(data)))==sha(data),'Flash SHA256 readback mismatch'
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
   data=read(esp,0,0x400000)
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
  # The WSL owner reattaches the already-shared normal-mode identity.
 print('ESP_FIXTURE_NORMAL_USB_RESTORED',flush=True)
if __name__=='__main__':main()
