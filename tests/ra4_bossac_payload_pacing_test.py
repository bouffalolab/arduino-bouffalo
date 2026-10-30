"""SAM-BA pacing must survive arbitrary PTY read fragmentation."""
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_ra4_bossac_direct as proxy_module


class PacingTest(unittest.TestCase):
    def exercise(self, fragments, elapsed_between_reads=0):
        now = [0.0]
        writes = []
        commands = []

        def sleep(seconds):
            now[0] += seconds

        class Link:
            def write(self, data):
                writes.append((now[0], bytes(data)))
                return len(data)

            def flush(self):
                pass

        clock = types.SimpleNamespace(monotonic=lambda: now[0], sleep=sleep)
        with patch.object(proxy_module, "time", clock):
            proxy = proxy_module.DirectSerialProxy(-1, Link(), 64, .01, commands.append)
            for fragment in fragments:
                proxy.tx_buffer.extend(fragment)
                proxy.flush_tx()
                now[0] += elapsed_between_reads
        return writes, commands, proxy.pending_bytes

    def test_fragmented_payload_obeys_same_minimum_spacing_as_single_read(self):
        command = b"S20000000,000000C0#"
        data = bytes(range(192))
        for fragments in [[command + data + b"K#"],
                          [command, data[:64], data[64:128], data[128:], b"K#"],
                          [command[:7], command[7:] + data[:32], data[32:64],
                           data[64:128], data[128:] + b"K#"]]:
            writes, commands, pending = self.exercise(fragments)
            self.assertEqual(b"".join(value for _, value in writes), command + data + b"K#")
            self.assertEqual(commands, [command, b"K#"])
            payload_writes = [(stamp, value) for stamp, value in writes if value not in commands]
            self.assertTrue(all(len(value) <= 64 for _, value in payload_writes))
            for (before, _), (after, _) in zip(payload_writes, payload_writes[1:]):
                self.assertGreaterEqual(after - before + 1e-12, .01)
            self.assertEqual(pending, 0)

    def test_elapsed_read_delay_counts_without_adding_another_full_delay(self):
        command = b"S20000000,00000080#"
        writes, _, pending = self.exercise([command, b"a" * 64, b"b" * 64], .02)
        self.assertAlmostEqual(writes[-1][0] - writes[-2][0], .02)
        self.assertEqual(pending, 0)

    def test_fragmented_4k_staging_write_respects_512_byte_uart_queue(self):
        now = [0.0]
        queue = [0.0]
        last = [0.0]
        maximum = [0.0]

        def sleep(seconds):
            now[0] += seconds

        class Link:
            def write(self, data):
                # USB flush completion does not mean the 230400 baud UART
                # has drained. Model its independent, bounded bridge queue.
                queue[0] = max(0, queue[0] - (now[0] - last[0]) * 230400 / 10)
                queue[0] += len(data)
                last[0] = now[0]
                maximum[0] = max(maximum[0], queue[0])
                if queue[0] > 512:
                    raise BufferError("bridge UART queue overflow")
                return len(data)

            def flush(self):
                pass

        with patch.object(proxy_module, "time", types.SimpleNamespace(
                monotonic=lambda: now[0], sleep=sleep)):
            proxy = proxy_module.DirectSerialProxy(-1, Link(), 64, .01)
            proxy.tx_buffer.extend(b"S20000000,00001000#")
            proxy.flush_tx()
            for _ in range(64):
                proxy.tx_buffer.extend(b"x" * 64)
                proxy.flush_tx()
        self.assertLessEqual(maximum[0], 512)
        self.assertEqual(proxy.pending_bytes, 0)


if __name__ == "__main__":
    unittest.main()
