#!/usr/bin/env python3
"""Read passive STA TCP headers; never print payload or IP addresses."""
import os
import argparse
from inspect_bl616cl_gpio35 import find_hidraw, read_feature, select_feature_page, FEATURE_VERSION

def u32(report, offset):
    return int.from_bytes(report[offset:offset + 4], 'little')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tx-release', action='store_true', help='Read layout-1 completion page58 on supporting images')
    args = parser.parse_args()
    fd = os.open(find_hidraw(), os.O_RDWR | os.O_NONBLOCK)
    try:
        for page in (46, 47, 48, 49, 50, 51) + ((58,) if args.tx_release else ()):
            select_feature_page(fd, page)
            report = read_feature(fd)
            if report[:5] != b'U35D' + bytes((FEATURE_VERSION,)) or report[5] != page:
                raise RuntimeError('STA TCP diagnostic page unavailable on this image')
            if page == 46:
                names = ('generation', 'local_port', 'rx_packets', 'rx_bytes', 'last_seq',
                         'highest_end', 'last_length', 'tx_acks', 'last_ack', 'repeated_acks',
                         'rx_flags', 'tx_flags', 'window_raw', 'invalid_frames')
                print('STA_TCP=enabled:' + str(report[6]) + ',' + ','.join(
                    name + ':' + str(u32(report, 8 + 4 * i)) for i, name in enumerate(names)))
            elif page == 47:
                count = u32(report, 12)
                print(f'STA_TCP_EVENTS=generation:{u32(report, 8)},count:{count}')
                for index in range(4 - min(count, 4), 4):
                    offset = 16 + 12 * index
                    print('STA_TCP_EVENT=' + ('tx' if report[offset + 11] else 'rx')
                          + f',seq:{u32(report, offset)},ack:{u32(report, offset + 4)}'
                          + f',length:{int.from_bytes(report[offset + 8:offset + 10], "little")}'
                          + f',flags:{report[offset + 10]}')
            elif page == 58:
                if report[7] != 1:
                    raise RuntimeError('Unsupported STA TCP TX release layout')
                names = ('generation', 'local_port', 'released', 'mac_acknowledged', 'discarded',
                         'released_ack', 'mac_acknowledged_ack', 'discarded_ack', 'peer_port')
                print('STA_TCP_RELEASE=enabled:' + str(report[6]) + ',' + ','.join(
                    name + ':' + str(u32(report, 8 + 4 * i)) for i, name in enumerate(names)))
            elif page == 51:
                print('STA_RX_OWNERSHIP=enabled:' + str(report[6]) + ',ooseq_pbuf_limit:' + str(report[7]) + ',' + ','.join(
                    name + ':' + str(u32(report, 8+4*i))
                    for i, name in enumerate(('copied', 'failed', 'bytes')))
                    + ',' + ','.join(name + ':' + str(u32(report, offset)) for offset, name in (
                        (20, 'ooseq_global_limit'), (24, 'rx_reserve'), (28, 'reserve_dropped'),
                        (32, 'receive_window'), (36, 'rx_chunk')))
                    + (',tx_extra:' + str(u32(report, 40)) if report[44:48] == b'TXP1' else '')
                    + (',diagnostic_peer_port:' + str(u32(report, 48)) if report[52:56] == b'TCP1' else '')
                    + (',ap_owned_rx:' + str(report[56]) + ',atomic_rx_reserve:' + str(report[61]) if report[57:61] == b'APR1' else ''))
            else:
                if report[7] != 2:
                    raise RuntimeError('Unsupported MAC RX diagnostic layout')
                names = (('mac_entry', 'mac_unsuccessful', 'mac_exit', 'descriptor_busy',
                          'descriptor_prepare', 'transfer_failed', 'reorder_check',
                          'reorder_queued', 'reorder_removed', 'reorder_timer',
                          'reorder_release', 'fhost_message', 'fhost_forward', 'fhost_delete')
                         if page == 48 else
                         ('net_al_input', 'mac_not_uploaded', 'data_entry', 'pn_reject',
                          'duplicate_reject', 'data_retry', 'data_keyid_clear', 'data_unsuccessful',
                          'last_event', 'value0', 'value1', 'value2') if page == 49 else
                         ('buffer_get', 'buffer_return', 'buffer_empty_check',
                          'buffer_get_failed', 'buffer_return_failed'))
                print('MAC_RX=page:' + str(page) + ',enabled:' + str(report[6]) + ',' + ','.join(
                    name + ':' + str(u32(report, 8 + 4 * i)) for i, name in enumerate(names)))
    finally:
        try:
            select_feature_page(fd, 0)
        finally:
            os.close(fd)

if __name__ == '__main__':
    main()
