"""Exercise production passive completion parsing/reporting with SDK headroom."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseTest(unittest.TestCase):
    def test_native_offset_and_report(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'lwip').mkdir()
            (folder / 'sdk/mm').mkdir(parents=True)
            (folder / 'lwip/pbuf.h').write_text('''#include <stdint.h>
#define PBUF_LINK_ENCAPSULATION_HLEN 388
struct pbuf { uint16_t tot_len; uint8_t *payload; };
uint16_t pbuf_copy_partial(const struct pbuf*,void*,uint16_t,uint16_t);
''')
            (folder / 'sdk/mm/mm.h').write_text('''#include <stdint.h>
extern unsigned locks;
static inline uintptr_t mm_lock_save() { ++locks; return 123; }
static inline void mm_unlock_restore(uintptr_t f) { if(f!=123||!locks)__builtin_trap();--locks; }
''')
            source = folder / 'test.cpp'
            source.write_text(r'''
#include <cassert>
#include <cstring>
#include "sta_tcp_diagnostic.h"
extern "C" {
#include <lwip/pbuf.h>
}
unsigned locks;
extern "C" uint16_t pbuf_copy_partial(const pbuf *p,void *dest,uint16_t n,uint16_t off) {
 if(off>p->tot_len)return 0;
 if(n>p->tot_len-off)n=p->tot_len-off;
 memcpy(dest,p->payload+off,n);return n;
}
static void be16(uint8_t *p,unsigned v) { p[0]=v>>8;p[1]=v; }
static unsigned get(const uint8_t *p) { return p[0]|unsigned(p[1])<<8|unsigned(p[2])<<16|unsigned(p[3])<<24; }
int main() {
 uint8_t bytes[442]={}; uint8_t *eth=bytes+388;
 eth[12]=8;eth[14]=0x45;eth[17]=40;eth[23]=6;eth[29]=1;eth[33]=2;
 be16(eth+34,1234);be16(eth+36,18444);eth[41]=10;eth[46]=0x50;eth[47]=2;
 pbuf exposed={442,bytes},plain={54,eth};uint8_t report[64]={};
 bridge::sta_tcp_observe(true,true,&plain);
 eth[47]=16;eth[45]=99;
 bridge::sta_tcp_tx_release(&exposed,1,0);
 bridge::sta_tcp_tx_release(&plain,0,1);
 bridge::sta_tcp_tx_release(&plain,0,0);
 bridge::sta_tcp_diagnostic_report(58,report);
 assert(report[5]==58 && report[6]==1 && report[7]==1);
 assert(get(report+8)==1 && get(report+12)==1234);
 assert(get(report+16)==3 && get(report+20)==1 && get(report+24)==1);
 assert(get(report+28)==99 && get(report+32)==99 && get(report+36)==99 && get(report+40)==18444);
 // Different tuple, null and truncated frames cannot alter completion counters.
 eth[29]=3;bridge::sta_tcp_tx_release(&plain,1,0);eth[29]=1;
 bridge::sta_tcp_tx_release(nullptr,1,0);
 plain.tot_len=40;bridge::sta_tcp_tx_release(&plain,1,0);
 bridge::sta_tcp_diagnostic_report(58,report);assert(get(report+16)==3 && !locks);
}
''')
            binary = folder / 'test'
            subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DBL616CL_STA_TCP_DIAGNOSTIC=1',
                            '-DBL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT=18444',
                            '-I'+str(folder), '-I'+str(ROOT/'UNOR4BL616CLBridge'), str(source),
                            str(ROOT/'UNOR4BL616CLBridge/sta_tcp_diagnostic.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
