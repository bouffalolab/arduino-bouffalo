"""Windows peer for 16 KiB STA UDP and verified TLS on the original UNO AP.

All Windows/root processes are prepared before changing WLAN. The generated
profile and network are restored; actual addresses are temporary in-memory data.
"""
import ipaddress
import json
import re
from pathlib import Path
import subprocess
import time
import socket
import threading

from bl616cl_prepared_usb_session import PreparedUsbSession
from bl616cl_softap_test_session import SoftApTestSession
from bl616cl_usb_mapping import ensure_bl_usb_mapping
from run_ra4_wifis3_tls_gate import create_material, wsl_unc_path

PEER = r'''
import json, socket, ssl, sys, threading, time
from pathlib import Path
work=Path(sys.argv[1])
def emit(text):
 with (work/'events').open('a') as f:f.write(text+'\n')
def wait(path,seconds=180):
 deadline=time.monotonic()+seconds
 while not path.exists():
  if (work/'stop').exists() or time.monotonic()>deadline:raise RuntimeError('peer wait deadline')
  time.sleep(.05)
try:
 emit('HOST_PEER_PREPARED')
 wait(work/'target')
 target=json.loads((work/'target').read_text())
 udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.settimeout(30)
 udp.connect((target['sta'],18462))
 (work/'address.tmp').write_text(udp.getsockname()[0])
 (work/'address.tmp').replace(work/'address')
 wait(work/'go')
 context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
 context.minimum_version=context.maximum_version=ssl.TLSVersion.TLSv1_2
 context.set_ciphers('ECDHE-ECDSA-AES128-GCM-SHA256')
 context.load_cert_chain(str(work/'server.pem'),str(work/'server.key'))
 listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
 listener.bind(('0.0.0.0',18452));listener.listen(2);listener.settimeout(.5)
 emit('HOST_SERVICES_READY')
 def datagrams():
  try:
   if target.get('ap_fragment_control'):
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as control:
     control.settimeout(20)
     wait(work/'ap-fragment-go')
     data=bytes((i*19+11)%251 for i in range(1460))
     control.sendto(data,(target['gateway'],18464))
     emit('HOST_AP_FRAGMENT_CONTROL_SENT bytes=1460 fragmented=False')
     try:
      reply,source=control.recvfrom(20000)
      if reply!=data[:32]:raise RuntimeError('AP control prefix echo mismatch')
      emit('HOST_AP_FRAGMENT_CONTROL_DELIVERED')
     except socket.timeout:emit('HOST_AP_FRAGMENT_CONTROL_TIMEOUT')
     emit('HOST_AP_FRAGMENT_CONTROL_DONE')
   for sequence,size in enumerate(target.get('udp_sizes',(64,1024,4096,16384,16384,16384))):
    data=bytes((i*17+sequence*29)%251 for i in range(size))
    wait(work/('udp-go-'+str(sequence+1)))
    for attempt in range(1,target.get('attempts',1)+1):
     udp.send(data)
     emit('HOST_UDP_SENT='+str(sequence+1)+',attempt='+str(attempt)+',bytes='+str(size))
     try:reply=udp.recv(20000);break
     except socket.timeout:
      emit('HOST_UDP_TIMEOUT='+str(sequence+1)+',attempt='+str(attempt))
      if attempt==target.get('attempts',1):raise
    if reply!=data:raise RuntimeError('UDP mismatch')
    emit('HOST_UDP_PASS='+str(sequence+1)+',bytes='+str(size))
   udp.close()
  except Exception as error:emit('HOST_UDP_FAIL='+type(error).__name__)
 thread=None
 if target.get('udp_peer','windows')=='windows':
  thread=threading.Thread(target=datagrams,daemon=True);thread.start()
 with listener:
  deadline=time.monotonic()+120
  while True:
   if (work/'stop').exists() or time.monotonic()>deadline:raise RuntimeError('TLS accept deadline')
   try:connection,_=listener.accept();break
   except socket.timeout:pass
  connection.settimeout(30)
  with context.wrap_socket(connection,server_side=True) as secure:
   data=b''
   while len(data)<1024:
    chunk=secure.recv(1024-len(data))
    if not chunk:raise RuntimeError('short TLS')
    data+=chunk
   expected=bytes((i*13+5)%251 for i in range(1024))
   if data!=expected:raise RuntimeError('TLS mismatch')
   secure.sendall(data);emit('HOST_TLS_VERIFIED_ECHO_PASS')
   # Allow the bridge to read buffered data before shutdown.
   wait(work/'tls-read',30)
 if thread is not None:
  thread.join(100+10*len(target.get('udp_sizes',())))
  if thread.is_alive():raise RuntimeError('UDP thread deadline')
 else:wait(work/'udp-complete',180)
 if 'HOST_UDP_FAIL=' in (work/'events').read_text():raise RuntimeError('UDP exchange failed')
 emit('HOST_SERVICES_COMPLETE')
except Exception as error:
 emit('HOST_SERVICES_FAIL='+type(error).__name__+','+str(getattr(error,'reason','')))
 sys.exit(1)
'''


