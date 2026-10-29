#!/usr/bin/env python3
"""Basic passive STA recovery using the authorized original UNO R4 as upstream AP.

Only RA4M1 proxy Sketches are replaced. Credentials and network addresses stay
in memory. Every normal exit restores both scan Sketches. The optional host-services stage temporarily
joins Windows to the test AP and restores its previous WLAN connection.
"""
import argparse
import ipaddress
import re
import secrets
import sys
import time
from pathlib import Path

import serial
import run_bl616cl_hostname_ipv6_final_carrier as bl
import run_esp32_sta_baseline_gate as esp
import run_esp32_softap_smoke as ap
from ble_independent_failure_probe import prepare_baseline_usb
from uno_r4_usb_identity import resolve_baseline_esp32_port, resolve_paired_bl616cl_port


def command(link, name, args=None, payload=b'', timeout=8):
    request = ('AT+' + name + ('=' + str(args) if args is not None else '') + '\r\n').encode('ascii')
    link.reset_input_buffer()
    # Bound writes to avoid overrunning the RA4M1 proxy UART receive buffer.
    data = request + payload
    for offset in range(0, len(data), 64):
        link.write(data[offset:offset+64]); link.flush()
        if payload:
            time.sleep(.008)
    result = bytearray(); deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result.extend(link.read(256))
        if len(result) > 20000:
            raise RuntimeError(name + ' response limit')
        if result.endswith((b'OK\r\n', b'ERROR\r\n')):
            if result.endswith(b'ERROR\r\n'):
                raise RuntimeError(name + ' rejected')
            return bytes(result)
    raise RuntimeError(name + ' response timeout bytes=' + str(len(result)))


def number(link, name, args=None):
    data = command(link, name, args)
    match = re.fullmatch(rb'\+' + name.encode() + rb': (-?\d+)\r\nOK\r\n', data)
    if not match:
        raise RuntimeError(name + ' numeric framing')
    return int(match[1])


def address(link, name, args=None):
    data = command(link, name, args)
    match = re.fullmatch(rb'\+' + name.encode() + rb': ([0-9.]+)\r\n(?:\r\n)?OK\r\n', data)
    if not match:
        raise RuntimeError(name + ' IPv4 framing')
    value = ipaddress.IPv4Address(match[1].decode())
    if value.is_unspecified:
        raise RuntimeError(name + ' zero IPv4')
    return str(value)


def wait_value(read, predicate, seconds, label):
    deadline = time.monotonic() + seconds
    values = []
    while time.monotonic() < deadline:
        value = read()
        if not values or values[-1] != value:
            values.append(value)
        if predicate(value):
            print(label + '_PASS statuses=' + ','.join(map(str, values)), flush=True)
            return value
        time.sleep(.25)
    raise RuntimeError(label + ' deadline statuses=' + ','.join(map(str, values)))


def passive_cycle(sta, stop_ap, start_ap, enabled, settle_seconds=15):
    """During outage/recovery issue only GETSTATUS on the station."""
    stop_ap()
    wait_value(lambda: bl.read_status(sta, 4),
               lambda status: status != 3, 45, 'PASSIVE_AP_LOSS')
    start_ap()
    if enabled:
        wait_value(lambda: bl.read_status(sta, 4), lambda status: status == 3,
                   90, 'PASSIVE_AUTO_RECOVERY')
    else:
        deadline = time.monotonic() + settle_seconds
        while time.monotonic() < deadline:
            if bl.read_status(sta, 4) == 3:
                raise RuntimeError('RECONNECT=0 rejoined without application request')
            time.sleep(.25)
        print('PASSIVE_RECONNECT_DISABLED_PASS', flush=True)


def receive(link, name, sock, expected):
    received = bytearray(); deadline = time.monotonic() + 15
    while len(received) < len(expected) and time.monotonic() < deadline:
        result = command(link, name, f'{sock},{len(expected)-len(received)}')
        match = re.fullmatch(rb'\+' + name.encode() + rb': (\d+)\|(.*)OK\r\n', result, re.S)
        if match and name in ('UDPREAD', 'SSLCLIENTRECEIVE') and len(match[2]) == int(match[1]) + 2 and match[2].endswith(b'\r\n'):
            part = match[2][:-2]
        else:
            part = match[2] if match else b''
        if not match or int(match[1]) != len(part):
            raise RuntimeError(name + ' payload/framing mismatch bytes=' + str(len(result)))
        received.extend(part)
        if not part: time.sleep(.05)
    if received != expected:
        raise RuntimeError(name + ' payload mismatch received=' + str(len(received)))


