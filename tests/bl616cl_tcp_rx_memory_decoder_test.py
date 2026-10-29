from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from inspect_bl616cl_tcp_rx_memory import decode, FIELDS, FEATURE_VERSION, decode_physical, PHYSICAL_FIELDS


class MemoryDecoderTest(unittest.TestCase):
    def report(self, **changes):
        values = dict.fromkeys(FIELDS, 0)
        values.update(sequence_used=8192, live_flows=32, physical_used=1040,
                      physical_peak=2048, blocks=1, block_peak=2)
        values.update(changes)
        return b'U35D' + bytes((FEATURE_VERSION, 59, 1, 1)) + b''.join(
            values[key].to_bytes(4, 'little') for key in FIELDS) + bytes(4)

    def test_independent_charges_and_sequence(self):
        row = decode(self.report())
        self.assertEqual(row['sequence_used'], 8192)
        self.assertEqual(row['physical_used'], 1040)
        # Retired owner still holds a real allocation after its sequence release.
        row = decode(self.report(sequence_used=7936, live_flows=31, retired_flows=1,
                                 future_reserved=256, minimum_reserved=256))
        self.assertEqual(row['physical_used'], 1040)

    def test_invalid_accounting_rejected(self):
        for changes in ({'sequence_used':12289}, {'minimum_reserved':4097},
                        {'physical_used':2049}, {'physical_peak':20481},
                        {'blocks':3}, {'block_peak':129}, {'retired_flows':1}, {'faults':1}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                decode(self.report(**changes))

    def test_unknown_or_disabled_layout(self):
        for offset, value in ((4,0), (5,58), (6,0), (7,2)):
            report=bytearray(self.report());report[offset]=value
            with self.assertRaises(ValueError):decode(report)
        with self.assertRaises(ValueError):decode(self.report()[:-1])

    def test_physical_shares(self):
        values = dict.fromkeys(PHYSICAL_FIELDS, 0)
        values.update(physical_used=4096, physical_reserved=15872, blocks=16,
                      blocks_reserved=31, max_owner_charge=4096, max_owner_blocks=16,
                      owner_minimum=512, owner_cap=4096, owner_block_cap=16,
                      physical_limit=20480, block_limit=128)
        def report(changes):
            row=dict(values,**changes)
            return b'U35D'+bytes((FEATURE_VERSION,60,1,1))+b''.join(
                row[k].to_bytes(4,'little') for k in PHYSICAL_FIELDS)+bytes(8)
        self.assertEqual(decode_physical(report({}))['physical_reserved'],15872)
        self.assertEqual(decode_physical(report({'owner_minimum':256,
            'physical_reserved':7936}))['owner_minimum'],256)
        for changes in ({'physical_reserved':16385}, {'blocks_reserved':113},
                        {'max_owner_charge':4097}, {'max_owner_blocks':17},
                        {'owner_minimum':0}, {'faults':1}):
            with self.subTest(changes=changes),self.assertRaises(ValueError):
                decode_physical(report(changes))


if __name__ == '__main__':
    unittest.main()
