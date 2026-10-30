#!/usr/bin/env python3
"""Hold one native TCP RX owner full while another completes exact exchanges.

Uses the dedicated BL616CL board and a temporary RA4 AT proxy. Private STA
credentials stay in memory. The standard scan Sketch is restored on exit.
No application retries or receive-deadline relaxation are used.
"""
import json
import re
import subprocess
import tempfile
import time
from pathlib import Path

import serial

import run_bl616cl_hostname_ipv6_final_carrier as control
from inspect_bl616cl_tcp_rx_memory import capture
from run_ra4_wifis3_tcp_resource_gate import windows_wlan_address, wsl_unc_path
from uno_r4_usb_identity import resolve_paired_bl616cl_port

ROOT = Path(__file__).resolve().parents[1]
PORT = 18471
PEER = r'''
import argparse, os, socket, threading, time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--bind',required=True);p.add_argument('--work',required=True)
p.add_argument('--bad-count',type=int,choices=range(1,8),default=1)
p.add_argument('--healthy-sessions',type=int,choices=(1,2),default=1)
a=p.parse_args();work=Path(a.work);stop=work/'stop';lock=threading.Lock()
(work/'peer.pid').write_text(str(os.getpid()))
def emit(s):
 with lock:
  with (work/'events').open('a') as f:f.write(s+'\n');f.write('ISOLATION_PEER_TIME_MS='+str(int(time.monotonic()*1000))+'\n')
def receive(c,n):
 b=b''
 while len(b)<n:
  v=c.recv(n-len(b))
  if not v:raise RuntimeError('early close')
  b+=v
 return b
def worker(listener,bad,index):
 phase='accept';sent=0
 try:
  for session in range(1 if bad else a.healthy_sessions):
   c,remote=listener.accept()
   emit('ISOLATION_PEER_ACCEPT='+str(index)+',AP_SOURCE='+str(int(remote[0]=='192.168.4.1')))
   with c:
    phase='tiny_send' if bad else 'healthy_receive'
    c.settimeout(15);c.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
    if bad:
     # Native retained block counts prove saturation, not send-call counts.
     for i in range(64):
      if stop.exists() or (work/'release').exists():return
      c.sendall(b'a');sent+=1;time.sleep(.05)
     emit('ISOLATION_BAD_SENT=64')
     while not stop.exists() and not (work/'release').exists():time.sleep(.05)
    else:
     for i in range(8):
      expected=bytes([65+i])*64
      if receive(c,64)!=expected:raise RuntimeError('payload mismatch')
      c.sendall(expected);emit('ISOLATION_PEER_EXCHANGE='+str(session*8+i+1))
     if c.recv(1):raise RuntimeError('unexpected trailing payload')
 except Exception as e:
  if not stop.exists() and not (bad and (work/'release').exists()):
   detail=':'+str(e) if isinstance(e,RuntimeError) else ''
   emit('ISOLATION_PEER_FAIL='+type(e).__name__+':SLOT='+str(index)+':PHASE='+phase+
        ':SENT='+str(sent)+':ERRNO='+str(getattr(e,'errno',None))+
        ':WINERROR='+str(getattr(e,'winerror',None))+detail)
listeners=[];threads=[]
try:
 for i in range(a.bad_count+1):
  s=socket.socket()
  # On Windows SO_REUSEADDR permits another listener to steal connections.
  # Fail preparation on an occupied port rather than run an ambiguous gate.
  if hasattr(socket,'SO_EXCLUSIVEADDRUSE'):
   s.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
  else:s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
  s.bind((a.bind,18471+i));s.listen(1);s.settimeout(180);listeners.append(s)
 for i,s in enumerate(listeners):
  t=threading.Thread(target=worker,args=(s,i<a.bad_count,i),daemon=True);t.start();threads.append(t)
 emit('ISOLATION_PEER_READY')
 while not stop.exists():
  if (work/'address.request').exists() and not (work/'address').exists():
   with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
    probe.connect(('192.168.4.1',9))
    address=probe.getsockname()[0]
   import ipaddress
   ip=ipaddress.IPv4Address(address)
   if ip in ipaddress.IPv4Network('192.168.4.0/24') and str(ip)!='192.168.4.1':
    (work/'address.tmp').write_text(address)
    (work/'address.tmp').replace(work/'address')
  time.sleep(.05)
finally:
 for s in listeners:s.close()
 for t in threads:t.join(timeout=2)
'''


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    args = control.parse_arguments()
    require(not args.source_only, 'use the existing proxy source-only gate to compile')
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    control.require_final_carrier_bridge(args.bridge_port)
    ssid, password = control.load_private_credentials(args.credential_env)
    require(password is not None, 'private STA password required')
    address = windows_wlan_address()
    print('ISOLATION_TARGET=dedicated BL616CL; temporary RA4M1 proxy; scan restored', flush=True)
    print('ISOLATION_INITIAL_OWNER=' + json.dumps(capture(), sort_keys=True), flush=True)
    capture(True)  # Require the explicitly enabled physical policy before upload.
    peer = link = None
    modified = False
    work = Path(tempfile.mkdtemp(prefix='tcp-rx-isolation-', dir=ROOT / 'build'))
    try:
        image = control.compile_proxy(args)
        (work / 'peer.py').write_text(PEER)
        with (work / 'peer.log').open('w') as output:
            peer = subprocess.Popen([
                'powershell.exe', '-NoProfile', '-Command',
                f'& py.exe -3 "{wsl_unc_path(work / "peer.py")}" --bind {address} '
                f'--work "{wsl_unc_path(work)}"'], stdout=output, stderr=subprocess.STDOUT)
        def events():
            return (work / 'events').read_text() if (work / 'events').exists() else ''
        deadline = time.monotonic() + 20
        while 'ISOLATION_PEER_READY' not in events():
            require(time.monotonic() < deadline and peer.poll() is None, 'peer startup failed')
            time.sleep(.1)
        modified = True
        control.upload_proxy(args, image)
        link = serial.Serial(args.bridge_port, 115200, timeout=.1, write_timeout=2)
        link.dtr = False
        link.rts = False
        time.sleep(2)
        def query(command):
            return control.transact(link, command, 10)
        require(query(b'AT\r\n') == control.TERMINAL, 'AT preflight failed')
        require(query(b'AT+WIFIMODE=1\r\n') == b'+WIFIMODE: \r\nOK\r\n', 'STA mode failed')
        require(re.fullmatch(rb'\+BEGINSTA: [0-9]+\r\nOK\r\n',
                            query(control.begin_sta_request(ssid, password))), 'STA request failed')
        control.wait_for_status(link, 3, 30, 10)
        slots = []
        for port in (PORT, PORT+1):
            match = re.fullmatch(rb'\+BEGINCLIENT: ([0-7])\r\nOK\r\n', query(b'AT+BEGINCLIENT\r\n'))
            require(match, 'client allocation failed')
            slot = int(match.group(1)); slots.append(slot)
            reply = query(f'AT+CLIENTCONNECT={slot},{address},{port},5000\r\n'.encode())
            require(reply == b'+CLIENTCONNECT: \r\nOK\r\n', 'client connect failed')
        initial = capture()
        deadline = time.monotonic()+10
        while True:
            physical = capture(True)
            owner = capture()
            if physical['max_owner_blocks'] == 16 and owner['denied'] > initial['denied']:
                break
            require(time.monotonic() < deadline, 'native tiny segments did not fill owner and trigger denial')
            time.sleep(.1)
        print('ISOLATION_SATURATED=' + json.dumps(physical, sort_keys=True), flush=True)
        for n in range(8):
            payload = bytes([65+n])*64
            reply = query(f'AT+CLIENTSEND={slots[1]},64\r\n'.encode()+payload)
            require(reply == control.TERMINAL, 'healthy send failed')
            deadline = time.monotonic()+10
            received = b''
            while len(received)<64:
                require(time.monotonic()<deadline, 'healthy receive exceeded 10 seconds')
                reply = query(f'AT+AVAILABLE={slots[1]}\r\n'.encode())
                match = re.fullmatch(rb'\+AVAILABLE: ([0-9]+)\r\nOK\r\n',reply)
                require(match, 'available framing failed')
                if not int(match.group(1)):
                    time.sleep(.01);continue
                reply = query(f'AT+CLIENTRECEIVE={slots[1]},{64-len(received)}\r\n'.encode())
                match = re.fullmatch(rb'\+CLIENTRECEIVE: ([0-9]+)\|(.*)OK\r\n',reply,re.S)
                require(match and int(match.group(1))==len(match.group(2)), 'receive framing failed')
                received += match.group(2)
            require(time.monotonic()<deadline, 'healthy receive exceeded 10 seconds')
            require(received==payload, 'healthy payload mismatch')
            require(capture(True)['max_owner_blocks']==16, 'non-consumer no longer saturated')
            print(f'ISOLATION_EXCHANGE_PASS={n+1}', flush=True)
        owner = capture();require(not owner['faults'] and not owner['type_rejected'], 'ownership fault')
        print('ISOLATION_FINAL_OWNER=' + json.dumps(owner, sort_keys=True), flush=True)
        require('ISOLATION_PEER_FAIL' not in events(), 'peer failed')
        print('ISOLATION_RESULT=PASS', flush=True)
    finally:
        try:
            if link is not None:
                try:
                    for slot in locals().get('slots', []):
                        control.transact(link, f'AT+CLIENTCLOSE={slot}\r\n'.encode(), 10)
                    control.transact(link, b'AT+DISCONNECT\r\n', 10)
                finally:
                    link.close()
        finally:
            try:
                (work/'stop').touch()
                if peer is not None:
                    try:
                        peer.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        # PID is supplied by this uniquely named owned peer.
                        pid = (work/'peer.pid').read_text().strip()
                        require(pid.isdigit() and int(pid)>4, 'invalid owned peer PID')
                        subprocess.run(['taskkill.exe','/PID',pid,'/T','/F'],
                                       check=True, timeout=10, capture_output=True)
                        peer.wait(timeout=5)
            finally:
                print('ISOLATION_WORK=' + str(work), flush=True)
                if modified:
                    control.restore_scan(args)



if __name__ == '__main__':
    main()