TCP_PORT = 18470


def tcp_exchange(sta, peer, gateway, label):
    global TCP_PORT
    TCP_PORT += 1
    port = TCP_PORT
    server = number(peer, 'BEGINSERVER', port)
    client = accepted = None
    payload = bytes((i * 17 + 9) % 251 for i in range(256))
    try:
        client = number(sta, 'BEGINCLIENT')
        command(sta, 'CLIENTCONNECTIP', f'{client},{gateway},{port}')
        command(sta, 'CLIENTSEND', f'{client},{len(payload)}', payload)
        accepted = wait_value(lambda: number(peer, 'SERVERAVAILABLE', server),
                              lambda value: value >= 1000, 10, label + '_ACCEPT')
        wait_value(lambda: number(peer, 'AVAILABLE', accepted), lambda n: n > 0, 10, label + '_RX')
        receive(peer, 'CLIENTRECEIVE', accepted, payload)
        command(peer, 'CLIENTSEND', f'{accepted},{len(payload)}', payload)
        wait_value(lambda: number(sta, 'AVAILABLE', client), lambda n: n > 0, 10, label + '_RETURN')
        receive(sta, 'CLIENTRECEIVE', client, payload)
        print(label + '_TCP_BIDIRECTIONAL_PASS bytes=256', flush=True)
    finally:
        for link, sock in ((sta, client), (peer, accepted)):
            if sock is not None:
                command(link, 'CLIENTCLOSE', sock)
        command(peer, 'SERVEREND', server)


def udp_exchange(sta, peer, gateway, label):
    sta_address = address(sta, 'IPSTA', 0)
    one = number(sta, 'UDPBEGIN', f'18462,{sta_address}')
    two = None
    payload = bytes((i * 23 + 7) % 251 for i in range(1460))
    try:
        two = number(peer, 'UDPBEGIN', 18463)
        sta_address = address(sta, 'IPSTA', 0)
        for source, target, sock, target_sock, host, port in (
                (sta, peer, one, two, gateway, 18463),
                (peer, sta, two, one, sta_address, 18462)):
            command(source, 'UDPBEGINPACKETIP', f'{sock},{port},{host}')
            command(source, 'UDPWRITE', f'{sock},{len(payload)}', payload)
            command(source, 'UDPENDPACKET', sock)
            wait_value(lambda: number(target, 'UDPPARSE', target_sock), lambda n: n == len(payload), 15, label + '_UDP_RX')
            receive(target, 'UDPREAD', target_sock, payload)
        print(label + '_UDP_BIDIRECTIONAL_PASS bytes=1460', flush=True)
    finally:
        command(sta, 'UDPSTOP', one)
        if two is not None:
            command(peer, 'UDPSTOP', two)


def wait_peer_dhcp(peer, network, gateway, seconds=30):
    """ESP's STA_CONNECTED status precedes GOT_IP; do not start TCP yet."""
    subnet = ipaddress.IPv4Network(network)
    def ready():
        try:
            host = ipaddress.IPv4Address(address(peer, 'IPSTA', 0))
            router = address(peer, 'IPSTA', 1)
        except RuntimeError:
            return False
        return host in subnet and str(host) != gateway and router == gateway
    wait_value(ready, bool, seconds, 'APSTA_LOCAL_DHCP_READY')


