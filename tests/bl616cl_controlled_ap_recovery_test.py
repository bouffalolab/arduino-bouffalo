#!/usr/bin/env python3
"""Ensure passive recovery evidence cannot be replaced with an active rejoin."""
import sys
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import run_bl616cl_controlled_ap_recovery as gate


class PassiveRecoveryTest(unittest.TestCase):
    def test_enabled_only_polls_until_real_loss_then_return(self):
        events = []
        values = iter((3, 0, 0, 3))
        def status(sta, timeout):
            events.append('status')
            return next(values)
        with patch.object(gate.bl, 'read_status', side_effect=status), \
             patch.object(gate.time, 'sleep'), \
             patch.object(gate.bl, 'transact', side_effect=AssertionError('active command')):
            gate.passive_cycle(object(), lambda: events.append('off'),
                               lambda: events.append('on'), True)
        self.assertEqual(events, ['off', 'status', 'status', 'on', 'status', 'status'])

    def test_disabled_rejects_an_automatic_join(self):
        with patch.object(gate.bl, 'read_status', side_effect=(0, 3)):
            with self.assertRaisesRegex(RuntimeError, 'RECONNECT=0'):
                gate.passive_cycle(object(), lambda: None, lambda: None, False)

    def test_no_disconnect_is_not_a_pass(self):
        with patch.object(gate.bl, 'read_status', return_value=3), \
             patch.object(gate.time, 'monotonic', side_effect=(0, 0, 0, 46)), \
             patch.object(gate.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'PASSIVE_AP_LOSS'):
                gate.passive_cycle(object(), lambda: None, lambda: self.fail('premature recovery'), True)

    def test_peer_dhcp_must_be_on_local_ap_subnet(self):
        with patch.object(gate, 'address', side_effect=(RuntimeError('zero IPv4'),
                '192.168.4.2', '192.168.4.1', '192.168.7.2', '192.168.7.1')), \
             patch.object(gate.time, 'sleep'):
            gate.wait_peer_dhcp(object(), '192.168.7.0/24', '192.168.7.1')
        with patch.object(gate, 'address', return_value='192.168.4.2'), \
             patch.object(gate.time, 'monotonic', side_effect=(0,0,0,31)), \
             patch.object(gate.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'APSTA_LOCAL_DHCP_READY'):
                gate.wait_peer_dhcp(object(), '192.168.7.0/24', '192.168.7.1')

    def test_tcp_partial_reads_and_original_udp_newline(self):
        with patch.object(gate, 'command', side_effect=(
                b'+CLIENTRECEIVE: 1|aOK\r\n', b'+CLIENTRECEIVE: 2|bcOK\r\n')):
            gate.receive(object(), 'CLIENTRECEIVE', 0, b'abc')
        with patch.object(gate, 'command', return_value=b'+UDPREAD: 3|abc\r\nOK\r\n'):
            gate.receive(object(), 'UDPREAD', 0, b'abc')
        with patch.object(gate, 'command', return_value=b'+SSLCLIENTRECEIVE: 3|abc\r\nOK\r\n'):
            gate.receive(object(), 'SSLCLIENTRECEIVE', 0, b'abc')
        with patch.object(gate, 'command', return_value=b'+UDPREAD: 3|abOK\r\n'):
            with self.assertRaisesRegex(RuntimeError, 'framing'):
                gate.receive(object(), 'UDPREAD', 0, b'abc')


if __name__ == '__main__': unittest.main()
