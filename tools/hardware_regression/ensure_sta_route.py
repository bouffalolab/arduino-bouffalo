#!/usr/bin/env python3
"""Run a gate on the existing private STA profile and restore the original WLAN.

Profile names and credentials stay in process memory; netsh output is never
logged. A usable route on a different SSID is insufficient for a local peer.
"""
import json,re,subprocess,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from run_bl616cl_apsta_gate import load_private_credentials,DEFAULT_CREDENTIAL_ENV
from run_ra4_wifis3_tls_gate import windows_wlan_address,wsl_unc_path
NATIVE=wsl_unc_path(Path(__file__).with_name("private_sta_windows.cs")).replace("/", "\\")

def command(args,input=None):
 try:return subprocess.run(args,input=input,capture_output=True,timeout=20)
 except (OSError,subprocess.TimeoutExpired):raise RuntimeError('Bounded private WLAN control command failed') from None

def ssid():
 r=command(['netsh.exe','wlan','show','interfaces'])
 names=re.findall(r'^\s*SSID\s*:\s*(.*?)\s*$',r.stdout.decode('utf-8',errors='replace'),re.M)
 return names[0] if len(names)==1 else None

def native_setup():
 return "[Console]::InputEncoding=[Text.Encoding]::UTF8; [Console]::OutputEncoding=[Text.Encoding]::UTF8; Add-Type -TypeDefinition ([IO.File]::ReadAllText('"+NATIVE+"')); $adapter=Get-NetAdapter -Name WLAN; $interfaceId=[Guid]$adapter.InterfaceGuid; "

def profile():
 r=command(['powershell.exe','-NoProfile','-Command',native_setup()+"[BridgePrivateSta]::CurrentProfile($interfaceId) | ConvertTo-Json -Compress"])
 try:
  result=json.loads(r.stdout.decode('utf-8-sig')) if r.stdout.strip() else None
 except (UnicodeDecodeError,json.JSONDecodeError):raise RuntimeError('Private WLAN profile query returned invalid data') from None
 if r.returncode or (result is not None and not isinstance(result,str)):raise RuntimeError('Expected one native WLAN profile')
 return result

def connect_private(target,password):
 script=native_setup()+"$p=([Console]::In.ReadToEnd() | ConvertFrom-Json); $result=[BridgePrivateSta]::Connect($interfaceId,$p.ssid,$p.password); Write-Output $result; if($result -ne 0){exit 1}"
 r=command(['powershell.exe','-NoProfile','-Command',script],input=json.dumps({'ssid':target,'password':password}).encode())
 if r.returncode:raise RuntimeError('Temporary private STA connection request failed')

def connect(name):
 r=command(['netsh.exe','wlan','connect','name='+name,'interface=WLAN'])
 if r.returncode:raise RuntimeError('Saved WLAN profile connection failed')

def wait(expected,by_profile=False):
 deadline=time.monotonic()+45
 while time.monotonic()<deadline:
  if (profile() if by_profile else ssid())==expected:
   try:windows_wlan_address();return
   except RuntimeError:pass
  time.sleep(.5)
 raise RuntimeError('Expected WLAN profile did not acquire a usable IPv4 route')

def main():
 if len(sys.argv)<2:raise RuntimeError('Supply the gate command as arguments')
 target,password=load_private_credentials(DEFAULT_CREDENTIAL_ENV)
 original=profile();changed=ssid()!=target;code=1
 try:
  if changed:connect_private(target,password)
  wait(target)
  print('HOST_PRIVATE_STA_SSID_AND_ROUTE_VERIFIED',flush=True)
  code=subprocess.run(sys.argv[1:]).returncode
 finally:
  if changed:
   if original:
    connect(original);wait(original,by_profile=True)
   else:
    r=command(['netsh.exe','wlan','disconnect','interface=WLAN'])
    if r.returncode:raise RuntimeError('Original disconnected WLAN state restoration failed')
   print('HOST_ORIGINAL_WLAN_RESTORED',flush=True)
 return code
if __name__=='__main__':
 try:raise SystemExit(main())
 except RuntimeError as e:raise SystemExit(str(e)) from None
