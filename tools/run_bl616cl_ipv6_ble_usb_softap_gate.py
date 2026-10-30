#!/usr/bin/env python3
"""Targeted IPv6 diagnostic data with live BLE 32/32 and numbered USB echoes.

This is separate from the existing 24 connection/TLS resource pressure gate.
Uses only the dedicated BL paired board and a generated temporary SoftAP.
"""
import argparse
import json
from pathlib import Path
import secrets
import subprocess
import threading
import time

import serial
from bl616cl_ipv6_tcp_windows_peer import PreparedIPv6Peer
from bl616cl_prepared_usb_session import PreparedUsbSession
from bl616cl_softap_test_session import SoftApTestSession
from bl616cl_usb_mapping import ensure_bl_usb_mapping
from inspect_bl616cl_ap_lifecycle import capture as capture_ap
from inspect_bl616cl_ipv6_tcp import capture as capture_ipv6
from run_bl616cl_ipv6_tcp_softap_gate import ap_address, cleanup_actions
from run_bl616cl_tcp_rx_multi_softap_gate import wait_ap_ready, wait_empty
import run_bl616cl_hostname_ipv6_final_carrier as control

ROOT=Path(__file__).resolve().parents[1]


def marker(link, prefix, timeout=10):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        line=link.readline().decode('ascii',errors='replace').strip()
        if line.startswith('V6_COEX:FAIL='):raise RuntimeError(line)
        if line.startswith(prefix):
            print(line,flush=True)
            return line[len(prefix):]
    raise RuntimeError('IPv6 coexistence Sketch marker deadline')