def run_services(args, sta, ssid, password, gateway, work):
    import run_bl616cl_controlled_ap_recovery as gate
    import serial
    work = Path(work).resolve(); work.mkdir(mode=0o700, parents=True, exist_ok=False)
    large_count = getattr(args,'udp_large_count',3)
    sizes = (64,1024,1460)+(4096,)*large_count if args.host_udp_profile == 'basic' else (64,1024,4096)+(16384,)*large_count
    print('HOST_UDP_PROFILE=' + args.host_udp_profile, flush=True)
    session = SoftApTestSession(work)
    usb = process = output = None
    changed = False; primary = None; sock = tls = None; link = sta
    udp_native = udp_thread = None
    control_peer = control_sock = None
    def events():
        path = work/'events'
        return path.read_text() if path.exists() else ''
    def wait(marker, seconds=30):
        deadline = time.monotonic()+seconds
        while marker not in events():
            if 'FAIL' in events() or time.monotonic()>deadline or process.poll() is not None:
                raise RuntimeError('host services marker missing: '+marker+' events='+events().replace('\n',','))
            time.sleep(.1)
    def snapshot(label):
        import os
        import inspect_bl616cl_gpio35 as hid
        fd=os.open(hid.find_hidraw(),os.O_RDWR)
        report={}
        if getattr(args,'ap_tx_diagnostic',False) and control_peer is not None:
            data=gate.command(control_peer,'UDPNATIVETXREPORT')
            match=re.fullmatch(rb'\+UDPNATIVETXREPORT: ([0-9,]+)\r\nOK\r\n',data)
            if not match:raise RuntimeError('AP TX diagnostic framing')
            fields=list(map(int,match[1].split(b',')))
            names=('callbacks','total','id','count','success_mask','failure_mask','format','unrecognized','last_failure_offset')
            if len(fields)!=len(names):raise RuntimeError('AP TX diagnostic fields')
            report['ap_tx']=dict(zip(names,fields))
        try:
            for page in (30,48,49,50,51,81,82):
                hid.select_feature_page(fd,page);data=hid.read_feature(fd)
                deadline=time.monotonic()+1
                while page==30 and data[5]!=page and time.monotonic()<deadline:
                    time.sleep(.01);data=hid.read_feature(fd)
                if page==82 and data[5]!=page:
                    report['mac_fragment']={'available':False};continue
                if data[:4]!=b'U35D' or data[5]!=page:raise RuntimeError('HID diagnostic page mismatch')
                if page==82:
                    report['mac_fragment']={'total':int.from_bytes(data[8:12],'little'),'id':int.from_bytes(data[12:16],'little'),'count':int.from_bytes(data[16:20],'little'),'mask':int.from_bytes(data[20:24],'little'),'sequence':int.from_bytes(data[24:28],'little'),'offsets':[int.from_bytes(data[i:i+2],'little') for i in range(28,52,2)]}
                elif page==81:
                    report['fragment']={'totals':[int.from_bytes(data[i:i+4],'little') for i in (8,12,16)],'id':int.from_bytes(data[20:22],'little'),'count':int.from_bytes(data[22:24],'little'),'offsets':[int.from_bytes(data[i:i+2],'little') for i in range(24,48,2)]}
                elif page==30:
                    report['udp']=[int.from_bytes(data[i:i+2],'little') for i in range(22,50,2)]
                else:
                    count={48:14,49:8,50:5,51:3}[page]
                    report[str(page)]=[int.from_bytes(data[i:i+4],'little') for i in range(8,8+4*count,4)]
        finally:
            hid.select_feature_page(fd,0);os.close(fd)
        print('HOST_SERVICES_RX_SNAPSHOT='+json.dumps({'label':label,'counts':report}),flush=True)
    try:
        # The owning runner already starts this AP with four client slots.
        # Check it without reissuing BEGINAP while the BL remains associated.
        import run_esp32_sta_baseline_gate as baseline
        from uno_r4_usb_identity import resolve_baseline_esp32_port
        peer_port = resolve_baseline_esp32_port('auto')
        baseline.require_port_identity(peer_port)
        with baseline.open_serial(peer_port,.1) as peer:
            if gate.bl.read_status(peer,4) not in (7,8):raise RuntimeError('upstream AP is not active')
        gate.wait_value(lambda:gate.bl.read_status(link,4),lambda value:value==3,60,'HOST_FIXTURE_STA_READY')
        usb = PreparedUsbSession()
        (work/'peer.py').write_text(PEER)
        output = (work/'peer.log').open('w')
        process = subprocess.Popen(['powershell.exe','-NoProfile','-Command',
            f'& py.exe -3 "{wsl_unc_path(work/"peer.py")}" "{wsl_unc_path(work)}"'],stdout=output,stderr=subprocess.STDOUT)
        wait('HOST_PEER_PREPARED')
        sta_address = gate.address(link,'IPSTA',0)
        link.close(); usb.detach(); changed=True
        session.prepare_association(args,ssid,password,gateway)
        session.connect_prepared()
        args.bridge_port=ensure_bl_usb_mapping(usb)
        link=serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=3)
        link.dtr=False;link.rts=False
        sock=gate.number(link,'UDPBEGIN',f'18462,{sta_address}')
        control_enabled = getattr(args,'ap_fragment_control',False)
        if args.udp_peer=='ap-native' or getattr(args,'ap_tx_diagnostic',False):
            control_peer=baseline.open_serial(resolve_baseline_esp32_port('auto'),.1)
        if control_enabled:
            if args.udp_peer != 'windows':raise RuntimeError('AP fragment control requires Windows peer')
            if control_peer is None:
                control_peer=baseline.open_serial(resolve_baseline_esp32_port('auto'),.1)
            control_sock=gate.number(control_peer,'UDPBEGIN',18464)
        (work/'target.tmp').write_text(json.dumps({'sta':sta_address,'attempts':args.udp_attempts,'udp_peer':args.udp_peer,'udp_sizes':sizes,'ap_fragment_control':control_enabled,'gateway':gateway}))
        (work/'target.tmp').replace(work/'target')
        deadline=time.monotonic()+15
        while not (work/'address').exists() or not (work/'address').read_text().strip():
            if time.monotonic()>deadline:raise RuntimeError('host address deadline')
            time.sleep(.1)
        host=ipaddress.IPv4Address((work/'address').read_text())
        if host not in ipaddress.IPv4Network(gateway+'/24',strict=False) or str(host)==gateway:
            raise RuntimeError('host outside upstream AP subnet')
        if args.udp_peer=='wsl':
            udp_native=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
            udp_native.settimeout(.5);udp_native.connect((sta_address,18462))
            def native_datagrams():
                def emit(line):
                    with (work/'events').open('a') as file:file.write(line+'\n')
                try:
                    emit('HOST_UDP_PEER=WSL')
                    for sequence,size in enumerate(sizes):
                        ready=work/('udp-go-'+str(sequence+1))
                        deadline=time.monotonic()+180
                        while not ready.exists():
                            if (work/'stop').exists() or time.monotonic()>deadline:raise RuntimeError('UDP ready deadline')
                            time.sleep(.05)
                        data=bytes((i*17+sequence*29)%251 for i in range(size))
                        udp_native.send(data);emit('HOST_UDP_SENT='+str(sequence+1)+',attempt=1,bytes='+str(size))
                        deadline=time.monotonic()+30
                        while True:
                            try:reply=udp_native.recv(20000);break
                            except socket.timeout:
                                if time.monotonic()>deadline or (work/'stop').exists():raise
                        if reply!=data:raise RuntimeError('UDP mismatch')
                        emit('HOST_UDP_PASS='+str(sequence+1)+',bytes='+str(size))
                except Exception as error:emit('HOST_UDP_FAIL='+type(error).__name__)
            udp_thread=threading.Thread(target=native_datagrams,daemon=True);udp_thread.start()
        ca,_,_=create_material(work,'ec',host)
        (work/'go').touch();wait('HOST_SERVICES_READY')
        tls=gate.number(link,'SSLBEGINCLIENT')
        pem=ca.read_bytes()+b'\0'
        gate.command(link,'SETCAROOT',f'{tls},{len(pem)}',pem)
        gate.command(link,'SSLCLIENTCONNECTIP',f'{tls},{host},18452',timeout=30)
        payload=bytes((i*13+5)%251 for i in range(1024))
        gate.command(link,'SSLCLIENTSEND',f'{tls},1024',payload)
        gate.wait_value(lambda:gate.number(link,'SSLAVAILABLE',tls),lambda n:n>0,30,'RECOVERED_STA_TLS_RX')
        gate.receive(link,'SSLCLIENTRECEIVE',tls,payload)
        (work/'tls-read').touch()
        wait('HOST_TLS_VERIFIED_ECHO_PASS')
        print('RECOVERED_STA_VERIFIED_TLS_BIDIRECTIONAL_PASS bytes=1024',flush=True)
        if control_enabled:
            expected=bytes((i*19+11)%251 for i in range(1460))
            (work/'ap-fragment-go').touch()
            deadline=time.monotonic()+18
            delivered=False
            while time.monotonic()<deadline:
                amount=gate.number(control_peer,'UDPPARSE',control_sock)
                if amount:
                    if amount!=1460:raise RuntimeError('AP original UDP truncate boundary mismatch')
                    gate.receive(control_peer,'UDPREAD',control_sock,expected[:1460])
                    port=gate.number(control_peer,'UDPREMOTEPORT',control_sock)
                    remote=gate.address(control_peer,'UDPREMOTEIP',control_sock)
                    gate.command(control_peer,'UDPBEGINPACKETIP',f'{control_sock},{port},{remote}')
                    gate.command(control_peer,'UDPWRITE',f'{control_sock},32',expected[:32])
                    gate.command(control_peer,'UDPENDPACKET',control_sock)
                    delivered=True;break
                time.sleep(.1)
            wait('HOST_AP_FRAGMENT_CONTROL_DONE',25)
            print('AP_FRAGMENT_CONTROL_RESULT='+str(delivered)+' bytes=1460 fragmented=False',flush=True)
        for sequence,size in enumerate(sizes):
            expected=bytes((i*17+sequence*29)%251 for i in range(size))
            snapshot('before-'+str(size))
            if args.udp_peer=='ap-native':
                if gate.number(control_peer,'UDPNATIVESEND',f'{sta_address},18462,{size},{sequence}')!=size:raise RuntimeError('native AP UDP send failed')
                print('NATIVE_AP_UDP_SENT bytes='+str(size),flush=True)
            else:(work/('udp-go-'+str(sequence+1))).touch()
            gate.wait_value(lambda:gate.number(link,'UDPPARSE',sock),lambda n:n==size,30*args.udp_attempts+5,'STA_UDP_RX_'+str(size))
            snapshot('received-'+str(size))
            # Read a datagram through bounded AT chunks; length/hash checked in memory.
            for offset in range(0,size,512):
                part=expected[offset:offset+512]
                gate.receive(link,'UDPREAD',sock,part)
            port=gate.number(link,'UDPREMOTEPORT',sock)
            gate.command(link,'UDPBEGINPACKETIP',f"{sock},{port},{gate.address(link,'UDPREMOTEIP',sock)}")
            echo_size=32 if args.udp_peer=='ap-native' else size
            for offset in range(0,echo_size,512):
                part=expected[offset:min(offset+512,echo_size)]
                gate.command(link,'UDPWRITE',f'{sock},{len(part)}',part)
            gate.command(link,'UDPENDPACKET',sock)
            if args.udp_peer=='ap-native':
                gate.wait_value(lambda:gate.number(control_peer,'UDPNATIVEACK'),lambda n:n==32,10,'NATIVE_AP_ACK')
                print('NATIVE_AP_STA_UDP_RECEIVE_PASS bytes='+str(size)+' ack_bytes=32',flush=True)
                continue
            wait('HOST_UDP_PASS='+str(sequence+1)+',bytes='+str(size))
            print('STA_UDP_BIDIRECTIONAL_PASS='+str(sequence+1)+',bytes='+str(size),flush=True)
        (work/'udp-complete').touch()
        wait('HOST_SERVICES_COMPLETE')
    except Exception as error:
        print('HOST_SERVICES_GATE_FAILURE='+str(error),flush=True);primary=error
        if tls is not None and link is not None and link.is_open:
            try:print('HOST_SERVICES_TLS_DIAG='+gate.command(link,'SSLCLIENTDIAG',tls).decode('ascii').replace('\r\n',','),flush=True)
            except Exception:print('HOST_SERVICES_TLS_DIAG_FAILED',flush=True)
        try:snapshot('failure')
        except Exception:print('HOST_SERVICES_RX_SNAPSHOT_FAILED',flush=True)
        if sock is not None and link is not None and link.is_open:
            try:
                reply=gate.command(link,'UDPDEBUG',sock)
                fields=reply.split(b': ',1)[1].split(b'\r\n',1)[0].split(b',')
                for index in range(3,min(7,len(fields))):fields[index]=b'<address>'
                print('HOST_SERVICES_UDPDEBUG='+b','.join(fields).decode('ascii'),flush=True)
            except Exception:print('HOST_SERVICES_UDPDEBUG_FAILED',flush=True)
    finally:
        if control_peer is not None:
            try:
                if control_sock is not None:gate.command(control_peer,'UDPSTOP',control_sock)
            except Exception as error:
                if primary is None:primary=error
            finally:control_peer.close()
        for name,s in (('SSLCLIENTCLOSE',tls),('UDPSTOP',sock)):
            if s is not None and link is not None:
                try:gate.command(link,name,s)
                except Exception as error:
                    if primary is None:primary=error
        if link is not None:link.close()
        (work/'stop').touch()
        if udp_native is not None:udp_native.close()
        if udp_thread is not None:udp_thread.join(2)
        if process is not None:
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=5)
        if output is not None:output.close()
        if changed:
            try:usb.detach()
            except Exception as error:
                if primary is None:primary=error
        try:session.close()
        except Exception as error:
            if primary is None:primary=error
        if changed:
            try:args.bridge_port=ensure_bl_usb_mapping(usb)
            except Exception as error:
                if primary is None:primary=error
        if usb is not None:usb.close()
        for name in ('address','target','server.key'):(work/name).unlink(missing_ok=True)
    if primary is not None:raise primary
    restored=serial.Serial(args.bridge_port,115200,timeout=.1,write_timeout=3)
    restored.dtr=False;restored.rts=False
    return restored
