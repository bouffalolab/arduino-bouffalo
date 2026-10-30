from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from summarize_tcp_rx_budget import decode, summarize, FIELDS, LABELS


def value(**overrides):
    row=dict.fromkeys(FIELDS,0)
    row.update(pool_capacity=32,pool_used=24,active=24,established=24,
               cap_total=49152,accept_total=48000,window_debit_total=1152,
               announced_outstanding_total=40000,min_accept=1800,max_accept=2048,
               max_announced_outstanding=2048,heap_capacity=40960,heap_used=12000,heap_peak=24000)
    row.update(overrides)
    return '1,1,'+','.join(str(row[f]) for f in FIELDS)


def line(r,label,raw=None):
    return f'RA4_TLS_RESOURCE:TCP_RX_BUDGET={r},{label},{raw or value()}\n'


class DecoderTest(unittest.TestCase):
    def test_complete_lifecycle(self):
        text=''.join(line(r,label) for r in (1,2) for label in LABELS)
        rows=summarize(text)
        self.assertEqual(len(rows),8)
        self.assertEqual(rows[0]['announced_outstanding_total'],40000)
        # Max is a lifetime native peak; it need not decrease when active credit falls.
        self.assertEqual(rows[-1]['heap_peak'],24000)
        idle=dict.fromkeys(FIELDS,0)
        idle.update(pool_capacity=32,pool_used=2,timewait=2,heap_capacity=40960,heap_peak=24000)
        raw='1,1,'+','.join(str(idle[f]) for f in FIELDS)
        self.assertEqual(decode(raw)['active'],0)

    def test_invalid_snapshot(self):
        bad=['1,0','2,1,'+value()[4:],value()+',0',value().replace('49152','-1'),
             value(pool_used=33),value(pool_used=23),value(established=23),
             value(window_debit_total=0),value(announced_over_accept=1),
             value(announced_outstanding_total=48001),value(min_accept=2049),
             value(max_announced_outstanding=2049),value(zero_windows=25),
             value(heap_used=24001),value(heap_peak=40961),value(heap_capacity=0),
             value(active=0,established=0,pool_used=0),value(cap_total=2**32)]
        for raw in bad:
            with self.subTest(raw=raw),self.assertRaises(ValueError):decode(raw)

    def test_missing_duplicate_out_of_order_failed(self):
        good=''.join(line(1,label) for label in LABELS)
        for text in (good.replace(line(1,'AFTER_CLOSE'),''),good+line(1,'AFTER_CLOSE'),
                     ''.join(line(1,label) for label in reversed(LABELS)),
                     good.replace('AFTER_RX','FAILED_RX'),good.replace('=1,','=2,'),
                     good+line(3,'BEFORE_OPEN')):
            with self.subTest(text=text),self.assertRaises(ValueError):summarize(text)

if __name__=='__main__':unittest.main()
