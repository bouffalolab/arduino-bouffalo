"""Repair WSL forwarding of the dedicated BL, without resetting either chip."""
from pathlib import Path
import ipaddress
import json
import re
import subprocess
import time
from uno_r4_usb_identity import resolve_paired_bl616cl_port


def stale_bl_ports(sys_root=Path('/sys')):
    """Call only after Windows confirms the dedicated chip is not attached."""
    result=[]
    for controller in sorted((sys_root/'devices/platform').glob('vhci_hcd.*')):
        if not re.fullmatch(r'vhci_hcd\.\d+',controller.name):continue
        for row in (controller/'status').read_text().splitlines()[1:]:
            values=row.split()
            if len(values)!=7 or values[0]!='hs' or values[2]!='006':continue
            if not values[1].isdigit() or not re.fullmatch(r'\d+-[\d.]+',values[6]):continue
            device=sys_root/'bus/usb/devices'/values[6]
            try:
                identity=tuple((device/name).read_text().strip() for name in ('idVendor','idProduct','serial'))
            except OSError:continue
            if identity==('2341','1002','BL616CL-PROVISIONAL'):
                result.append((controller/'detach',int(values[1])))
    return result


def usbip_listing():
    # Windows enumeration can stall beyond 10 s; only these read-only queries
    # get a longer deadline. Identity validation and attach deadlines stay strict.
    for attempt in range(2):
        try:
            return subprocess.run(['usbipd.exe','list'],capture_output=True,text=True,
                                  errors='replace',timeout=30,check=True).stdout
        except subprocess.TimeoutExpired:
            if attempt: raise
            print('BL616CL_USB_ENUMERATION_TIMEOUT_RETRY',flush=True)


def dedicated_bus_id():
    """Resolve the dedicated chip by serial; USB sockets can change on replug."""
    for attempt in range(2):
        try:
            result=subprocess.run(['usbipd.exe','state'],capture_output=True,timeout=30,check=True)
            break
        except subprocess.TimeoutExpired:
            if attempt: raise
            print('BL616CL_USB_IDENTITY_QUERY_TIMEOUT_RETRY',flush=True)
    state=json.loads(result.stdout.decode('utf-8-sig'))
    matches=[d for d in state['Devices'] if d.get('BusId') and
             d.get('InstanceId','').upper()=='USB\\VID_2341&PID_1002\\BL616CL-PROVISIONAL']
    if len(matches)!=1 or not re.fullmatch(r'\d+-[\d.]+',matches[0]['BusId']):
        raise RuntimeError('expected one normal dedicated BL USB serial')
    return matches[0]['BusId']


def ensure_bl_usb_mapping(prepared_attach=None):
    bus_id=dedicated_bus_id()
    listing=usbip_listing()
    lines=[line for line in listing.splitlines() if re.match(rf'^{re.escape(bus_id)}\s+2341:1002\s+',line)]
    if len(lines)!=1:raise RuntimeError('dedicated serial bus is not a normal 2341:1002 device')
    line=lines[0].rstrip()
    if not line.endswith('Attached'):
        if not (line.endswith('Shared') or line.endswith('Not shared')):
            raise RuntimeError('unsupported dedicated USB forwarding state')
        if prepared_attach is not None:
            if not line.endswith('Shared'):
                raise RuntimeError('prepared attach requires a shared dedicated device')
            prepared_attach.attach()
            return wait_bl_port()
        # A broken transport can leave a USED vhci port and an uninterruptible
        # tty task even though Windows no longer owns an attached connection.
        # Detach only this serial, never a different board or physical USB port.
        for path,port in stale_bl_ports():
            subprocess.run(['wsl.exe','-d','Ubuntu-22.04','-u','root','--','sh','-c',
                            f'echo {port} > {path}'],capture_output=True,timeout=10,check=True)
            print(f'BL616CL_STALE_VHCI_DETACH_PASS={path.parent.name},{port}',flush=True)
        try:
            subprocess.run(['usbipd.exe','wsl','attach','--busid',bus_id,'--distribution','Ubuntu-22.04'],
                           capture_output=True,timeout=10,check=True)
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
            # usbipd's WSL launcher can time out while the device remains Shared.
            # Recheck ownership before using the native client on this bus only.
            listing=usbip_listing()
            lines=[row for row in listing.splitlines() if re.match(rf'^{re.escape(bus_id)}\s+2341:1002\s+',row)]
            if len(lines)!=1:raise RuntimeError('dedicated USB identity changed during attach')
            if not lines[0].rstrip().endswith('Attached'):
                if not lines[0].rstrip().endswith('Shared'):
                    raise RuntimeError('dedicated USB is not shared after attach failure')
                for path,port in stale_bl_ports():
                    subprocess.run(['wsl.exe','-d','Ubuntu-22.04','-u','root','--','sh','-c',
                                    f'echo {port} > {path}'],capture_output=True,timeout=10,check=True)
                routes=subprocess.run(['ip','-4','route','show','default'],capture_output=True,
                                      text=True,timeout=5,check=True).stdout
                gateways=re.findall(r'^default via (\S+)',routes,re.MULTILINE)
                if len(set(gateways))!=1:raise RuntimeError('no unique WSL host gateway')
                gateway=str(ipaddress.IPv4Address(gateways[0]))
                subprocess.run(['wsl.exe','-d','Ubuntu-22.04','-u','root','--','usbip',
                                'attach','-r',gateway,'-b',bus_id],
                               capture_output=True,timeout=15,check=True)
                print('BL616CL_USB_NATIVE_ATTACH_FALLBACK_PASS',flush=True)
        print('BL616CL_USB_FORWARD_REATTACH_PASS',flush=True)
    return wait_bl_port()


def wait_bl_port():
    deadline=time.monotonic()+20
    while True:
        try:return resolve_paired_bl616cl_port('auto')
        except (OSError,RuntimeError):
            if time.monotonic()>=deadline:raise
            time.sleep(.2)
