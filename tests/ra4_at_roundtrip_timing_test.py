import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from run_ra4_at_roundtrip_timing import summarize


class TimingTest(unittest.TestCase):
    def capture(self):
        return '\n'.join(f'RA4_AT_TIMING:SAMPLE={m},{c},{i},10,{100 if m == 0 else 0},200,{4 if m == 0 else 0},1'
                         for m in range(2) for c in range(3) for i in range(20)) + '\nRA4_AT_TIMING:RESULT=PASS\n'

    def test_complete_numeric_summary(self):
        result = summarize(self.capture())
        self.assertEqual(len(result), 6)
        self.assertEqual(result[0]['first_us']['median'], 100)
        self.assertNotIn('first_us', result[3])

    def test_scan_off_on_off_requires_all_phases_and_raw_boundaries(self):
        output = '\n'.join(f'RA4_AT_TIMING:SAMPLE={m},{c},{i},10,100,200,7,1'
                           for m in range(3) for c in range(3) for i in range(20)) + '\nRA4_AT_TIMING:RESULT=PASS\n'
        result = summarize(output, scan_window=True)
        self.assertEqual(len(result), 9)
        self.assertEqual(result[-1]['mode'], 'scan-off-after')
        for changed in (output.replace('SAMPLE=2,0,0', 'SAMPLE=1,0,0'),
                        output.replace(',100,200,7', ',0,200,7', 1)):
            with self.assertRaises(RuntimeError):
                summarize(changed, scan_window=True)

    def test_missing_duplicate_failed_invalid_and_noncausal_samples_rejected(self):
        source = self.capture()
        for changed in (source.replace('SAMPLE=0,0,0', 'SAMPLE=0,0,1'),
                        source.replace(',200,4,1', ',200,4,0', 1),
                        source.replace(',100,200,4', ',300,200,4', 1),
                        source.replace(',100,200,4', ',100,0,4', 1),
                        source.replace('SAMPLE=0,0,0', 'SAMPLE=2,0,0'),
                        source.replace('RESULT=PASS', 'RESULT=FAIL'),
                        source + '\nRA4_AT_TIMING:RESULT=FAIL\n',
                        '\n'.join(source.splitlines()[1:])):
            with self.assertRaises(RuntimeError):
                summarize(changed)


if __name__ == '__main__':
    unittest.main()
