"""Check bounded firmware observations and reject inconsistent HID snapshots."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from inspect_bl616cl_controller_links import snapshot


class ControllerLinksTest(unittest.TestCase):
    def test_native_ring_and_interrupt_state(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp / 'sdk/mm').mkdir(parents=True)
            (temp / 'sdk/mm/mm.h').write_text('''#include <stdint.h>
extern unsigned masked;
static inline uintptr_t mm_lock_save() { unsigned old=masked; masked=1; return old; }
static inline void mm_unlock_restore(uintptr_t old) { masked=old; }
''')
            (temp / 'bflb_mtimer.h').write_text('''#include <stdint.h>
static inline uint64_t bflb_mtimer_get_time_ms() { return 0x100000005ULL; }
''')
            (temp / 'main.cpp').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "controller_link_diagnostic.h"
unsigned masked=0;
extern "C" void bridge_ble_link_observe(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
extern "C" void bridge_ble_adv_rx_observe(unsigned);
static unsigned word(const uint8_t *p) { return unsigned(p[0]) | (unsigned(p[1])<<8) | (unsigned(p[2])<<16) | (unsigned(p[3])<<24); }
int main() {
  bridge::ControllerLinkDiagnostic ring;
  uint8_t report[66]; memset(report, 0xa5, sizeof(report));
  ring.report(63, report+1);
  assert(word(report+9)==0 && word(report+13)==16);
  assert(report[0]==0xa5 && report[65]==0xa5 && report[5]==0xa5 && report[49]==0xa5);
  for(unsigned i=1;i<=20;i++) ring.note(i*10,1,0,1,192,i,0,0,100+i);
  unsigned seen=0;
  for(unsigned page=64;page<=79;page++) {
    ring.report(page,report+1); unsigned seq=word(report+9);
    assert(seq>=5 && seq<=20); seen|=1U<<(seq-5);
    assert(word(report+13)==seq*10 && word(report+45)==100+seq);
    assert(report[0]==0xa5 && report[65]==0xa5 && report[49]==0xa5);
  }
  assert(seen==65535);
  memset(report,0,sizeof(report));
  bridge::controller_link_diagnostic_report(63,report);
#if BL616CL_CONTROLLER_LINK_DIAGNOSTIC
  assert(report[6]==1 && report[7]==1);
  masked=1; bridge_ble_link_observe(1,9,0,1,192,0,0,0); assert(masked==1); masked=0;
  bridge_ble_link_observe(2,10,0,1,192,1,2,0); assert(masked==0);
  bridge::controller_link_diagnostic_report(64,report);
  assert(word(report+8)==1 && word(report+12)==5 && word(report+16)==1);
  bridge::controller_link_diagnostic_report(65,report);
  assert(word(report+8)==2 && word(report+16)==2 && word(report+32)==1);
  masked=1; bridge_ble_adv_rx_observe(0); assert(masked==1); masked=0;
  bridge_ble_adv_rx_observe(0); bridge_ble_adv_rx_observe(1);
  bridge_ble_adv_rx_observe(2); bridge_ble_adv_rx_observe(3);
  bridge_ble_adv_rx_observe(4); assert(masked==0);
  memset(report,0,sizeof(report));
  bridge::controller_link_diagnostic_report(80,report);
  assert(report[5]==80 && report[6]==1 && report[7]==1);
  assert(word(report+8)==2 && word(report+12)==1 && word(report+16)==1 && word(report+20)==1);
  assert(word(report+24)==0);
#else
  assert(report[6]==0 && report[7]==1 && word(report+8)==0);
#endif
}
''')
            for enabled in (0, 1):
                exe = temp / f'test-{enabled}'
                subprocess.run(['g++', '-std=c++11', '-fsanitize=address,undefined',
                                '-fno-omit-frame-pointer', '-g',
                                f'-DBL616CL_CONTROLLER_LINK_DIAGNOSTIC={enabled}',
                                '-I' + str(temp), '-I' + str(ROOT / 'UNOR4BL616CLBridge'),
                                str(temp / 'main.cpp'),
                                str(ROOT / 'UNOR4BL616CLBridge/controller_link_diagnostic.cpp'),
                                '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)
                symbols = subprocess.check_output(['nm', str(exe)], text=True)
                self.assertEqual(' T bridge_ble_link_observe' in symbols, bool(enabled))
                self.assertEqual(' T bridge_ble_adv_rx_observe' in symbols, bool(enabled))

    def pages(self, count=20):
        pages = {}
        for page in range(63, 80):
            words = [count, 16] + [0]*8 if page == 63 else [0]*10
            if page > 63:
                physical = page - 64
                seq = next(i for i in range(max(1,count-15),count+1) if (i-1)%16 == physical)
                words = [seq, seq*10, 1, 0, 1, 192, 0, 0, 0, seq*100]
            pages[page] = b'U35D' + bytes((35,page,1,1)) + b''.join(w.to_bytes(4,'little') for w in words) + bytes(16)
        return pages

    def test_decode_retains_last_sixteen_in_order(self):
        result = snapshot(self.pages().__getitem__)
        self.assertEqual([r['sequence'] for r in result['entries']], list(range(5,21)))
        self.assertEqual(result['total'], 20)

    def test_reject_changed_ring_and_old_firmware(self):
        pages = self.pages()
        for corrupted in (bytes(64), pages[64][:5]+bytes((0,))+pages[64][6:]):
            broken = dict(pages); broken[64] = corrupted
            with self.assertRaises(ValueError): snapshot(broken.__getitem__)
        header_reads = [0]
        def changing(page):
            if page == 63:
                header_reads[0] += 1
                if header_reads[0] > 1: return self.pages(21)[63]
            return pages[page]
        with self.assertRaisesRegex(ValueError, 'changed'): snapshot(changing)
        broken = dict(pages); broken[65] = broken[64][:5] + bytes((65,)) + broken[64][6:]
        with self.assertRaisesRegex(ValueError, 'sequence'): snapshot(broken.__getitem__)


if __name__ == '__main__':
    unittest.main()
