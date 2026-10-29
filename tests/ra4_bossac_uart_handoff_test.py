"""USB write completion must not stand in for delivery through the UART queue."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import run_ra4_bossac_final_carrier as uploader
from run_ra4_bossac_direct import DirectSerialProxy

class HandoffTest(unittest.TestCase):
    def test_accepted_usb_bytes_wait_for_uart_and_handle_counter_wrap(self):
        for start in (100,0xfffffffe):
            with patch.object(uploader,'uart_tx_snapshot',side_effect=[(start,0,0),((start+1)&0xffffffff,0,0),((start+2)&0xffffffff,0,0)]),patch.object(uploader.time,'sleep') as sleep:
                uploader.wait_uart_forwarded((start,0,0),2)
                self.assertEqual(sleep.call_count,2)
    def test_stalled_uart_is_not_success(self):
        with patch.object(uploader,'uart_tx_snapshot',return_value=(100,0,0)),patch.object(uploader.time,'monotonic',side_effect=(0,0,3)),patch.object(uploader.time,'sleep'):
            with self.assertRaisesRegex(RuntimeError,'still pending'):uploader.wait_uart_forwarded((100,0,0),2)
    def test_drop_failure_and_concurrent_writer_are_rejected(self):
        for report in ((102,1,0),(102,0,1),(103,0,0)):
            with patch.object(uploader,'uart_tx_snapshot',return_value=report):
                with self.assertRaises(RuntimeError):uploader.wait_uart_forwarded((100,0,0),2)
    def test_all_wire_bytes_count_including_binary_and_literal_k(self):
        class Link:
            def write(self,data):return len(data)
            def flush(self):pass
        proxy=DirectSerialProxy(-1,Link(),64,0)
        raw=b'\x80N#S20000000,00000002#K#K#'
        proxy.tx_buffer.extend(raw);proxy.flush_tx()
        self.assertEqual(proxy.forwarded_bytes,len(raw));self.assertEqual(proxy.pending_bytes,0)
        proxy.link.write=lambda data:0
        proxy.tx_buffer.extend(b'K#')
        with self.assertRaises(Exception):proxy.flush_tx()
        self.assertEqual(proxy.forwarded_bytes,len(raw))
if __name__=='__main__':unittest.main()
