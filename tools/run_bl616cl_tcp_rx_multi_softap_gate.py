#!/usr/bin/env python3
"""Multiple unread native RX owners, healthy progress and bounded close/reopen.

Dedicated BL616CL; temporary RA4 proxy and generated SoftAP profile. All Windows
and USB helpers start before the WLAN switch. No application traffic retries.
"""
import argparse
import ipaddress
import json
from pathlib import Path
import re
import secrets
import subprocess
import time

import serial
from bl616cl_prepared_usb_session import PreparedUsbSession
from bl616cl_softap_test_session import SoftApTestSession
from bl616cl_usb_mapping import ensure_bl_usb_mapping
from inspect_bl616cl_tcp_rx_memory import capture
from inspect_bl616cl_ap_lifecycle import capture as capture_ap, capture_ra4
from run_bl616cl_ipv6_tcp_softap_gate import cleanup_actions
from run_bl616cl_tcp_rx_isolation import PEER, PORT, require
from run_ra4_wifis3_tcp_resource_gate import wsl_unc_path
import run_bl616cl_hostname_ipv6_final_carrier as control

ROOT = Path(__file__).resolve().parents[1]


def wait_ap_ready(previous_attempts, timeout=10):
    """AT acceptance precedes native AP completion; gate host discovery on it.

    This is preparation only. No scan/reconnect or application traffic retry.
    """
    deadline=time.monotonic()+timeout
    while True:
        state=capture_ap()
        require(time.monotonic()<=deadline,'AP readiness observation exceeded 10 seconds')
        start=state['2']
        if start['attempts']>previous_attempts and start['stage']==3:
            require(start['native_result']==0 and start['ap_started']==1,
                    'native AP start failed')
            records=state['26']['records']
            require(records and records[-1]['enabled']==1,'native AP state is not enabled')
            print('MULTI_OWNER_NATIVE_AP_READY='+json.dumps(state,sort_keys=True),flush=True)
            return
        require(time.monotonic()<deadline,'native AP readiness exceeded 10 seconds')
        time.sleep(.05)


def expected_blocks(bad_count):
    # Each occupied attacker releases one protected block; all 32 owner slots
    # retain a base. This is a metadata bound, independent of payload bytes.
    return min(16 * bad_count, 128 - (32 - bad_count))


def assert_held(physical, bad_count):
    require(physical['blocks'] == expected_blocks(bad_count),
            'unread owners lost their retained blocks')
    require(physical['max_owner_blocks'] == 16, 'no owner at block quota')
    require(physical['blocks'] + physical['blocks_reserved'] <= 128,
            'future owner block reserve exceeded')
    # At 2/4 attackers, aggregate=N*16 and every owner's cap=16 proves that
    # each attacker is full. At 7, only aggregate global saturation is proven.


def wait_empty(timeout=10):
    start = time.monotonic(); deadline = start + timeout
    while True:
        owner = capture()
        require(time.monotonic() <= deadline, 'owner observation exceeded close deadline')
        require(not owner['type_rejected'], 'unexpected native allocation type')
        if not any(owner[k] for k in ('sequence_used', 'physical_used', 'blocks',
                                      'live_flows', 'retired_flows')):
            physical = capture(True)
            require(physical['physical_reserved'] == 8192 and
                    physical['blocks_reserved'] == 32, 'final reserve not restored')
            require(time.monotonic() <= deadline, 'reserve observation exceeded close deadline')
            print('MULTI_OWNER_RELEASE_MS=' + str(int((time.monotonic()-start)*1000)), flush=True)
            print('MULTI_OWNER_EMPTY=' + json.dumps(owner, sort_keys=True), flush=True)
            return
        require(time.monotonic() < deadline, 'close did not release all owners within 10 seconds')
        time.sleep(.05)


