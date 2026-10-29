"""Active rejoin must check readiness and configuration, without logging secrets."""
import contextlib
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import run_bl616cl_hostname_ipv6_final_carrier as gate


class ReconnectTest(unittest.TestCase):
    def exercise(self, *, bad_policy=False, bad_begin=False, bad_hostname=False, no_ipv6=False):
        events = []
        policy = [False]
        def set_policy(_bridge, enabled, _timeout):
            policy[0] = enabled
        def transact(_bridge, request, _timeout):
            if request == gate.DISCONNECT_REQUEST:
                events.append('disconnect')
                return gate.DISCONNECT_RESPONSE
            if request.startswith(b'AT+BEGINSTA='):
                events.append('join')
                return b'ERROR\r\n' if bad_begin else b'+BEGINSTA: 0\r\nOK\r\n'
            self.assertEqual(request, gate.IPV6_ENABLE_REQUEST)
            events.append('ipv6_enable')
            return gate.TERMINAL
        def wait_status(_bridge, expected, *_args):
            events.append(('status', expected))
        def ipv6(*_args):
            events.append('ipv6_ready')
            if no_ipv6:
                raise RuntimeError('missing IPv6')
        output = io.StringIO()
        with patch.object(gate, 'set_reconnect', side_effect=set_policy), \
                patch.object(gate, 'read_reconnect', side_effect=lambda *_: not policy[0] if bad_policy else policy[0]), \
                patch.object(gate, 'transact', side_effect=transact), \
                patch.object(gate, 'wait_for_status', side_effect=wait_status), \
                patch.object(gate, 'read_hostname', return_value='wrong' if bad_hostname else gate.TEST_HOSTNAME), \
                patch.object(gate, 'wait_for_link_local_ipv6', side_effect=ipv6), \
                contextlib.redirect_stdout(output):
            try:
                gate.verify_active_reconnect(object(), 'private-ssid', 'private-password', 10, 30, 15, 1)
            except RuntimeError:
                return events, output.getvalue(), False
        return events, output.getvalue(), True

    def test_success_waits_for_both_network_states_and_ipv6(self):
        events, output, passed = self.exercise()
        self.assertTrue(passed)
        self.assertEqual(events, ['disconnect', ('status', 0), 'join', ('status', 3), 'ipv6_enable', 'ipv6_ready'])
        self.assertIn('ACTIVE_STA_RECONNECT_IPV6_PASS=1', output)
        self.assertNotIn('private', output)

    def test_each_missing_requirement_stops_without_pass(self):
        for condition in ('bad_policy', 'bad_begin', 'bad_hostname', 'no_ipv6'):
            with self.subTest(condition=condition):
                events, output, passed = self.exercise(**{condition: True})
                self.assertFalse(passed)
                self.assertNotIn('ACTIVE_STA_RECONNECT_IPV6_PASS', output)
                self.assertNotIn('private', output)
                if condition == 'bad_policy':
                    self.assertEqual(events, [])


if __name__ == '__main__':
    unittest.main()
