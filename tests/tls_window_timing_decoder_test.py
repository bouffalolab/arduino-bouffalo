import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from summarize_tls_window_timing import summarize, decode

class DecoderTest(unittest.TestCase):
    def sample(self, count):
        group=[count,count,21000*count,37*count,22000*count,21000 if count else 0,37 if count else 0,22000 if count else 0]
        return ','.join(map(str,[1,1,0]+group+[0]*8+group))
    def output(self):
        return '\n'.join('RA4_TLS_RESOURCE:TLS_WINDOW_TIMING=1,'+label+','+self.sample(n)
                         for label,n in [('BEFORE_RX',1),('AFTER_RX',4)])
    def test_interval_uses_deltas_and_lifetime_max_is_named(self):
        result=summarize(self.output())
        self.assertEqual(result[0]['count'],3)
        self.assertEqual(result[0]['pause_mean_us'],21000)
        self.assertEqual(result[0]['lifetime_max_us']['pause'],21000)
        self.assertIsNone(result[1]['body_mean_us'])
    def test_incomplete_failed_duplicate_or_decreasing_intervals_fail(self):
        source=self.output()
        for changed in (source.splitlines()[0],source+'\n'+source,source.replace('AFTER_RX','FAILED_RX'),
                        source.replace(self.sample(4),self.sample(0)),source.replace('=1,','=2,')):
            with self.assertRaises(ValueError):summarize(changed)
    def test_disabled_overflow_unknown_malformed_fail(self):
        for value in ('1,0',self.sample(1).replace('1,1,0,','1,1,1,',1),
                      self.sample(1).replace('1,1,0,','2,1,0,',1),self.sample(1)+',0','-1,'+self.sample(1)):
            with self.assertRaises(ValueError):decode(value)

if __name__=='__main__':unittest.main()