def exchange(link, slot, count, held=None):
    for n in range(8):
        payload = bytes([65+n])*64
        require(control.transact(link, f'AT+CLIENTSEND={slot},64\r\n'.encode()+payload, 10)
                == control.TERMINAL, 'healthy send failed')
        start = time.monotonic(); deadline = start+10
        received = b''
        while len(received) < 64:
            remaining = deadline-time.monotonic()
            require(remaining > 0, 'healthy receive exceeded 10 seconds')
            reply = control.transact(link, f'AT+AVAILABLE={slot}\r\n'.encode(), remaining)
            match = re.fullmatch(rb'\+AVAILABLE: ([0-9]+)\r\nOK\r\n', reply)
            require(match, 'available framing failed')
            if not int(match.group(1)):
                time.sleep(.01); continue
            remaining = deadline-time.monotonic()
            require(remaining > 0, 'healthy receive exceeded 10 seconds')
            reply = control.transact(link, f'AT+CLIENTRECEIVE={slot},{64-len(received)}\r\n'.encode(), remaining)
            match = re.fullmatch(rb'\+CLIENTRECEIVE: ([0-9]+)\|(.*)OK\r\n', reply, re.S)
            require(match and int(match.group(1)) == len(match.group(2)), 'receive framing failed')
            received += match.group(2)
        require(time.monotonic() < deadline and received == payload, 'healthy data/deadline failed')
        if held is not None:
            # Physical callbacks finish before the next HID sample; allow no
            # relaxation of the actual receive deadline.
            assert_held(capture(True), held)
        print(f'MULTI_OWNER_EXCHANGE={count+n+1},RX_MS={int((time.monotonic()-start)*1000)}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--bad-count', type=int, choices=(2,4,7), required=True)
    parser.add_argument('--arduino-cli', default='arduino-cli')
    parser.add_argument('--runtime-settle-ms', type=int, default=1000)
    args = parser.parse_args(); args.work_dir.mkdir(parents=True, exist_ok=False)
    code = "import sys,os;sys.path.insert(0,"+repr(str(ROOT/'tools'))+");from bl616cl_usb_mapping import ensure_bl_usb_mapping;p=ensure_bl_usb_mapping();f=os.open(p,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK);os.close(f);print('MULTI_OWNER_BL_IDENTITY_OPEN')"
    subprocess.run(['wsl.exe','-d','Ubuntu-22.04','-u','root','--','python3','-c',code],check=True,timeout=45)
    args.bridge_port = ensure_bl_usb_mapping()
    control.require_final_carrier_bridge(args.bridge_port)
    print('MULTI_OWNER_TARGET=dedicated BL616CL current image; temporary RA4M1 AT proxy; scan restored', flush=True)
    capture(True); wait_empty()
    image = control.compile_proxy(args)
    session = SoftApTestSession(args.work_dir)
    peer = first_attach = back_attach = first_detach = back_detach = link = None
    slots = []; modified = prepared = False; primary = None
    work = args.work_dir
    detached=False
    def snapshot(label):
        state=dict(label=label,ap=capture_ap(),owner=capture())
        if label=='FAILURE_BEFORE_CLEANUP':state['ra4']=capture_ra4()
        print('MULTI_OWNER_LIFECYCLE='+json.dumps(state,sort_keys=True),flush=True)
    def events():
        return (work/'events').read_text() if (work/'events').exists() else ''
    try:
        first_attach=PreparedUsbSession()
        back_attach=first_detach=back_detach=first_attach
        (work/'peer.py').write_text(PEER)
        with (work/'peer.log').open('w') as output:
            peer = subprocess.Popen(['powershell.exe','-NoProfile','-Command',
                f'& py.exe -3 "{wsl_unc_path(work/"peer.py")}" --bind 0.0.0.0 '
                f'--work "{wsl_unc_path(work)}" --bad-count {args.bad_count} --healthy-sessions 2'],
                stdout=output,stderr=subprocess.STDOUT)
        deadline = time.monotonic()+20
        while 'ISOLATION_PEER_READY' not in events():
            require(time.monotonic()<deadline and peer.poll() is None, 'Windows peer startup failed')
            time.sleep(.1)
        ssid='BL6-MULTI-'+secrets.token_hex(4); password=secrets.token_hex(8)
        prepared=True; session.prepare_association(args,ssid,password,'192.168.4.1')
        modified=True; control.upload_proxy(args,image)
        with serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=2) as setup:
            setup.dtr=False; setup.rts=False; time.sleep(1)
            control.require_exact(control.transact(setup,b'AT\r\n',10),control.TERMINAL,'proxy')
            control.require_exact(control.transact(setup,b'AT+WIFIMODE=2\r\n',10),b'+WIFIMODE: \r\nOK\r\n','AP mode')
            previous_attempts=capture_ap()['2']['attempts']
            control.require_exact(control.transact(setup,f'AT+BEGINSOFTAP={ssid},{password},6\r\n'.encode(),10),
                                  b'+BEGINSOFTAP: 1\r\nOK\r\n','AP start')
        wait_ap_ready(previous_attempts)
        snapshot('AP_START_BEFORE_USB_DETACH')
        first_detach.detach(); detached=True
        session.connect_prepared()
        args.bridge_port=ensure_bl_usb_mapping(first_attach); detached=False
        snapshot('HOST_ASSOCIATED')
        (work/'address.request').touch(); deadline=time.monotonic()+10
        while not (work/'address').exists():
            require(time.monotonic()<deadline and peer.poll() is None, 'prepared host DHCP address deadline')
            time.sleep(.05)
        address=str(ipaddress.IPv4Address((work/'address').read_text()))
        require(ipaddress.IPv4Address(address) in ipaddress.IPv4Network('192.168.4.0/24') and address!='192.168.4.1',
                'host is outside dedicated AP subnet')
        link=serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=2)
        link.dtr=False; link.rts=False
        def query(command):return control.transact(link,command,10)
        def connect(port):
            match=re.fullmatch(rb'\+BEGINCLIENT: ([0-7])\r\nOK\r\n',query(b'AT+BEGINCLIENT\r\n'))
            require(match,'client allocation failed'); slot=int(match.group(1)); slots.append(slot)
            require(query(f'AT+CLIENTCONNECT={slot},{address},{port},5000\r\n'.encode())==b'+CLIENTCONNECT: \r\nOK\r\n',
                    'client connect failed')
            return slot
        initial=capture()
        for i in range(args.bad_count):connect(PORT+i)
        deadline=time.monotonic()+10
        while True:
            physical=capture(True); owner=capture()
            require('ISOLATION_PEER_FAIL' not in events(),'Windows peer failed')
            if physical['blocks']==expected_blocks(args.bad_count) and owner['denied']>initial['denied']:break
            require(time.monotonic()<deadline,'multiple owners did not reach native block boundary')
            time.sleep(.1)
        assert_held(physical,args.bad_count)
        print('MULTI_OWNER_SATURATED='+json.dumps(physical,sort_keys=True),flush=True)
        healthy=connect(PORT+args.bad_count)
        exchange(link,healthy,0,held=args.bad_count)
        # After the last close reply, all charges must clear within 10s,
        # without reset or mode change. Then prove fresh admission.
        (work/'release').touch()
        for slot in list(slots):
            require(query(f'AT+CLIENTCLOSE={slot}\r\n'.encode())==b'+CLIENTCLOSE: \r\nOK\r\n','client close failed')
            slots.remove(slot)
        wait_empty()
        healthy=connect(PORT+args.bad_count); exchange(link,healthy,8)
        require(query(f'AT+CLIENTCLOSE={healthy}\r\n'.encode())==b'+CLIENTCLOSE: \r\nOK\r\n','reopened client close failed')
        slots.remove(healthy); wait_empty()
        require('ISOLATION_PEER_EXCHANGE=16' in events() and 'ISOLATION_PEER_FAIL' not in events(),'peer completion failed')
        print('MULTI_OWNER_DATA_PASS',flush=True)
    except Exception as error:
        primary=error
        # Capture the failed AP before cleanup changes mode or host profile.
        # Reattaching USB is diagnostic recovery, never a network retry.
        if modified:
            try:
                if detached:
                    args.bridge_port=ensure_bl_usb_mapping(first_attach); detached=False
                snapshot('FAILURE_BEFORE_CLEANUP')
            except Exception as diagnostic_error:
                print('MULTI_OWNER_FAILURE_SNAPSHOT_UNAVAILABLE='+type(diagnostic_error).__name__,flush=True)
    finally:
        actions=[]
        if link is not None:
            for slot in slots:
                actions.append(('MULTI_CLIENT_CLOSE',lambda slot=slot: control.transact(link,f'AT+CLIENTCLOSE={slot}\r\n'.encode(),10)))
            actions.append(('MULTI_SERIAL_CLOSE',link.close))
        def close_peer():
            (work/'stop').touch()
            if peer is not None:
                try:peer.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    pid=(work/'peer.pid').read_text().strip()
                    require(pid.isdigit() and int(pid)>4,'invalid owned peer PID')
                    subprocess.run(['taskkill.exe','/PID',pid,'/T','/F'],check=True,timeout=10,capture_output=True)
                    peer.wait(timeout=5)
        actions.append(('MULTI_PEER_CLEANUP',close_peer))
        if prepared:
            if back_detach is not None:actions.append(('MULTI_USB_DETACH',back_detach.detach))
            actions.append(('MULTI_NETWORK_RESTORE',session.close))
            def restore_mapping():args.bridge_port=ensure_bl_usb_mapping(back_attach)
            actions.append(('MULTI_USB_RESTORE',restore_mapping))
        if modified:
            def restore_mode():
                args.bridge_port=ensure_bl_usb_mapping()
                with serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=2) as port:
                    port.dtr=False;port.rts=False
                    control.require_exact(control.transact(port,b'AT+WIFIMODE=1\r\n',10),b'+WIFIMODE: \r\nOK\r\n','restore STA')
            actions.append(('MULTI_STA_RESTORE',restore_mode))
            actions.append(('MULTI_SCAN_RESTORE',lambda: control.restore_scan(args)))
        for helper in (first_attach,back_attach,first_detach,back_detach):
            if helper is not None:actions.append(('MULTI_HELPER_CLOSE',helper.close))
        cleanup_error=cleanup_actions(actions)
        if primary is None:primary=cleanup_error
    if primary is not None:raise primary
    print(f'MULTI_OWNER_GATE_PASS={args.bad_count}',flush=True)


if __name__=='__main__':main()
