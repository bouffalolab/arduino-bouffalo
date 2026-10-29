#!/usr/bin/env python3
"""Reset BL616CL with incomplete raw writes, then verify old and committed data.

Precise NOR program/erase interruption is covered by the actual-SDK host
fault model. This board test checks AT framing reset and persistence.
"""
import argparse
import time
import tempfile
from pathlib import Path
from prepared_bl616cl_reset import PreparedBlReset
import run_ra4_ota_download_final_carrier as d
from run_bl616cl_generic_at_final_carrier import restore_usb
from uno_r4_usb_identity import resolve_paired_bl616cl_port


def main():
    args=argparse.Namespace(bridge_port=resolve_paired_bl616cl_port("auto"),
                            arduino_cli="arduino-cli",runtime_settle_ms=1000)
    d.final_scan.require_final_carrier_bridge(args.bridge_port)
    link=None;installed=False;prepared=None
    work=Path(tempfile.mkdtemp(prefix="storage-reset-",dir=d.ROOT_DIR/"build"))
    def exact(command,response,label):
        d.require_exact(d.transact(link,command,5),response,label)
    def reset(prearmed=False, prepare_next=False):
        nonlocal link,prepared
        link.close();link=None
        if prearmed:
            prepared.fire()
            prepared.close()
            prepared=None
        else:
            restore_usb(physical_reset=True)
        args.bridge_port=resolve_paired_bl616cl_port("auto")
        d.final_scan.require_final_carrier_bridge(args.bridge_port)
        if prepare_next:
            prepared=PreparedBlReset(Path(tempfile.mkdtemp(prefix="armed-",dir=work)))
        d.upload_proxy(args,image) # RA-only restart; persisted BL data is untouched.
        link=d.open_bridge(args.bridge_port)
    def raw(command,payload,expected):
        result=d.transact_raw_payload(link,command,payload,32,.002,8)
        d.require_exact(result,expected,"committed write")
    def read(command,prefix,payload):
        link.reset_input_buffer();link.write(command);link.flush()
        expected=prefix+str(len(payload)).encode()+b"|"+payload+b"\r\nOK\r\n"
        response=bytearray();deadline=time.monotonic()+8
        while len(response)<len(expected) and time.monotonic()<deadline:
            response.extend(link.read(min(256,len(expected)-len(response))))
        d.require_exact(bytes(response),expected,"read exact data")
    old=bytes((i*37+11)%256 for i in range(6000))
    new=bytes((i*71+19)%256 for i in range(7000))
    cases=[
        ("file",b"AT+MOUNTFS=0\r\n",b"+MOUNTFS: FS Mounted\r\n\r\nOK\r\n",
         b"AT+FS=0,1,/reset-atomic.bin,%d\r\n",b"OK\r\n",
         b"AT+FS=0,2,/reset-atomic.bin,0,8192\r\n",b"+FS: "),
        ("preferences",b"AT+PREFBEGIN=resetatomic,0,\r\n",b"+PREFBEGIN: 1\r\n\r\nOK\r\n",
         b"AT+PREFPUT=blob,9,%d\r\n",None,b"AT+PREFGET=blob,9\r\n",b"+PREFGET: "),
        ("softse",b"AT+SOFTSEBEGIN=resetatomic,0,\r\n",b"+SOFTSEBEGIN: 1\r\nOK\r\n",
         b"AT+SOFTSE_WRITE_SLOT=203,%d\r\n",None,b"AT+SOFTSE_READ_SLOT=203\r\n",b"+SOFTSE_READ_SLOT: "),
    ]
    try:
        image=d.compile_proxy(args)
        prepared=PreparedBlReset(work)
        d.upload_proxy(args,image);installed=True
        link=d.open_bridge(args.bridge_port)
        for name,begin,begin_ok,write,write_ok,query,prefix in cases:
            if name!="file":
                old=old[:1024];new=new[:1536]
            # The FTDI is already armed before this proxy upload. A new
            # helper is prepared only while the link is closed after reset.
            exact(begin,begin_ok,"open "+name)
            def expected(size):
                if write_ok is not None:return write_ok
                return (b"+PREFPUT: " if name=="preferences" else b"+SOFTSE_WRITE_SLOT: ")+str(size).encode()+b"\r\nOK\r\n"
            raw(write%len(old),old,expected(len(old)))
            # The incomplete payload must never replace the old committed item.
            link.reset_input_buffer();command=write%len(new)
            assert link.write(command)==len(command)
            for offset in range(0,1024,32):
                assert link.write(new[offset:offset+32])==32
                time.sleep(.002)
            link.flush()
            print("STORAGE_INCOMPLETE_WRITE_RESET_ARMED="+name,flush=True)
            reset(prearmed=True);exact(begin,begin_ok,"reopen "+name)
            read(query,prefix,old)
            raw(write%len(new),new,expected(len(new)))
            reset(prepare_next=name!="softse");exact(begin,begin_ok,"committed reopen "+name)
            read(query,prefix,new)
            print("STORAGE_INCOMPLETE_AND_COMMITTED_RESET_PASS="+name,flush=True)
        # Remove only this gate's ordinary file and Preferences namespace.
        exact(b"AT+MOUNTFS=0\r\n",b"+MOUNTFS: FS Mounted\r\n\r\nOK\r\n","cleanup mount")
        exact(b"AT+FS=0,0,/reset-atomic.bin\r\n",b"OK\r\n","file cleanup")
        exact(b"AT+PREFBEGIN=resetatomic,0,\r\n",b"+PREFBEGIN: 1\r\n\r\nOK\r\n","pref cleanup open")
        exact(b"AT+PREFCLEAR\r\n",b"+PREFCLEAR: 1\r\nOK\r\n","pref cleanup")
        exact(b"AT+SOFTSE_WRITE_SLOT=203,0\r\n",b"+SOFTSE_WRITE_SLOT: 0\r\nOK\r\n","slot cleanup")
    finally:
        try:
            if prepared is not None:prepared.close()
        finally:
            try:
                if link is not None:link.close()
            finally:
                if installed:d.restore_scan(args)
    print("RA4_STORAGE_INTERRUPTION_FINAL_CARRIER_PASS")
    return 0


if __name__=="__main__":raise SystemExit(main())
