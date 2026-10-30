#!/usr/bin/env python3
"""Independent IPv6 echo gate on the dedicated BL SoftAP diagnostic firmware.

Uses generated AP credentials, prepared Windows/root processes, and restores
Windows WLAN, USB mapping and the RA4M1 scan Sketch on exit.
"""
import argparse
import ipaddress
import os
from pathlib import Path
import secrets
import select
import subprocess
import sys
import time
import serial
from bl616cl_prepared_usb_attach import PreparedBlAttach
from bl616cl_usb_mapping import ensure_bl_usb_mapping
from bl616cl_ipv6_tcp_windows_peer import PreparedIPv6Peer
from bl616cl_softap_test_session import SoftApTestSession
from inspect_bl616cl_gpio35 import find_hidraw, read_feature, select_feature_page, FEATURE_VERSION
from inspect_bl616cl_ipv6_tcp import log_snapshot
import run_bl616cl_hostname_ipv6_final_carrier as control

ROOT = Path(__file__).resolve().parents[1]


class PreparedDetach:
    def __init__(self):
        code = r'''
import sys
sys.path.insert(0,sys.argv[1])
from bl616cl_usb_mapping import stale_bl_ports
print('IPV6_ROOT_DETACH_READY',flush=True)
if sys.stdin.readline().strip()!='DETACH':sys.exit(0)
for path,port in stale_bl_ports():path.write_text(str(port))
print('IPV6_ROOT_DETACH_EXIT',flush=True)
'''
        self.process = subprocess.Popen(['wsl.exe','-d','Ubuntu-22.04','-u','root','--',
            'python3','-u','-c',code,str(ROOT/'tools')], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            if not select.select([self.process.stdout],[],[],10)[0] or self.process.stdout.readline().strip()!=b'IPV6_ROOT_DETACH_READY':
                raise RuntimeError('IPv6 detach helper not ready')
        except Exception:
            self.close();raise
        print('IPV6_ROOT_DETACH_READY',flush=True)

    def detach(self):
        output,_=self.process.communicate(input=b'DETACH\n',timeout=10)
        if self.process.returncode or b'IPV6_ROOT_DETACH_EXIT' not in output:
            raise RuntimeError('IPv6 dedicated detach failed')
        print('IPV6_ROOT_DETACH_EXIT',flush=True)

    def close(self):
        if self.process.poll() is None:
            self.process.communicate(input=b'CANCEL\n',timeout=5)


def ap_address():
    fd=os.open(find_hidraw(),os.O_RDWR|os.O_NONBLOCK)
    try:
        select_feature_page(fd,62);r=read_feature(fd)
        if r[:8]!=b'U35D'+bytes((FEATURE_VERSION,62,1,1)):
            raise RuntimeError('IPv6 AP diagnostic unavailable')
        address=ipaddress.IPv6Address(r[8:24])
        return str(address) if r[24]&0x10 and address.is_link_local else None
    finally:
        try:select_feature_page(fd,0)
        finally:os.close(fd)


def cleanup_actions(actions):
    """Attempt every independent recovery, retaining the first failure."""
    first = None
    for label, action in actions:
        try:
            action()
        except Exception as error:
            print(label + '_FAILED', flush=True)
            if first is None:
                first = error
    return first


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--arduino-cli',default='arduino-cli')
    parser.add_argument('--runtime-settle-ms',type=int,default=1000)
    args=parser.parse_args();args.work_dir.mkdir(parents=True,exist_ok=False)
    code="import sys,os;sys.path.insert(0,"+repr(str(ROOT/'tools'))+");from bl616cl_usb_mapping import ensure_bl_usb_mapping;p=ensure_bl_usb_mapping();f=os.open(p,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK);os.close(f);print('IPV6_SOFTAP_BL_IDENTITY_OPEN')"
    subprocess.run(['wsl.exe','-d','Ubuntu-22.04','-u','root','--','python3','-c',code],check=True,timeout=45)
    args.bridge_port=ensure_bl_usb_mapping()
    peer=first_attach=back_attach=first_detach=back_detach=None
    session=SoftApTestSession(args.work_dir)
    modified=prepared=False
    primary=None
    try:
        peer=PreparedIPv6Peer()
        first_attach=PreparedBlAttach();back_attach=PreparedBlAttach()
        first_detach=PreparedDetach();back_detach=PreparedDetach()
        ssid='BL6-V6-'+secrets.token_hex(4);password=secrets.token_hex(8)
        # Prepare all Windows processes before the WLAN switch.
        prepared=True;session.prepare_association(args,ssid,password,'192.168.4.1')
        image=control.compile_proxy(args);modified=True;control.upload_proxy(args,image)
        with serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=1) as link:
            link.dtr=False;link.rts=False;time.sleep(1)
            control.require_exact(control.transact(link,b'AT\r\n',10),b'OK\r\n','proxy')
            control.require_exact(control.transact(link,b'AT+WIFIMODE=2\r\n',10),b'+WIFIMODE: \r\nOK\r\n','AP mode')
            request=f'AT+BEGINSOFTAP={ssid},{password},6\r\n'.encode('ascii')
            control.require_exact(control.transact(link,request,10),b'+BEGINSOFTAP: 1\r\nOK\r\n','AP start')
        deadline=time.monotonic()+20;address=None
        while time.monotonic()<deadline:
            address=ap_address()
            if address:break
            time.sleep(.25)
        if not address:raise RuntimeError('SoftAP IPv6 valid address deadline')
        print('IPV6_SOFTAP_ADDRESS_VALID',flush=True)
        first_detach.detach();session.connect_prepared()
        args.bridge_port=ensure_bl_usb_mapping(first_attach)
        log_snapshot()
        try:peer.exchange(address)
        finally:log_snapshot()
        print('IPV6_SOFTAP_TCP_DATA_PASS',flush=True)
    except Exception as error:
        primary=error
    finally:
        actions=[]
        if peer is not None:actions.append(('IPV6_PEER_CLEANUP',peer.close))
        if prepared:
            # Network/profile recovery must run even if USB detach fails.
            actions.append(('IPV6_USB_DETACH_CLEANUP',back_detach.detach))
            actions.append(('IPV6_WINDOWS_NETWORK_CLEANUP',session.close))
            def restore_mapping():
                args.bridge_port=ensure_bl_usb_mapping(back_attach)
                print('IPV6_SOFTAP_WINDOWS_WLAN_USB_RESTORED',flush=True)
            actions.append(('IPV6_USB_RESTORE',restore_mapping))
        if modified:
            def restore_mode():
                args.bridge_port=ensure_bl_usb_mapping()
                with serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=1) as link:
                    link.dtr=False;link.rts=False
                    control.require_exact(control.transact(link,b'AT+WIFIMODE=1\r\n',10),b'+WIFIMODE: \r\nOK\r\n','restore STA mode')
            actions.append(('IPV6_STA_MODE_CLEANUP',restore_mode))
            actions.append(('IPV6_SCAN_CLEANUP',lambda:control.restore_scan(args)))
        for helper in (first_attach,back_attach,first_detach,back_detach):
            if helper is not None:actions.append(('IPV6_HELPER_CLEANUP',helper.close))
        cleanup_error=cleanup_actions(actions)
        if primary is None:primary=cleanup_error
    if primary is not None:raise primary
    print('IPV6_SOFTAP_GATE_PASS',flush=True)


if __name__=='__main__':
    main()
