#!/usr/bin/env python3
"""Verify, bind and program a BL616CL development release on Linux/Windows.

Programming requires a binding captured from the normal application at the same
physical USB location. BootROM has no reliable application serial identity.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Imported verifier must not add an unlisted __pycache__ to the sealed package.
sys.dont_write_bytecode = True
from bl616cl_release import SERIAL, require, verify, digest


def ports():
    from serial.tools import list_ports
    return list(list_ports.comports())


def location(port):
    if sys.platform != 'win32':
        kernel=Path('/proc/sys/kernel/osrelease')
        if kernel.exists() and 'microsoft' in kernel.read_text().lower():
            # vhci's 1-1 is a reusable virtual slot, not the physical host port.
            state=json.loads(subprocess.check_output(['usbipd.exe','state'],text=True,timeout=15))
            identity=f'VID_{port.vid:04X}&PID_{port.pid:04X}'
            matches=[d for d in state['Devices'] if d.get('BusId') and d.get('IsWslAttached')
                     and identity in d.get('InstanceId','').upper()
                     and (port.vid==0xffff or d.get('InstanceId','').upper().endswith('\\'+SERIAL))]
            require(len(matches)==1,'cannot uniquely correlate WSL USB device to host location')
            return 'wsl-usbipd:'+matches[0]['BusId']
        require(bool(port.location), 'USB physical location unavailable')
        return port.location.split(':',1)[0]
    # Resolve the USB device ancestor, not the CDC interface location.
    script = r'''
param([string]$PortName)
$ErrorActionPreference='Stop'
$d=@(Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -match ('\(' + [regex]::Escape($PortName) + '\)$') })
if ($d.Count -ne 1) { throw 'serial PnP identity is ambiguous' }
$id=$d[0].InstanceId
for ($i=0; $i -lt 8; $i++) {
 if ($id -match '^USB\\VID_' -and $id -notmatch '&MI_') {
  $p=Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_LocationPaths'
  if (-not $p.Data) { throw 'USB location missing' }
  Write-Output @($p.Data)[0]; exit 0
 }
 $id=(Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_Parent').Data
}
throw 'USB parent not found'
'''
    with tempfile.TemporaryDirectory() as work:
        path=Path(work)/'location.ps1';path.write_text(script)
        return subprocess.check_output(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass',
                                       '-File',str(path),port.device],text=True,timeout=20).strip()


def selected_port(name):
    found=[p for p in ports() if p.device == name]
    require(len(found)==1,'serial device absent or ambiguous: '+name)
    return found[0]


def bind(port_name, output):
    port=selected_port(port_name)
    require((port.vid,port.pid,port.serial_number)==(0x2341,0x1002,SERIAL),
            'binding requires the dedicated normal BL616CL serial identity')
    # CDC00 is the only serial interface on this composite bridge.
    with __import__('serial').Serial(port.device,115200,timeout=.1,write_timeout=1):pass
    binding={'schema':1,'serial':SERIAL,'chip':'bl616cl','location':location(port),
             'host':sys.platform,'normal_vid':0x2341,'normal_pid':0x1002}
    require(not output.exists(),'binding exists; use a new file after reviewing changed cabling')
    output.write_text(json.dumps(binding,indent=2)+'\n')
    print('BL616CL_TARGET_BIND_PASS '+json.dumps(binding))


def bootrom(port_name,binding):
    port=selected_port(port_name)
    require((port.vid,port.pid)==(0xffff,0xffff),'selected port is not BL616CL BootROM')
    require(binding.get('schema')==1 and binding.get('serial')==SERIAL and
            binding.get('chip')=='bl616cl' and binding.get('host')==sys.platform,
            'wrong binding chip/host/serial')
    require(location(port)==binding.get('location'),'BootROM physical USB location differs from bound board')
    return port


def flash(directory,port_name,binding_path,tool,attempts,timeout):
    # All local rejection gates precede opening a device or launching a writer.
    manifest=verify(directory)
    require(tool.is_file(),'FlashCube executable missing')
    binding=json.loads(binding_path.read_text())
    bootrom(port_name,binding)
    require(1 <= attempts <= 3 and timeout>0,'invalid retry/timeout bounds')
    tool=tool.resolve()
    # A private staged copy avoids path quoting/tool mutations and TOCTOU between attempts.
    with tempfile.TemporaryDirectory(prefix='bl616cl-release-') as work:
        import shutil
        stage=Path(work)
        for role in ('boot2','partition','application'):
            name=manifest['images'][role]['file']
            shutil.copyfile(directory/name,stage/name)
            require(digest((stage/name).read_bytes())==manifest['files'][name]['sha256'],'staged hash mismatch')
        command=[str(tool),'--interface=uart','--chipname=bl616cl','--port='+port_name,
                 '--baudrate=2000000','write_flash_files']
        for role in ('boot2','partition','application'):
            row=manifest['images'][role]
            command += [hex(row['address']),str(stage/row['file'])]
        # Existing FOTA may leave a newer valid table at F000 selecting B.
        # A factory recovery must replace both redundant copies to select A.
        command += ['0xf000',str(stage/'partition.bin')]
        for attempt in range(1,attempts+1):
            bootrom(port_name,binding)
            print(f'BL616CL_RELEASE_FLASH_ATTEMPT={attempt}/{attempts}',flush=True)
            try:
                result=subprocess.run(command,cwd=tool.parent,stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT,timeout=timeout)
                text=result.stdout.decode('utf-8',errors='replace');print(text,flush=True)
                require(result.returncode==0,'FlashCube nonzero exit')
                require(len(re.findall('verification succeeded',text,re.I))==4,
                        'four device-side SHA checks required (boot2, both tables, application)')
                print('BL616CL_RELEASE_FLASH_VERIFIED_PASS version='+manifest['firmware_version'],flush=True)
                print('Reset to Flash boot, then verify USB identity and runtime version; programming alone is not runtime validation.')
                return
            except (ValueError,subprocess.TimeoutExpired):
                if attempt==attempts:raise
                print('BL616CL_RELEASE_FLASH_RETRY',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    sub.add_parser('list')
    p=sub.add_parser('verify');p.add_argument('directory',type=Path)
    p=sub.add_parser('bind');p.add_argument('--port',required=True);p.add_argument('--output',type=Path,required=True)
    p=sub.add_parser('flash');p.add_argument('directory',type=Path)
    p.add_argument('--port',required=True);p.add_argument('--binding',type=Path,required=True)
    p.add_argument('--flash-tool',type=Path,required=True);p.add_argument('--attempts',type=int,default=3)
    p.add_argument('--timeout',type=int,default=90)
    args=parser.parse_args()
    if args.command=='list':
        for p in ports():
            if p.vid is None:continue
            try:physical_location=location(p)
            except (ValueError,subprocess.SubprocessError):physical_location=None
            print(json.dumps({'port':p.device,'vid':p.vid,'pid':p.pid,'serial':p.serial_number,'location':physical_location}))
    elif args.command=='verify':
        print('BL616CL_RELEASE_VERIFY_PASS version='+verify(args.directory)['firmware_version'])
    elif args.command=='bind':bind(args.port,args.output)
    else:flash(args.directory,args.port,args.binding,args.flash_tool,args.attempts,args.timeout)

if __name__=='__main__':
    try:main()
    except (ValueError,OSError,KeyError,TypeError,subprocess.SubprocessError) as error:
        raise SystemExit('REJECTED: '+str(error))
