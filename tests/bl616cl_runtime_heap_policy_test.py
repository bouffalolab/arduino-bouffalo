"""Exercise the patched SDK allocator against failure and capability boundaries."""
from pathlib import Path
import json
import hashlib
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches'

with tempfile.TemporaryDirectory(prefix='bl616cl-heap-policy-') as tmp:
    work = Path(tmp)
    source = work / 'components/mm/mm.c'
    source.parent.mkdir(parents=True)
    source.write_bytes((ROOT / 'third_party/bouffalo_sdk/components/mm/mm.c').read_bytes())
    for record in json.loads((PATCHES / 'manifest.json').read_text())['patches']:
        target = next((t for t in record['targets'] if t['path'] == 'components/mm/mm.c'), None)
        if target is None:
            continue
        assert hashlib.sha256(source.read_bytes()).hexdigest() == target['before_sha256']
        parts = (PATCHES / record['path']).read_text().split('--- a/')
        part = next(p for p in parts[1:] if p.startswith('components/mm/mm.c\n'))
        subprocess.run(['patch', '--batch', '-p1'], input=('--- a/' + part).encode(), cwd=work,
                       check=True, stdout=subprocess.DEVNULL)
        assert hashlib.sha256(source.read_bytes()).hexdigest() == target['after_sha256']
    (work / 'bflb_core.h').write_text('''#include <stdint.h>
static inline uintptr_t bflb_irq_save(void) { return 0; }
static inline void bflb_irq_restore(uintptr_t x) { (void)x; }
''')
    (work / 'log.h').write_text('''#define LOG_E(...) ((void)0)
#define LOG_D(...) ((void)0)
#define LOG_W(...) ((void)0)
''')
    (work / 'bflb_l1c.h').write_text('#define BFLB_CACHE_LINE_SIZE 32\n')
    (work / 'test.cpp').write_text(r'''
#include <cassert>
#include <cstring>
#include <limits>
extern "C" {
#include "mm.h"
}
#include "runtime_heap_policy.h"
static unsigned calls, tries, fail_heap, last_flags, active = 1;
static unsigned attempted[8];
static size_t available[4] = {0,0,24000,40000};
static unsigned char buffers[4][65536];
static mm_heap_t heaps[4];
#ifndef OMIT_POLICY
extern "C" int fhost_bridge_heap_policy_active() { return active; }
extern "C" uint32_t fhost_bridge_heap_preference(size_t n,uint32_t,size_t o,size_t w) {
    ++calls; return bridge::preferred_runtime_heap(n,o,w);
}
#endif
extern "C" void fhost_bridge_ap_start_allocator_event(uint8_t,uint32_t,uint32_t,uint32_t) {}
static void *allocate(mm_heap_t *h,size_t n,uint32_t flags) {
    attempted[tries++] = h->heap_id; last_flags=flags;
    if (h->heap_id == fail_heap || n > available[h->heap_id]) return nullptr;
    if (flags & MM_FLAG_PROP_ZERO) memset(buffers[h->heap_id],0,n);
    return buffers[h->heap_id];
}
static size_t free_size(mm_heap_t *h) { return available[h->heap_id]; }
static void *reallocate(mm_heap_t *h,void *,size_t) { attempted[tries++]=h->heap_id; return nullptr; }
static mm_allocator_t allocator;
static void reset() { calls=tries=fail_heap=last_flags=0; active=1;
    available[2]=24000;available[3]=40000;heaps[2].is_active=heaps[3].is_active=true;
    g_mem_manager.heap_alloc_order[0]=2;g_mem_manager.heap_alloc_order[1]=3;
}
int main() {
    allocator.malloc=allocate;allocator.get_free_size=free_size;allocator.realloc=reallocate;
    g_mem_manager.initialized=true;g_mem_manager.allocators[1]=&allocator;
    for (unsigned id=2;id<=3;++id) {
        heaps[id].heap_id=id;heaps[id].allocator_id=1;
        heaps[id].start_addr=buffers[id];heaps[id].end_addr=buffers[id]+sizeof buffers[id];
        g_mem_manager.heaps[id]=&heaps[id];
    }
#ifdef OMIT_POLICY
    reset();assert(kmalloc(4096,0)==buffers[2] && attempted[0]==2);
    reset();fail_heap=2;assert(kmalloc(4096,0)==buffers[3] && tries==2);
    return 0;
#endif
    reset();assert(kmalloc(256,0)==buffers[2] && attempted[0]==2);
    reset();assert(kmalloc(4096,0)==buffers[3] && attempted[0]==3);
    reset();available[3]=13000;assert(kmalloc(4096,0)==buffers[2]);
    reset();available[2]=8700;assert(kmalloc(256,0)==buffers[3]);
    reset();fail_heap=3;assert(kmalloc(4096,0)==buffers[2] && tries==2 && attempted[0]==3 && attempted[1]==2);
    reset();available[2]=available[3]=0;assert(!kmalloc(4096,0) && tries==2);
    reset();active=0;assert(kmalloc(4096,0)==buffers[2] && calls==0);
    reset();assert(kmalloc(4096,MM_FLAG_HEAP_OCRAM_0)==buffers[2] && calls==0);
    reset();fail_heap=3;assert(!kmalloc(4096,MM_FLAG_HEAP_WRAM_0) && tries==1 && calls==0);
    reset();assert(kmalloc(4096,MM_FLAG_PROP_DMA)==buffers[2] && calls==0);
    reset();const uint32_t flags=MM_FLAG_ALIGN_256BYTE|MM_FLAG_PROP_ZERO|MM_FLAG_PROP_NOWAIT;
    memset(buffers[3],0xa5,4096);assert(kmalloc(4096,flags)==buffers[3] && last_flags==flags && buffers[3][0]==0);
    reset();g_mem_manager.heap_alloc_order[1]=0;assert(kmalloc(4096,0)==buffers[2] && calls==0);
    reset();heaps[3].is_active=false;assert(kmalloc(4096,0)==buffers[2] && calls==0);
    reset();assert(!kmalloc(0,0) && calls==0 && tries==0);
    reset();buffers[2][0]=0xab;assert(!krealloc(buffers[2],100000) && calls==0 && attempted[0]==2 && buffers[2][0]==0xab);
    reset();assert(krealloc(nullptr,4096)==buffers[3] && calls==1);
    assert(bridge::preferred_runtime_heap(std::numeric_limits<size_t>::max(),24000,40000)==0);
    assert(g_mem_manager.heap_alloc_order[0]==2 && g_mem_manager.heap_alloc_order[1]==3);
}
''')
    includes = ['-I'+str(work), '-I'+str(ROOT/'third_party/bouffalo_sdk/components/mm'),
                '-DIS_ENABLED(x)=x', '-DCONFIG_MM_ENABLE_STATISTICS=0', '-DCONFIG_MM_ENABLE_MIN_FREE_TRACKING=0']
    subprocess.run(['gcc','-w','-ffunction-sections','-fdata-sections',*includes,'-c',str(source),'-o',str(work/'mm.o')],check=True)
    subprocess.run(['g++','-std=c++17',*includes,'-I'+str(ROOT/'UNOR4BL616CLBridge'),str(work/'test.cpp'),str(work/'mm.o'),'-Wl,--gc-sections','-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
    subprocess.run(['g++','-std=c++17','-DOMIT_POLICY',*includes,'-I'+str(ROOT/'UNOR4BL616CLBridge'),str(work/'test.cpp'),str(work/'mm.o'),'-Wl,--gc-sections','-o',str(work/'no-policy')],check=True)
    subprocess.run([str(work/'no-policy')],check=True)
print('BL616CL_RUNTIME_HEAP_POLICY_ALLOCATOR_PASS')