def start_ble(link, before_ap=False):
    link.write(b'PRESCAN\n' if before_ap else b'SCAN\n');link.flush()
    if marker(link,'V6_COEX:ACTIVE_SCAN=',20) != '32,32':
        raise RuntimeError('unexpected BLE scan parameters')
    time.sleep(1)
    link.write(b'SNAP\n');link.flush()
    reports=int(marker(link,'V6_COEX:BLE_REPORTS='))
    if reports<=0:raise RuntimeError('No live BLE reports at startup')
    return reports


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--arduino-cli',default='arduino-cli')
    parser.add_argument('--runtime-settle-ms',type=int,default=1000)
    parser.add_argument('--cycles',type=int,default=1,choices=range(1,21))
    parser.add_argument('--without-ble-control',action='store_true',
                        help='IPv6/USB control only; never counts as BLE coexistence pass')
    parser.add_argument('--ble-first-start',action='store_true',
                        help='Require live BLE reports before starting SoftAP')
    args=parser.parse_args()
    if args.ble_first_start and args.without_ble_control:
        parser.error('BLE-first requires BLE enabled')
    args.work_dir.mkdir(exist_ok=False,parents=True)
    code="import sys,os;sys.path.insert(0,"+repr(str(ROOT/'tools'))+");from uno_r4_usb_identity import resolve_paired_bl616cl_port;p=resolve_paired_bl616cl_port('auto');f=os.open(p,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK);os.close(f);print('V6_COEX_BL_IDENTITY_OPEN')"
    subprocess.run(['wsl.exe','-d','Ubuntu-22.04','-u','root','--','python3','-c',code],check=True,timeout=45)
    args.bridge_port=ensure_bl_usb_mapping()
    control.require_final_carrier_bridge(args.bridge_port)
    print('V6_COEX_TARGET=dedicated BL616CL current image; RA4M1 temporary Sketch; scan restored',flush=True)
    build=args.work_dir/'ra4-build'
    subprocess.run([args.arduino_cli,'compile','--fqbn','arduino:renesas_uno:unor4wifi',
        '--library',str(ROOT/'libraries/ArduinoBLE'),'--build-path',str(build),
        str(ROOT/'examples/ra4-ipv6-ble-usb-coex')],check=True)
    image=build/'ra4-ipv6-ble-usb-coex.ino.bin'
    peer=helper=link=thread=None
    session=SoftApTestSession(args.work_dir)
    modified=prepared=stopped=False
    primary=None
    errors=[]
    try:
        wait_empty()
        helper=PreparedUsbSession()
        peer=PreparedIPv6Peer(args.cycles,args.work_dir/'ipv6-peer-preparation.stderr.log')
        ssid='BL6-V6COEX-'+secrets.token_hex(4);password=secrets.token_hex(8)
        prepared=True
        session.prepare_association(args,ssid,password,'192.168.4.1')
        subprocess.run([args.arduino_cli,'board','list'],check=True)
        modified=True
        control.upload_proxy(args,image)
        link=serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=2)
        link.dtr=False;link.rts=False
        marker(link,'V6_COEX:READY')
        if args.ble_first_start:
            prescan=start_ble(link,before_ap=True)
            print('V6_COEX_START_ORDER=BLE_FIRST reports='+str(prescan),flush=True)
        else:
            print('V6_COEX_START_ORDER=AP_FIRST',flush=True)
        previous=capture_ap()['2']['attempts']
        link.write(f'START {ssid} {password}\n'.encode('ascii'));link.flush()
        marker(link,'V6_COEX:AP_REQUESTED',20)
        wait_ap_ready(previous)
        link.close();link=None
        helper.detach()
        session.connect_prepared()
        args.bridge_port=ensure_bl_usb_mapping(helper)
        address=ap_address()
        if not address:raise RuntimeError('IPv6 AP link-local not ready')
        link=serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=2)
        link.dtr=False;link.rts=False
        if args.without_ble_control:
            link.write(b'CONTROL\n');link.flush()
            marker(link,'V6_COEX:CONTROL_WITHOUT_BLE')
        elif not args.ble_first_start:
            start_ble(link)
        time.sleep(1)
        link.write(b'SNAP\n');link.flush()
        reports_before=int(marker(link,'V6_COEX:BLE_REPORTS='))
        if not args.without_ble_control and reports_before<=0:raise RuntimeError('No live BLE reports before IPv6')
        before=capture_ipv6()
        print('V6_COEX_IPV6_BEFORE='+json.dumps(before,sort_keys=True),flush=True)
        def exchange():
            try:peer.exchange(address)
            except Exception as error:errors.append(error)
        thread=threading.Thread(target=exchange)
        thread.start()
        sequence=overlap=ble_overlap=0
        deadline=time.monotonic()+120
        while thread.is_alive():
            if time.monotonic()>deadline:raise RuntimeError('IPv6 coexistence peer deadline')
            sequence+=1
            pre=capture_ipv6()
            link.write(b'SNAP\n');link.flush()
            ble_pre=int(marker(link,'V6_COEX:BLE_REPORTS='))
            link.write(f'E {sequence}\n'.encode());link.flush()
            if int(marker(link,'V6_COEX:USB_ECHO='))!=sequence:
                raise RuntimeError('USB echo sequence mismatch')
            link.write(b'SNAP\n');link.flush()
            ble_post=int(marker(link,'V6_COEX:BLE_REPORTS='))
            post=capture_ipv6()
            progressed=(post['received']>pre['received'] and post['sent']>pre['sent'])
            overlap+=int(progressed)
            ble_overlap+=int(progressed and ble_post>ble_pre)
            print('V6_COEX_OVERLAP='+json.dumps(dict(sequence=sequence,progressed=progressed,
                before=pre,after=post,ble_before=ble_pre,ble_after=ble_post),sort_keys=True),flush=True)
            time.sleep(.05)
        thread.join()
        if errors:raise errors[0]
        after=capture_ipv6()
        for field,total in (('connections',3),('closed',3),('received',69696),('sent',69696)):
            if after[field]-before[field]!=total*args.cycles:raise RuntimeError('IPv6 diagnostic counter delta mismatch')
        if after['errors']!=before['errors'] or after['pending']:
            raise RuntimeError('IPv6 diagnostic errors or pending data')
        if overlap<1:raise RuntimeError('No USB echo bracketed by bidirectional IPv6 progress')
        if not args.without_ble_control and ble_overlap<1:raise RuntimeError('No BLE report and USB echo bracketed by bidirectional IPv6 progress')
        link.write(b'SNAP\n');link.flush()
        reports_after=int(marker(link,'V6_COEX:BLE_REPORTS='))
        if not args.without_ble_control and reports_after<=reports_before:raise RuntimeError('BLE reports did not advance across IPv6')
        print('V6_COEX_RESULT='+json.dumps(dict(cycles=args.cycles,usb_echoes=sequence,overlapping_echoes=overlap,
            ble_overlapping_echoes=ble_overlap,
            reports_before=reports_before,reports_after=reports_after,ipv6_after=after),sort_keys=True),flush=True)
        link.write(b'STOP\n');link.flush()
        marker(link,'V6_COEX:STOPPED')
        stopped=True
        wait_empty()
        print('V6_USB_CONTROL_DATA_PASS' if args.without_ble_control else 'V6_COEX_DATA_PASS',flush=True)
    except Exception as error:
        primary=error
        if modified:
            try:
                args.bridge_port=ensure_bl_usb_mapping(helper)
                if link is None:
                    link=serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=2)
                    link.dtr=False;link.rts=False
                print('V6_COEX_FAILURE_BEFORE_CLEANUP='+json.dumps(dict(ap=capture_ap(),
                    ipv6=capture_ipv6()),sort_keys=True),flush=True)
            except Exception as diagnostic_error:
                print('V6_COEX_FAILURE_SNAPSHOT_UNAVAILABLE='+type(diagnostic_error).__name__,flush=True)
    finally:
        if thread is not None and thread.is_alive():
            peer.process.terminate();thread.join(5)
        actions=[]
        if link is not None:
            def stop_scan():
                if not stopped:
                    link.write(b'STOP\n');link.flush()
                    marker(link,'V6_COEX:STOPPED')
            actions.extend((('V6_COEX_BLE_STOP',stop_scan),('V6_COEX_SERIAL_CLOSE',link.close)))
        if peer is not None:actions.append(('V6_COEX_PEER_CLOSE',peer.close))
        if prepared:
            if helper is not None:actions.append(('V6_COEX_USB_DETACH',helper.detach))
            actions.append(('V6_COEX_NETWORK_RESTORE',session.close))
            def attach():args.bridge_port=ensure_bl_usb_mapping(helper)
            actions.append(('V6_COEX_USB_RESTORE',attach))
        if modified:actions.append(('V6_COEX_SCAN_RESTORE',lambda:control.restore_scan(args)))
        if helper is not None:actions.append(('V6_COEX_HELPER_CLOSE',helper.close))
        cleanup_error=cleanup_actions(actions)
        if primary is None:primary=cleanup_error
    if primary is not None:raise primary
    print('V6_USB_CONTROL_GATE_PASS' if args.without_ble_control else 'V6_COEX_GATE_PASS',flush=True)


if __name__=='__main__':main()
