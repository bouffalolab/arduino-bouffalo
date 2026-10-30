"""Read optional IPv6 echo and passive header counters; no addresses/payloads."""
import json
import os
from inspect_bl616cl_gpio35 import find_hidraw, read_feature, select_feature_page, FEATURE_VERSION

FIELDS = ('state', 'port', 'connections', 'received', 'sent', 'closed', 'errors',
          'last_error', 'pending', 'rx_ipv6', 'tx_ipv6', 'rx_ns', 'tx_na', 'rx_syn')


def capture():
    fd = os.open(find_hidraw(), os.O_RDWR | os.O_NONBLOCK)
    try:
        select_feature_page(fd, 61)
        report = read_feature(fd)
        if len(report) != 64 or report[:8] != b'U35D' + bytes((FEATURE_VERSION, 61, 1, 1)):
            raise RuntimeError('IPv6 TCP diagnostic unavailable or disabled')
        values = dict(zip(FIELDS, (int.from_bytes(report[i:i+4], 'little') for i in range(8, 64, 4))))
        if values['state'] > 3 or values['port'] != 46160 or values['pending'] > 256:
            raise RuntimeError('invalid IPv6 TCP diagnostic snapshot')
        return values
    finally:
        try:
            select_feature_page(fd, 0)
        finally:
            os.close(fd)


def log_snapshot():
    result = capture()
    print('IPV6_TCP_DIAG=' + json.dumps(result, sort_keys=True), flush=True)
    return result


if __name__ == '__main__':
    log_snapshot()
