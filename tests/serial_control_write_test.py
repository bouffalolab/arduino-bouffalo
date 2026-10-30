"""Control writes retain failure, never resend, and avoid unbounded tcdrain."""
import contextlib
import io
import os
from pathlib import Path
import pty
import sys
import unittest
from unittest.mock import Mock, PropertyMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import serial
from serial_control_write import write_control


class ControlWriteTest(unittest.TestCase):
    def test_real_posix_serial_delivers_command_without_tcdrain(self):
        master, slave = pty.openpty()
        try:
            with serial.Serial(os.ttyname(slave), 115200, write_timeout=1) as link:
                with patch.object(link, 'flush', side_effect=AssertionError('unbounded drain')):
                    write_control(link, b'@PREPSTATUS 0123456789abcdef\n', phase='pty')
                self.assertEqual(os.read(master, 128), b'@PREPSTATUS 0123456789abcdef\n')
        finally:
            os.close(master)
            os.close(slave)

    def test_write_failure_or_short_write_never_retries_or_drains(self):
        for failure in (serial.SerialTimeoutException('original'), OSError('original'), 2):
            link = Mock()
            if isinstance(failure, Exception):
                link.write.side_effect = failure
            else:
                link.write.return_value = failure
            pending = PropertyMock(return_value=0)
            type(link).out_waiting = pending
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                with self.assertRaises((serial.SerialTimeoutException, OSError)) as caught:
                    write_control(link, b'private-payload', phase='start')
            if isinstance(failure, Exception):
                self.assertIs(caught.exception, failure)
            link.write.assert_called_once_with(b'private-payload')
            pending.assert_not_called()
            link.flush.assert_not_called()
            self.assertNotIn('private-payload', output.getvalue())
            self.assertIn('SERIAL_CONTROL_DELIVERY_UNKNOWN=start', output.getvalue())

    def test_pyserial_timeout_can_follow_complete_kernel_acceptance(self):
        # Execute installed POSIX pySerial: write reaches the PTY, then a
        # simulated readiness timeout raises. Resending would duplicate START.
        master, slave = pty.openpty()
        try:
            with serial.Serial(os.ttyname(slave), 115200, write_timeout=1) as link:
                with patch('serial.serialposix.select.select', return_value=([], [], [])):
                    with self.assertRaises(serial.SerialTimeoutException):
                        write_control(link, b'START once\n', phase='pty-timeout')
                self.assertEqual(os.read(master, 128), b'START once\n')
        finally:
            os.close(master)
            os.close(slave)

    def test_drain_deadline_and_error_remain_failures(self):
        for pending in (PropertyMock(return_value=1), PropertyMock(side_effect=OSError('gone'))):
            link = Mock()
            link.write.return_value = 7
            type(link).out_waiting = pending
            with self.assertRaises((serial.SerialTimeoutException, OSError)):
                write_control(link, b'command', phase='start', drain_timeout=0.02)
            link.write.assert_called_once()
            link.flush.assert_not_called()

    def test_queue_drains_and_invalid_timeout_sends_nothing(self):
        link = Mock()
        link.write.return_value = 7
        type(link).out_waiting = PropertyMock(side_effect=[4, 2, 0])
        write_control(link, b'command', phase='start')
        for timeout in (0, -1, float('inf'), float('nan')):
            with self.assertRaises(ValueError):
                write_control(link, b'command', phase='start', drain_timeout=timeout)
        link.write.assert_called_once()


if __name__ == '__main__':
    unittest.main()
