#!/usr/bin/env python3
"""Read bounded AP state fields without network names, keys or heap addresses."""
import json
import os
from inspect_bl616cl_gpio35 import find_hidraw, read_feature, select_feature_page, FEATURE_VERSION


def decode(report, page):
    if len(report) != 64 or report[:6] != b'U35D'+bytes((FEATURE_VERSION,page)):
        raise ValueError('AP lifecycle diagnostic frame mismatch')
    def u32(offset):return int.from_bytes(report[offset:offset+4],'little')
    if page == 2:
        result=dict(zip(('stage','native_result','sta_vif','ap_vif','sta_connected',
                         'ap_started','channel'),report[6:13]))
        result.update(attempts=u32(16),elapsed_ms=u32(20),started_ms=u32(24),completed_ms=u32(28))
        return result
    if page == 24:
        count=report[32]
        if count>3:raise ValueError('AP lifecycle record count out of range')
        return dict(persisted=report[6],phase_mask=u32(7),last_phase=report[11],
                    result=report[12],iface_state=report[13],wait_channel=report[14],
                    deauth_reason=int.from_bytes(report[30:32],'little'),
                    records=[dict(phase=report[33+n*8],result=report[34+n*8],
                                  iface_state=report[35+n*8],wait_channel=report[36+n*8])
                             for n in range(count)])
    if page == 25:
        return dict(persisted=report[6],last_phase=report[9],vif=report[10],
                    message_kind=report[11],raw_state=report[14],vif_state=report[15],
                    result=report[16],updates=u32(20))
    if page == 26:
        count=report[7]
        if count>3:raise ValueError('AP state record count out of range')
        return dict(persisted=report[6],next_sequence=u32(8),records=[
            dict(zip(('sequence','hostapd_state','enabled','raw_before','vif_before','raw_after','vif_after'),
                     (u32(12+n*12),*report[16+n*12:22+n*12]))) for n in range(count)])
    if page == 9:
        count=report[9]
        if count>8:raise ValueError('bootstrap record count out of range')
        return dict(operation=report[6],active=report[7],succeeded=report[8],sequence=u32(10),
                    records=[dict(zip(('step','boot','reset','boot_level','reset_level','succeeded'),
                                      report[14+n*6:20+n*6])) for n in range(count)])
    if page in (12,13):
        return dict(initialized=report[6],rx_fifo=report[7],received=u32(20),overflows=u32(24),
                    rx_config=u32(12),bit_period=u32(16))
    if page in (34,35):
        return dict(state=report[6],error=report[7],active=report[8],succeeded=report[9],
                    sequence=u32(10),started_ms=u32(14),completed_ms=u32(18),
                    transmitted=u32(22),received=u32(26))
    raise ValueError('unsupported AP lifecycle page')


def capture_pages(pages):
    fd=os.open(find_hidraw(),os.O_RDWR|os.O_NONBLOCK)
    try:
        result={}
        for page in pages:
            select_feature_page(fd,page);result[str(page)]=decode(read_feature(fd),page)
        return result
    finally:
        try:select_feature_page(fd,0)
        finally:os.close(fd)


def capture():return capture_pages((2,24,25,26))

def capture_ra4():return capture_pages((9,12,13,34,35))


if __name__=='__main__':print(json.dumps(capture(),sort_keys=True))
