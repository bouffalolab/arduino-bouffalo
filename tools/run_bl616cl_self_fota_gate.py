#!/usr/bin/env python3
"""Exercise BL self-FOTA using a temporary RA4 proxy on the dedicated carrier.

Uploads only a supplied, postprocessed BFNP application, with a SHA256 tail.
Negative tests never commit their inactive slot. Restore a scan Sketch on exit.
This gate intentionally does not claim an automatic boot-health rollback.
"""
import argparse
import hashlib
import json
import subprocess
import tempfile
import time
from pathlib import Path
import run_ra4_ota_download_final_carrier as d
from prepared_bl616cl_reset import PreparedBlReset
from run_bl616cl_generic_at_final_carrier import restore_usb
from uno_r4_usb_identity import resolve_paired_bl616cl_port


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--application',type=Path,required=True)
    parser.add_argument('--expected-version',required=True)
    parser.add_argument('--initial-version',default='0.6.0')
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--skip-negative',action='store_true')
    mode.add_argument('--negative-only',action='store_true')
    parser.add_argument('--check-header',action='store_true')
    args=parser.parse_args()
    image=args.application.read_bytes()
    if not image.startswith(b'BFNP') or not 4096<=len(image)<=0x168000-32:
        raise ValueError('expected a BFNP application fitting both firmware slots')
    payload=image+hashlib.sha256(image).digest()
    print(f'SELF_FOTA_IMAGE bytes={len(image)} sha256={hashlib.sha256(image).hexdigest()}',flush=True)
    # Windows D2XX owns the reset lines; do not leave the same FTDI in WSL.
    state=json.loads(subprocess.check_output(['usbipd.exe','state'],text=True))
    ftdi=[dev for dev in state['Devices'] if dev.get('BusId') and
          'VID_0403&PID_6001' in dev.get('InstanceId','').upper()]
    if len(ftdi)!=1:
        raise RuntimeError('expected exactly one final-carrier FTDI')
    print('SELF_FOTA_FTDI_IDENTITY='+ftdi[0]['InstanceId'],flush=True)
    if ftdi[0].get('IsWslAttached'):
        subprocess.run(['usbipd.exe','wsl','detach','--busid',ftdi[0]['BusId']],check=True)
        restore_usb(physical_reset=True)
    opts=argparse.Namespace(bridge_port=resolve_paired_bl616cl_port('auto'),
        arduino_cli='arduino-cli',runtime_settle_ms=1000)
    d.final_scan.require_final_carrier_bridge(opts.bridge_port)
    link=None;prepared=None;installed=False
    current_version=args.initial_version
    work=Path(tempfile.mkdtemp(prefix='self-fota-',dir=d.ROOT_DIR/'build'))
    def exact(cmd,want,timeout=10):
        d.require_exact(d.transact(link,cmd,timeout),want,cmd.decode().strip())
    def version(want):
        nonlocal current_version
        # Existing WiFiS3 VERSION response is the image marker.
        result=d.transact(link,b'AT+FWVERSION?\r\n',5)
        print('SELF_FOTA_VERSION_RESPONSE='+repr(result),flush=True)
        if result!=f'+FWVERSION: {want}\r\nOK\r\n'.encode():
            raise RuntimeError('firmware image marker does not match '+want)
        current_version=want
    def begin(size):exact(f'AT+BLFOTABEGIN={size}\r\n'.encode(),b'+BLFOTABEGIN: 0\r\nOK\r\n',30)
    def chunk(data):
        response=d.transact_raw_payload(link,f'AT+BLFOTADATA={len(data)}\r\n'.encode(),data,64,.001,15)
        d.require_exact(response,b'OK\r\n','FOTA chunk')
    def reopen(reset=False):
        nonlocal link
        if link:link.close();link=None
        restore_usb(physical_reset=reset)
        opts.bridge_port=resolve_paired_bl616cl_port('auto')
        d.final_scan.require_final_carrier_bridge(opts.bridge_port)
        d.upload_proxy(opts,proxy)
        link=d.open_bridge(opts.bridge_port)
    try:
        proxy=d.compile_proxy(opts)
        prepared=PreparedBlReset(work)
        restore_usb()
        opts.bridge_port=resolve_paired_bl616cl_port('auto')
        installed=True
        d.upload_proxy(opts,proxy)
        link=d.open_bridge(opts.bridge_port)
        version(args.initial_version)
        if not args.skip_negative:
            for cmd in (b'AT+BLFOTABEGIN=0\r\n',b'AT+BLFOTABEGIN=-1\r\n',
                        b'AT+BLFOTABEGIN=1474561\r\n',b'AT+BLFOTAFINISH\r\n'):
                exact(cmd,b'ERROR\r\n')
            if args.check_header:
                begin(4096)
                result=d.transact_raw_payload(link,b'AT+BLFOTADATA=4096\r\n',bytes(4096),64,.001,15)
                d.require_exact(result,b'ERROR\r\n','reject non-BFNP image')
                print('SELF_FOTA_INVALID_HEADER_PASS',flush=True)
            begin(len(payload))
            pending=b'AT+BLFOTADATA=4096\r\n'+payload[:64]
            if link.write(pending)!=len(pending):
                raise RuntimeError('raw timeout setup write failed')
            link.flush()
            d.require_exact(d.read_until_terminal(link,8),b'ERROR\r\n','raw receive timeout')
            exact(b'AT+BLFOTAFINISH\r\n',b'ERROR\r\n')
            exact(b'AT+BLFOTAABORT\r\n',b'+BLFOTAABORT: 0\r\nOK\r\n')
            print('SELF_FOTA_RAW_TIMEOUT_ABORT_PASS',flush=True)
            begin(len(payload));chunk(payload[:4096])
            exact(b'AT+BLFOTAFINISH\r\n',b'ERROR\r\n')
            exact(b'AT+BLFOTAABORT\r\n',b'+BLFOTAABORT: 0\r\nOK\r\n')
            version(args.initial_version)
            print('SELF_FOTA_TRUNCATED_ABORT_PASS',flush=True)
            # A full, short corrupt payload exercises SDK flash readback/hash,
            # without ever activating it. Retry must work without a reboot.
            begin(4096);chunk(b'BFNP'+bytes(4092))
            exact(b'AT+BLFOTAFINISH\r\n',b'ERROR\r\n',30)
            begin(len(payload));chunk(payload[:4096])
            # Reset with the next raw block incomplete, before its 5s timeout.
            pending=b'AT+BLFOTADATA=4096\r\n'+payload[4096:4352]
            if link.write(pending)!=len(pending):
                raise RuntimeError('incomplete raw reset setup write failed')
            link.flush()
            link.close();link=None
            prepared.fire();prepared.close();prepared=None
            reopen();version(args.initial_version)
            print('SELF_FOTA_BAD_HASH_RETRY_AND_RESET_PASS',flush=True)
        if args.negative_only:
            print('BL616CL_SELF_FOTA_NEGATIVE_GATE_PASS',flush=True)
            return
        # Reset lines stay in normal boot mode during the firmware's own reset.
        if prepared is None:
            link.close();link=None
            prepared=PreparedBlReset(Path(tempfile.mkdtemp(prefix='finish-',dir=work)))
            restore_usb()
            opts.bridge_port=resolve_paired_bl616cl_port('auto')
            d.upload_proxy(opts,proxy);link=d.open_bridge(opts.bridge_port)
        begin(len(payload))
        started=time.monotonic()
        for offset in range(0,len(payload),4096):
            chunk(payload[offset:offset+4096])
            if offset%65536==0:
                print(f'SELF_FOTA_PROGRESS={min(offset+4096,len(payload))}/{len(payload)}',flush=True)
        exact(b'AT+BLFOTAFINISH\r\n',b'+BLFOTAFINISH: 0\r\nOK\r\n',30)
        link.close();link=None
        time.sleep(2)
        reopen();version(args.expected_version)
        print(f'SELF_FOTA_SWITCH_PASS elapsed_ms={int((time.monotonic()-started)*1000)}',flush=True)
    finally:
        if link:link.close()
        try:
            if prepared:prepared.close()
        finally:
            if installed:
                restore_usb()
                d.run_command(['python3',str(d.ROOT_DIR/'tools/run_ra4_wifis3_scan_final_carrier.py'),
                               '--expected-version',current_version])
    print('BL616CL_SELF_FOTA_GATE_PASS',flush=True)

if __name__=='__main__':main()
