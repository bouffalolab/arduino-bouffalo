#!/usr/bin/env python3
"""Isolate SAM-BA K# -> user UART startup. No radio or data-transfer retries."""
import argparse
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
from types import SimpleNamespace
from upload_ra4_samba_split import open_serial
from uno_r4_usb_identity import resolve_paired_bl616cl_port
from run_ra4_bossac_final_carrier import require_final_carrier_bridge, wait_uart_forwarded
import inspect_bl616cl_gpio35 as hid
from run_bl616cl_swd_d6_openocd import resolve_paths
ROOT = Path(__file__).resolve().parents[1]

def counters():
    fd=os.open(hid.find_hidraw(),os.O_RDWR)
    try:
        hid.select_feature_page(fd,0);r=hid.read_feature(fd)
        return {'out':int.from_bytes(r[32:36],'little'),'drop':int.from_bytes(r[36:40],'little'),
                'tx':int.from_bytes(r[40:44],'little'),'tx_fail':int.from_bytes(r[44:48],'little'),
                'rx':int.from_bytes(r[48:52],'little'),'overflow':int.from_bytes(r[52:56],'little'),
                'buffered':int.from_bytes(r[56:58],'little'),'flags':list(r[58:64])}
    finally: os.close(fd)

def swd_snapshot(work):
    exe,scripts,target=resolve_paths(SimpleNamespace(openocd=None,scripts_dir=None,target_config=None))
    result=subprocess.run([str(exe),'-s',str(scripts),'-f','interface/cmsis-dap.cfg',
        '-c','cmsis_dap_serial BL616CL-PROVISIONAL','-f',str(target),'-c','adapter speed 100',
        '-c','init','-c','halt','-c','reg','-c','mdw 0xe000ed28 6','-c','resume','-c','shutdown'],
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=20)
    (work/'failure-swd.log').write_text(result.stdout)
    print('STARTUP_FAILURE_SWD_EXIT='+str(result.returncode),flush=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--cycles',type=int,default=20)
    parser.add_argument('--confirmed-handoff',action='store_true',help='Wait for board UART forwarding before changing baud')
    parser.add_argument('--handoff-pause-ms',type=int,default=0,help='Diagnostic control only; default immediate baud change')
    args=parser.parse_args();work=args.work_dir.resolve();work.mkdir(exist_ok=False,parents=True)
    if not 1<=args.cycles<=100 or not 0<=args.handoff_pause_ms<=100:raise ValueError('invalid cycles/pause')
    port=resolve_paired_bl616cl_port('auto');require_final_carrier_bridge(port)
    print('TARGET=BL616CL-PROVISIONAL RA4M1 Sketch only; BL Flash unchanged',flush=True)
    primary=None
    try:
        subprocess.run(['/home/developer/.local/bin/arduino-cli','compile','--fqbn','arduino:renesas_uno:unor4wifi',
            '--build-path',str(work/'build'),str(ROOT/'examples/ra4-startup-handoff-smoke')],check=True)
        subprocess.run([sys.executable,str(ROOT/'tools/run_ra4_bossac_final_carrier.py'),'--bridge-port',port,
            '--attempts','1','--image',str(work/'build/ra4-startup-handoff-smoke.ino.bin')],check=True)
        for cycle in range(1,args.cycles+1):
            port=resolve_paired_bl616cl_port('auto');require_final_carrier_bridge(port)
            with open_serial(port,115200,.05) as link:
                link.baudrate=1200;time.sleep(.6)
                link.baudrate=230400;time.sleep(.1);link.reset_input_buffer()
                link.write(b'N#');link.flush();time.sleep(.05);link.read(128)
                link.write(b'V#');link.flush();version=bytearray();deadline=time.monotonic()+2
                while time.monotonic()<deadline and b'[Arduino:' not in version:version.extend(link.read(128))
                if b'Arduino Bootloader (SAM-BA' not in version:raise RuntimeError('bootloader precondition failed')
                before=counters()
                link.write(b'K#');link.flush()
                if args.confirmed_handoff:
                    wait_uart_forwarded((before['tx'],before['drop'],before['tx_fail']),2)
                if args.handoff_pause_ms:time.sleep(args.handoff_pause_ms/1000)
                link.baudrate=115200;link.reset_input_buffer()
                received=bytearray();deadline=time.monotonic()+3
                while time.monotonic()<deadline and b'RA4_STARTUP:READY' not in received:
                    received.extend(link.read(256))
                token=secrets.token_hex(8).encode()
                if b'RA4_STARTUP:READY' in received:
                    link.write(token+b'\n');link.flush()
                    while time.monotonic()<deadline and b'RA4_STARTUP:ECHO='+token not in received:
                        received.extend(link.read(256))
                after=counters()
                passed=b'RA4_STARTUP:ECHO='+token in received
                print('STARTUP_CYCLE='+json.dumps({'cycle':cycle,'passed':passed,'bytes':len(received),'before':before,'after':after}),flush=True)
                if not passed:
                    swd_snapshot(work)
                    raise RuntimeError('startup echo failed cycle='+str(cycle))
        print('RA4_STARTUP_HANDOFF_GATE_PASS cycles='+str(args.cycles),flush=True)
    except Exception as error:primary=error
    finally:
        subprocess.run([sys.executable,str(ROOT/'tools/run_ra4_wifis3_scan_final_carrier.py'),
            '--arduino-cli','/home/developer/.local/bin/arduino-cli'],check=True)
    if primary:raise primary
if __name__=='__main__':main()
