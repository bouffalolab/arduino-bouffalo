"""Exercise the actual IRQ-safe callback/report ABI with a host MM lock fake."""
import pathlib
import json
import hashlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class MacRxDiagnosticTest(unittest.TestCase):
    def test_passive_source_patch_chain(self):
        patchdir = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches'
        records = json.loads((patchdir / 'manifest.json').read_text())['patches']
        paths = {t['path'] for r in records if r['id'].startswith(('0072-', '0073-', '0074-')) for t in r['targets']}
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            for name in paths:
                target = temp / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / 'third_party/bouffalo_sdk' / name).read_bytes())
            for record in records:
                selected = [t for t in record['targets'] if t['path'] in paths]
                if not selected:
                    continue
                sections = (patchdir / record['path']).read_text().split('--- a/')[1:]
                for target in selected:
                    name = target['path']
                    path = temp / name
                    self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), target['before_sha256'])
                    section = next('--- a/' + s for s in sections if s.startswith(name + '\n'))
                    # Diagnostic patches retain all existing statements except the
                    # queue calls whose original expression must still be evaluated once.
                    if record['id'].startswith(('0072-', '0073-', '0074-')):
                        removed = [line[1:] for line in section.splitlines() if line.startswith('-') and not line.startswith('---')]
                        if record['id'].startswith('0074-') and name.endswith('macif_fhost.c'):
                            self.assertEqual(len(removed), 2)
                            self.assertIn('rtos_queue_read(fhost_rx_env.queue_buf, &buf, 0, false)', section)
                            self.assertIn('rtos_queue_is_empty(fhost_rx_env.queue_buf)', section)
                        else:
                            self.assertTrue(all(line.strip() in ('if (!(statinfo & RX_HD_SUCCESS))', 'break;') for line in removed))
                    subprocess.run(['patch','--batch','-p1'], cwd=temp, input=section, text=True, check=True, stdout=subprocess.DEVNULL)
                    self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), target['after_sha256'])
            self.assertIn('BRIDGE_RX_EVENT(17, tid, 0, 0)', (temp / 'components/wireless/macsw/ip/umac/src/rxu/rxu_cntrl.c').read_text())

    def test_callback_and_report(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            header = temp / 'sdk/mm/mm.h'
            header.parent.mkdir(parents=True)
            header.write_text('''#include <stdint.h>
extern unsigned locks;
static inline uintptr_t mm_lock_save(void) { ++locks; return 123; }
static inline void mm_unlock_restore(uintptr_t f) { if(f != 123 || !locks) __builtin_trap(); --locks; }
''')
            source = temp / 'test.cpp'
            source.write_text('''#include <cassert>
#include <cstring>
#include "mac_rx_diagnostic.h"
unsigned locks;
extern "C" void fhost_bridge_rx_event(unsigned,unsigned,unsigned,unsigned);
extern "C" void bridge_udp_mac_fragment_observe(const void *,unsigned,unsigned);
unsigned get(const unsigned char *p,unsigned o) { return p[o]|unsigned(p[o+1])<<8|unsigned(p[o+2])<<16|unsigned(p[o+3])<<24; }
int main() {
 unsigned char r[64] = {};
 fhost_bridge_rx_event(2,0,7,8);
 fhost_bridge_rx_event(2,1,9,10);
 fhost_bridge_rx_event(14,512,12,0);
 fhost_bridge_rx_event(16,1,0,1);
 fhost_bridge_rx_event(17,6,0,0);
 fhost_bridge_rx_event(18,1,123,6);
 fhost_bridge_rx_event(99,0,0,0);
 bridge::mac_rx_diagnostic_report(48,r);
 assert(r[5]==48 && r[6]==1 && get(r,16)==2);
 memset(r,0,64); bridge::mac_rx_diagnostic_report(49,r);
 assert(r[7]==2 && get(r,8)==1 && get(r,12)==1 && get(r,16)==1);
 assert(get(r,20)==1 && get(r,24)==1 && get(r,28)==1);
 assert(get(r,32)==1 && get(r,36)==0 && get(r,40)==18);
 assert(get(r,44)==1 && get(r,48)==123 && get(r,52)==6 && !locks);
 fhost_bridge_rx_event(23,0,0,0);
 fhost_bridge_rx_event(23,0,0,0);
 fhost_bridge_rx_event(22,0,0,0);
 fhost_bridge_rx_event(22,1,0,0);
 fhost_bridge_rx_event(24,0,0,0);
 memset(r,0,64); bridge::mac_rx_diagnostic_report(50,r);
 assert(get(r,8)==2 && get(r,12)==2 && get(r,16)==1);
 assert(get(r,20)==1 && get(r,24)==0 && !locks);
 unsigned char llc[28] = {0xaa,0xaa,3,0,0,0,8,0,0x45};
 llc[17]=17; llc[13]=7;
 bridge_udp_mac_fragment_observe(nullptr,28,1);
 bridge_udp_mac_fragment_observe(llc,27,1);
 bridge_udp_mac_fragment_observe(llc,28,1); // unfragmented
 memset(r,0,64); bridge::mac_rx_diagnostic_report(82,r);
 assert(r[5]==82 && r[7]==1 && get(r,8)==0);
 for(unsigned i=0;i<12;++i) {
  unsigned f=i*185 | (i<11 ? 0x2000 : 0);
  llc[14]=f>>8; llc[15]=f;
  bridge_udp_mac_fragment_observe(llc,28,100+i);
 }
 memset(r,0,64); bridge::mac_rx_diagnostic_report(82,r);
 assert(get(r,8)==12 && get(r,12)==7 && get(r,16)==12 && get(r,20)==4095 && get(r,24)==111);
 bridge_udp_mac_fragment_observe(llc,28,111); // duplicate is counted, not a new mask bit
 memset(r,0,64); bridge::mac_rx_diagnostic_report(82,r);
 assert(get(r,8)==13 && get(r,16)==13 && get(r,20)==4095);
 llc[13]=8; llc[14]=0x20; llc[15]=0;
 bridge_udp_mac_fragment_observe(llc,28,200);
 memset(r,0,64); bridge::mac_rx_diagnostic_report(82,r);
 assert(get(r,8)==14 && get(r,12)==8 && get(r,16)==1 && get(r,20)==1 && !locks);
}
''')
            binary = temp / 'test'
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',
                '-DBL616CL_MAC_RX_DIAGNOSTIC=1', '-I'+str(temp),
                '-I'+str(ROOT/'UNOR4BL616CLBridge'), str(source),
                str(ROOT/'UNOR4BL616CLBridge/mac_rx_diagnostic.cpp'), '-o',str(binary)],check=True)
            subprocess.run([str(binary)], check=True)
            obj = temp / 'off.o'
            subprocess.run(['g++','-std=c++11','-c',str(ROOT/'UNOR4BL616CLBridge/mac_rx_diagnostic.cpp'),'-o',str(obj)], check=True)
            symbols = subprocess.check_output(['nm','-C',str(obj)],text=True)
            self.assertNotIn('fhost_bridge_rx_event', symbols)
            self.assertNotIn('counts', symbols)

if __name__ == '__main__':
    unittest.main()