def apsta_recovery(sta, peer, upstream_request, gateway, ssid, password):
    """Keep the BL local AP usable while its separate upstream AP disappears."""
    local_ssid = 'uno-local-' + secrets.token_hex(4)
    local_password = secrets.token_hex(12)
    primary = None
    try:
        command(sta, 'WIFIMODE', 3)
        command(peer, 'WIFIMODE', 3)
        command(sta, 'SOFTAPCONFIG', '192.168.7.1,192.168.7.1,255.255.255.0')
        ap.require_softap_start_response(bl.transact(sta,
            ap.build_softap_start_request(local_ssid, local_password, 6), 10))
        time.sleep(3)
        command(peer, 'RECONNECT', 0)
        esp.validate_beginsta(bl.transact(peer, bl.begin_sta_request(local_ssid, local_password), 10))
        wait_value(lambda: bl.read_status(peer, 4), lambda n: n == 3, 45, 'APSTA_LOCAL_CLIENT_JOIN')
        if address(sta, 'IPSOFTAP') != '192.168.7.1':
            raise RuntimeError('BL local AP configured address mismatch')
        wait_peer_dhcp(peer, '192.168.7.0/24', '192.168.7.1')
        tcp_exchange(peer, sta, '192.168.7.1', 'LOCAL_AP_BEFORE_UPSTREAM_LOSS')
        bl.set_reconnect(sta, True, 4)
        command(peer, 'DISCONNECTSOFTAP', 0)
        wait_value(lambda: bl.read_status(sta, 4), lambda n: n == 0, 45, 'APSTA_UPSTREAM_LOSS')
        tcp_exchange(peer, sta, '192.168.7.1', 'LOCAL_AP_DURING_UPSTREAM_LOSS')
        ap.require_softap_start_response(bl.transact(peer, upstream_request, 10))
        wait_value(lambda: bl.read_status(sta, 4), lambda n: n == 3, 90, 'APSTA_UPSTREAM_RECOVERY')
        udp_exchange(sta, peer, gateway, 'APSTA_UPSTREAM_SERVICE_RECOVERY')
        tcp_exchange(peer, sta, '192.168.7.1', 'LOCAL_AP_AFTER_UPSTREAM_RECOVERY')
        print('APSTA_LOCAL_SERVICE_SURVIVES_UPSTREAM_LOSS_PASS', flush=True)
    except Exception as error:
        primary = error
        print('APSTA_PRIMARY_FAILURE=' + str(error), flush=True)
        raise
    finally:
        try:
            # Return the peer to AP-only and the BL to STA-only before host traffic.
            for link, name, value in ((peer, 'DISCONNECT', None),
                                      (sta, 'DISCONNECTSOFTAP', 0),
                                      (sta, 'WIFIMODE', 1)):
                command(link, name, value)
            # WIFIMODE=2 leaves the peer's existing AP role enabled.
            command(peer, 'WIFIMODE', 2)
            ap.require_softap_start_response(bl.transact(peer, upstream_request, 10))
            # Mode teardown is explicit fixture cleanup, not passive recovery evidence.
            bl.require_exact(bl.transact(sta, bl.DISCONNECT_REQUEST, 8), bl.DISCONNECT_RESPONSE, 'APSTA cleanup disconnect')
            esp.validate_beginsta(bl.transact(sta, bl.begin_sta_request(
                ssid, password), 10))
            wait_value(lambda: bl.read_status(sta, 4), lambda n: n == 3, 60, 'APSTA_CLEANUP_STA_REJOIN')

        except Exception as error:
            print('APSTA_MODE_CLEANUP_FAILED=' + str(error), flush=True)
            if primary is None: raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sta-ota', action='store_true', help='Also verify STA HTTP/HTTPS OTA on this controlled AP, without private WLAN credentials')
    parser.add_argument('--arduino-cli', default='/home/developer/.local/bin/arduino-cli')
    parser.add_argument('--ap-udp-control', '--ap-fragment-control', dest='ap_fragment_control', action='store_true', help='Diagnostic: verify direct-to-original-AP unfragmented 1460 B UDP; original SDK disables IPv4 reassembly')
    parser.add_argument('--ap-tx-diagnostic', action='store_true', help='Read AP TX-completion fragment metadata; requires the temporary observer firmware')
    parser.add_argument('--host-udp-profile', choices=('basic','extended'), default='extended', help='basic: baseline 1460-byte boundary plus 4 KiB; extended: also three 16 KiB datagrams (default)')
    parser.add_argument('--udp-large-count', type=int, choices=range(1,65), default=3, help='Consecutive largest UDP datagrams, each sent once by default; 3 preserves the acceptance gate')
    parser.add_argument('--udp-peer', choices=('windows','wsl','ap-native'), default='windows', help='Diagnostic sender; ap-native requires temporary ESP diagnostic firmware and validates full STA receive plus 32 B ACK only; TLS remains on Windows')
    parser.add_argument('--udp-attempts', type=int, choices=range(1,4), default=1, help='Bounded host UDP attempts per datagram; every send/timeout is recorded')
    parser.add_argument('--skip-recovery-cases', action='store_true', help='Run only the separately evidenced host-services stage')
    parser.add_argument('--host-services-work-dir', type=Path, help='Also join Windows to the AP for 16 KiB UDP and verified TLS; restore host WLAN on exit')
    args = parser.parse_args()
    if args.skip_recovery_cases and args.host_services_work_dir is None:
        raise RuntimeError('host-services stage requires a work directory')
    args.fqbn = esp.DEFAULT_FQBN
    args.runtime_settle_ms = 1500
    args.scan_startup_timeout_ms = 100
    args.scan_capture_timeout_ms = 45000
    try:
        args.port = resolve_baseline_esp32_port('auto')
    except RuntimeError:
        prepare_baseline_usb()
        args.port = resolve_baseline_esp32_port('auto')
    args.bridge_port = resolve_paired_bl616cl_port('auto')
    esp.require_port_identity(args.port); bl.require_final_carrier_bridge(args.bridge_port)
    print('TARGET_ESP32_RA4=' + args.port + ' TARGET_BL616CL_RA4=' + args.bridge_port, flush=True)
    print('Temporarily replace both RA4M1 Sketches; both bridge firmware images are unchanged.', flush=True)
    ssid = 'uno-recovery-' + secrets.token_hex(4); password = secrets.token_hex(12)
    start_request = ap.build_softap_start_request(ssid, password, 6).replace(b',0,1\r\n', b',0,4\r\n')
    uploaded_esp = uploaded_bl = False
    sta = peer = None; original_policy = None; primary = None; host_services_done = False
    try:
        uploaded_esp = True
        esp.compile_and_upload(args, esp.PROXY_SKETCH, esp.PROXY_BUILD_DIR, 'ESP32_RA4_PROXY')
        image = bl.compile_proxy(args)
        uploaded_bl = True
        bl.upload_proxy(args, image)
        esp.require_port_identity(args.port); bl.require_final_carrier_bridge(args.bridge_port)
        peer = esp.open_serial(args.port, .1)
        sta = serial.Serial(args.bridge_port, 115200, timeout=.1, write_timeout=3)
        sta.dtr = False; sta.rts = False; time.sleep(1.5)
        esp.parse_firmware(command(peer, 'FWVERSION?'))
        firmware = esp.parse_firmware(command(sta, 'FWVERSION?'))
        print('BL_FIRMWARE=' + firmware, flush=True)
        if firmware != '0.6.0':
            raise RuntimeError('expected freshly built 0.6.0 candidate; check active FOTA slot')
        original_policy = bl.read_reconnect(sta, 4)
        # Resource gates leave AP-only mode; normalize before the STA fixture.
        command(sta, 'WIFIMODE', 1)
        def start():
            ap.require_softap_start_response(esp.transact(peer, start_request, 8))
            wait_value(lambda: bl.read_status(peer, 4), lambda s: s in (7,8), 15, 'UPSTREAM_AP_ON')
        def stop():
            command(peer, 'DISCONNECTSOFTAP', 1)
            wait_value(lambda: bl.read_status(peer, 4), lambda s: s == 0, 15, 'UPSTREAM_AP_OFF')
        def join(enabled):
            bl.set_reconnect(sta, enabled, 4)
            response = bl.transact(sta, bl.begin_sta_request(ssid, password), 10)
            if not re.fullmatch(rb'\+BEGINSTA: \d+\r\nOK\r\n', response):
                raise RuntimeError('BEGINSTA framing')
            wait_value(lambda: bl.read_status(sta, 4), lambda s: s == 3, 60, 'STA_JOIN')
        start(); gateway = address(peer, 'IPSOFTAP')
        bl.set_reconnect(sta, False, 4)
        bl.require_exact(bl.transact(sta, bl.DISCONNECT_REQUEST, 8), bl.DISCONNECT_RESPONSE, 'initial disconnect')
        join(True)
        original_hostname = bl.read_hostname(sta, 4)
        lease = address(sta, 'IPSTA', 0)
        mask = address(sta, 'IPSTA', 2)
        command(sta, 'SETIP', f'{lease},{gateway},{mask},192.0.2.53,0.0.0.0')
        if address(sta, 'IPSTA', 3) != '192.0.2.53':
            raise RuntimeError('DHCP DNS override did not apply')
        if not args.skip_recovery_cases:
            tcp_exchange(sta, peer, gateway, 'BEFORE_OUTAGE')
            udp_exchange(sta, peer, gateway, 'BEFORE_OUTAGE')
            command(sta, 'IPV6'); bl.wait_for_link_local_ipv6(sta, 15, 4)
            passive_cycle(sta, stop, start, True)
            address(sta, 'IPSTA', 0)
            # Do not re-enable IPv6: check the existing setting survives the outage.
            bl.wait_for_link_local_ipv6(sta, 15, 4)
            print('PASSIVE_IPV6_LINK_LOCAL_RECOVERY_PASS', flush=True)
            if bl.read_hostname(sta, 4) != original_hostname or address(sta, 'IPSTA', 3) != '192.0.2.53':
                raise RuntimeError('hostname or DNS override lost on passive recovery')
            print('PASSIVE_HOSTNAME_DHCP_DNS_RECOVERY_PASS', flush=True)
            tcp_exchange(sta, peer, gateway, 'AFTER_OUTAGE')
            udp_exchange(sta, peer, gateway, 'AFTER_OUTAGE')
            if args.host_services_work_dir:
                from controlled_ap_host_services import run_services
                sta = run_services(args, sta, ssid, password, gateway, args.host_services_work_dir)
                host_services_done = True
                print('PASSIVE_RECOVERY_VERIFIED_TLS_UDP_PASS profile=' + args.host_udp_profile, flush=True)
            # Test changing the policy on an already connected station.
            bl.set_reconnect(sta, False, 4)
            passive_cycle(sta, stop, start, False)
            join(False)
            tcp_exchange(sta, peer, gateway, 'MANUAL_RECOVERY')
            udp_exchange(sta, peer, gateway, 'MANUAL_RECOVERY')
            bl.set_reconnect(sta, False, 4)
            bl.require_exact(bl.transact(sta, bl.DISCONNECT_REQUEST, 8), bl.DISCONNECT_RESPONSE, 'startup disconnect')
            stop()
            bl.set_reconnect(sta, True, 4)
            response = bl.transact(sta, bl.begin_sta_request(ssid, password), 10)
            if not re.fullmatch(rb'\+BEGINSTA: \d+\r\nOK\r\n', response):
                raise RuntimeError('absent AP BEGINSTA framing')
            time.sleep(5)
            if bl.read_status(sta, 4) == 3:
                raise RuntimeError('STA connected while AP unavailable')
            start()
            wait_value(lambda: bl.read_status(sta, 4), lambda s: s == 3, 90, 'AP_ABSENT_STARTUP_AUTO_RECOVERY')
            tcp_exchange(sta, peer, gateway, 'STARTUP_RECOVERY')
            udp_exchange(sta, peer, gateway, 'STARTUP_RECOVERY')
            apsta_recovery(sta, peer, start_request, gateway, ssid, password)

        if args.host_services_work_dir is not None and not host_services_done:
            from controlled_ap_host_services import run_services
            sta = run_services(args, sta, ssid, password, gateway, args.host_services_work_dir)
        if args.sta_ota:
            from controlled_ap_ota import run_sta_ota
            sta.close()
            run_sta_ota(args,ssid,password,gateway)
    except Exception as error:
        print("CONTROLLED_AP_FAILURE=" + str(error), flush=True)
        primary = error
    finally:
        if sta is not None and not sta.is_open:
            try:
                args.bridge_port = resolve_paired_bl616cl_port('auto')
                sta = serial.Serial(args.bridge_port, 115200, timeout=.1, write_timeout=3)
                sta.dtr = False; sta.rts = False
            except Exception as error:
                sta = None
                if primary is None: primary = error
        actions = []
        if sta is not None:
            actions.extend([lambda: bl.set_reconnect(sta, False, 4),
                            lambda: bl.transact(sta, bl.DISCONNECT_REQUEST, 8),
                            lambda: command(sta, 'SOFTRESETWIFI')])
            if original_policy is not None:
                actions.append(lambda: bl.set_reconnect(sta, original_policy, 4))
        if peer is not None:
            actions.append(lambda: command(peer, 'DISCONNECTSOFTAP', 1))
        for action in actions:
            try: action()
            except Exception as error:
                print('AP_RECOVERY_STATE_CLEANUP_FAILED', flush=True)
                if primary is None: primary = error
        for link in (sta, peer):
            if link is not None: link.close()
        for modified, restore in ((uploaded_bl, lambda: bl.restore_scan(args)),
                                  (uploaded_esp, lambda: esp.restore_scan_smoke(args))):
            if modified:
                try: restore()
                except Exception as error:
                    print('AP_RECOVERY_SCAN_RESTORE_FAILED', flush=True)
                    if primary is None: primary = error
    if primary is not None: raise primary
    print('CONTROLLED_AP_NATIVE_UDP_DIAGNOSTIC_PASS' if args.udp_peer=='ap-native' else 'CONTROLLED_AP_HOST_SERVICES_ONLY_PASS' if args.skip_recovery_cases
          else 'CONTROLLED_AP_RECOVERY_GATE_PASS', flush=True)


if __name__ == '__main__':
    main()
