"""Run the patched native allocator with failure observer present and absent."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT/'tools/bridge_runtime/profiles/bridge/fhost_patches'

class AllocationFailureTest(unittest.TestCase):
    def test_native_allocator(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            relative = Path('components/net/lwip/lwip/src/core/mem.c')
            target = temp/relative
            target.parent.mkdir(parents=True)
            (temp/relative.with_name('pbuf.c')).write_bytes((ROOT/'third_party/bouffalo_sdk'/relative.with_name('pbuf.c')).read_bytes())
            target.write_bytes((ROOT/'third_party/bouffalo_sdk'/relative).read_bytes())
            subprocess.run(['patch', '-s', '-p1', '-i', str(PATCHES/'0076-bridge-lwip-allocation-failure-diagnostic.patch')], cwd=temp, check=True)
            subprocess.run(['patch', '-s', '-p1', '-i', str(PATCHES/'0078-bridge-lwip-atomic-rx-reserve.patch')], cwd=temp, check=True)
            subprocess.run(['patch', '-s', '-p1', '-i', str(PATCHES/'0079-bridge-lwip-memory-lifecycle.patch')], cwd=temp, check=True)
            owner_patch = (PATCHES/'0083-bridge-tcp-rx-memory-owner.patch').read_text()
            mem_patch = owner_patch.split('\n--- a/components/net/lwip/lwip/src/core/tcp_in.c')[0] + '\n'
            subprocess.run(['patch', '-s', '-p1'], input=mem_patch, text=True, cwd=temp, check=True)
            text = target.read_text()
            start = text.index('/* Optional failure-only observer.')
            end = text.index('\n#endif /* MEM_USE_POOLS */', start)
            from bl616cl_pbuf_release_lifecycle_test import function
            native = '\nextern void bridge_lwip_mem_lifecycle(void *, unsigned, unsigned) __attribute__((weak));\n'
            native += '\nstatic void\n' + function(text, 'plug_holes(struct mem *mem)')
            native += '\nstatic int\n' + function(text, 'mem_link_valid(struct mem *mem)')
            native += '\nunsigned\n' + function(text, 'bridge_lwip_mem_charge(const void *allocation)')
            native += '\nvoid\n' + function(text[text.index('plug_holes(struct mem *mem)'):], 'mem_free(void *rmem)')
            native += '\nvoid *\n' + function(text[text.index('plug_holes(struct mem *mem)'):], 'mem_trim(void *rmem, mem_size_t new_size)')
            native += '\n' + text[start:end]
            pbuf_text = (temp/relative.with_name('pbuf.c')).read_text()
            native += '\n' + r'''
struct pbuf { unsigned len, tot_len, type; };
#define SIZEOF_STRUCT_PBUF sizeof(struct pbuf)
#define LWIP_MEM_ALIGN(p) ((void *)(((uintptr_t)(p)+3U)&~(uintptr_t)3U))
#define PBUF_RAM 3
static void pbuf_init_alloced_pbuf(struct pbuf *p, void *payload,
    unsigned total, unsigned length, unsigned type, unsigned flags) {
  assert(payload==(void *)(p+1) && !flags);
  p->len=length;p->tot_len=total;p->type=type;
}
'''
            native += '\nstruct pbuf *\n' + function(pbuf_text, 'bridge_pbuf_alloc_reserved(u16_t length, unsigned reserve, int *denied)')

            harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef unsigned mem_size_t; typedef uintptr_t mem_ptr_t;
typedef uint8_t u8_t; typedef uint16_t u16_t; typedef int16_t s16_t;
struct mem { unsigned next, prev, used; };
#define MEM_SIZE_ALIGNED 1024U
#define SIZEOF_STRUCT_MEM 12U
#define MIN_SIZE_ALIGNED 12U
#define MEM_ALIGNMENT 4U
#define LWIP_MEM_ALIGN_SIZE(s) (((s)+3U)&~3U)
#define MEM_SANITY_OFFSET 0
#define MEM_SANITY_OVERHEAD 0
#define MEM_STATS_INC_LOCKED(x) (++lwip_stats.mem.x)
#define MEM_STATS_DEC_USED(x,n) (lwip_stats.mem.x-=(n))
#define MEM_ILLEGAL_CHECK 0
#define LWIP_MEM_ILLEGAL_FREE(m) assert(0)
#define LWIP_MEM_FREE_DECL_PROTECT()
#define LWIP_MEM_FREE_PROTECT() LWIP_MEM_ALLOC_PROTECT()
#define LWIP_MEM_FREE_UNPROTECT() LWIP_MEM_ALLOC_UNPROTECT()
#define ram storage.bytes
#define LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT 0
#define MEM_OVERFLOW_CHECK 0
#define LWIP_STATS 1
#define MEM_STATS 1
#define LWIP_ASSERT(m,c) assert(c)
#define MEM_SANITY()
#define LWIP_DEBUGF(a,b)
#define LWIP_MEM_ALLOC_DECL_PROTECT()
#define LWIP_MEM_ALLOC_PROTECT() do { assert(!protected); protected=1; } while(0)
#define LWIP_MEM_ALLOC_UNPROTECT() do { assert(protected); protected=0; } while(0)
#define MEM_STATS_INC(x) (++lwip_stats.mem.x)
#define MEM_STATS_INC_USED(x,n) (lwip_stats.mem.x+=(n))
static unsigned locks, protected, mem_mutex;
static union { max_align_t align; uint8_t bytes[1040]; } storage;
static struct mem *lfree, *ram_end;
static struct { struct { unsigned used,err,illegal; } mem; } lwip_stats;
static struct mem *ptr_to_mem(unsigned n) { return (struct mem *)(storage.bytes+n); }
static unsigned mem_to_ptr(struct mem *m) { return (uint8_t *)m-storage.bytes; }
static void sys_mutex_lock(unsigned *m) { (void)m; assert(!locks); locks=1; }
static void sys_mutex_unlock(unsigned *m) { (void)m; assert(locks); locks=0; }
static unsigned observations, snapshot[8];
#if OBSERVER
void bridge_lwip_mem_failure(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,unsigned f,unsigned g,unsigned h,unsigned long pc) {
 assert(locks && protected && pc); ++observations;
 unsigned values[]={a,b,c,d,e,f,g,h}; memcpy(snapshot,values,sizeof(values));
}
#endif
#if LIFECYCLE
static unsigned lifecycle_events, allocated_charge, released_charge;
void bridge_lwip_mem_lifecycle(void *p, unsigned before, unsigned after) {
 assert(p && protected); // callback executes inside allocator critical section
 assert(before != after);
 ++lifecycle_events;
 if (after > before) allocated_charge += after-before;
 else released_charge += before-after;
}
#endif
''' + native + r'''
static void init(void) {
 memset(&storage,0,sizeof(storage)); memset(&lwip_stats,0,sizeof(lwip_stats));
 lfree=ptr_to_mem(0); ram_end=ptr_to_mem(1024); lfree->next=1024; ram_end->used=1;
 observations=0; memset(snapshot,0,sizeof(snapshot));
#if LIFECYCLE
 lifecycle_events=allocated_charge=released_charge=0;
#endif
}
int main(void) {
 // Exact admission includes alignment/header, and rejects under the heap lock.
 init(); int denied=9;
 void *a=bridge_mem_malloc_reserved(13,996,&denied);
 assert(a && !denied && lwip_stats.mem.used==28);
 assert(bridge_lwip_mem_charge(a)==28 && !bridge_lwip_mem_charge((u8_t *)a+4));
 assert(!bridge_lwip_mem_charge(NULL) && !bridge_lwip_mem_charge(&denied));
 assert(!bridge_mem_malloc_reserved(1,996,&denied) && denied);
 assert(!lwip_stats.mem.err && !observations && !locks && !protected);
 mem_free(a); assert(!lwip_stats.mem.used && !bridge_lwip_mem_charge(a));
 a=bridge_mem_malloc_reserved(13,996,&denied); assert(a && !denied);
 // Unrestricted control allocations may use the reserve; future RX then stops.
 void *b=mem_malloc(100); assert(b && lwip_stats.mem.used==140);
 assert(!bridge_mem_malloc_reserved(12,996,&denied) && denied);
 mem_free(b); mem_free(a); assert(!lwip_stats.mem.used);
 // Near fit charges the whole block, including slack that cannot be split.
 init(); assert(!bridge_mem_malloc_reserved(992,20,&denied) && denied);
 assert(!lwip_stats.mem.used);
 a=bridge_mem_malloc_reserved(992,0,&denied);assert(a && lwip_stats.mem.used==1024);
 assert(bridge_lwip_mem_charge(a)==1024); // full unsplittable native charge
 mem_free(a);assert(!lwip_stats.mem.used);
 // Reject an oversized unsplit hole, but keep searching for a cheaper fit.
 init(); unsigned fits[]={0,60,100,148,1024};
 for(unsigned i=0;i<4;++i) {
  struct mem *m=ptr_to_mem(fits[i]);m->next=fits[i+1];m->prev=i?fits[i-1]:0;m->used=i%2;
 }
 lwip_stats.mem.used=916;
 a=bridge_mem_malloc_reserved(36,60,&denied);
 assert(a==storage.bytes+112 && !denied && lwip_stats.mem.used==964 && !lwip_stats.mem.err);
 mem_free(a);assert(lwip_stats.mem.used==916);
 // Native trim returns only the actual released charge, both next-block cases.
 init(); a=mem_malloc(200); b=mem_malloc(100); assert(lwip_stats.mem.used==324);
 assert(mem_trim(a,100)==a && lwip_stats.mem.used==224);
 assert(bridge_lwip_mem_charge(a)==112 && bridge_lwip_mem_charge(b)==112);
 assert(mem_trim(a,96)==a && lwip_stats.mem.used==220);
 mem_free(b); mem_free(a);assert(!lwip_stats.mem.used);
 // Exercise the SDK pbuf admission entry point itself with exact heap charge.
 init(); struct pbuf *packet=bridge_pbuf_alloc_reserved(13,984,&denied);
 assert(packet && !denied && packet->len==13 && packet->type==PBUF_RAM);
 assert(lwip_stats.mem.used==40);
 assert(!bridge_pbuf_alloc_reserved(1,984,&denied) && denied);
 mem_free(packet);assert(!lwip_stats.mem.used);
#if LIFECYCLE
 assert(lifecycle_events==2 && allocated_charge==40 && released_charge==40);
#endif
 // A no-op trim or partial payload consumption cannot return allocation credit.
 init(); a=mem_malloc(100); b=mem_malloc(100);
 assert(mem_trim(a,96)==a); // slack too small to split: actual charge unchanged
#if LIFECYCLE
 assert(lifecycle_events==2 && allocated_charge==224 && !released_charge);
#endif
 mem_free(a); mem_free(b);
#if LIFECYCLE
 assert(lifecycle_events==4 && allocated_charge==224 && released_charge==224);
#endif
 // Shrinking with a free successor releases exactly its native charge delta.
 init(); a=mem_malloc(100); assert(mem_trim(a,96)==a);
#if LIFECYCLE
 assert(lifecycle_events==2 && allocated_charge==112 && released_charge==4);
#endif
 mem_free(a);
#if LIFECYCLE
 assert(lifecycle_events==3 && released_charge==112);
#endif
 // Invalid reserve is rejected without unsigned subtraction wrapping.
 assert(!bridge_mem_malloc_reserved(12,1025,&denied) && denied);
 init(); assert(!mem_malloc(0)); assert(!mem_malloc(1025)); assert(!observations && !locks);
 assert(mem_malloc(13)==storage.bytes+12); assert(lwip_stats.mem.used==28);
 assert(mem_malloc(980)); assert(!mem_malloc(12)); assert(lwip_stats.mem.err==1);
 assert(!locks && !protected);
#if OBSERVER
 assert(observations==1 && snapshot[0]==12 && snapshot[1]==12);
 assert(snapshot[2]==0 && snapshot[3]==0 && snapshot[4]==0 && snapshot[5]==1);
 assert(snapshot[6]==1024 && snapshot[7]==1);
#endif
 // Three separated holes: enough total space, no single fitting block.
 init(); unsigned offsets[]={0,212,424,636,848,1024};
 for(unsigned i=0;i<5;++i) {struct mem *m=ptr_to_mem(offsets[i]);m->next=offsets[i+1];m->used=i%2;}
 lwip_stats.mem.used=424;
 assert(!mem_malloc(220)); assert(!locks && !protected);
#if OBSERVER
 assert(observations==1 && snapshot[0]==220 && snapshot[1]==220);
 assert(snapshot[2]==564 && snapshot[3]==200 && snapshot[4]==3 && snapshot[5]==1);
#endif
 // Corrupt links must terminate the observer walk rather than loop or read out of bounds.
 init(); lfree=ram_end; ptr_to_mem(0)->next=2048;
 assert(!mem_malloc(12));
#if OBSERVER
 assert(observations==1 && snapshot[5]==0);
#endif
}
'''
            source=temp/'test.c'; source.write_text(harness)
            for enabled, lifecycle in ((0,0),(0,1),(1,0),(1,1)):
                binary=temp/'test'
                subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DOBSERVER={enabled}',f'-DLIFECYCLE={lifecycle}',str(source),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)

class ApplicationReportTest(unittest.TestCase):
    def test_ring_and_disabled_symbol(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            header = temp/'sdk/mm/mm.h'
            header.parent.mkdir(parents=True)
            header.write_text('#include <stdint.h>\nstatic inline uintptr_t mm_lock_save(void) { return 0; }\nstatic inline void mm_unlock_restore(uintptr_t f) { (void)f; }\n')
            harness = temp/'test.cpp'
            harness.write_text(r'''#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "lwip_allocation_diagnostic.h"
extern "C" void bridge_lwip_mem_failure(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned long);
static uint32_t value(uint8_t *r, unsigned n) { uint32_t v; memcpy(&v,r+n,4);return v; }
int main() {
 uint8_t r[64]={};
#if BL616CL_LWIP_ALLOC_FAILURE_DIAGNOSTIC
 for(unsigned i=1;i<=9;++i) bridge_lwip_mem_failure(100+i,104,500,200,3,1,19000,i,0x1234);
 bridge::lwip_allocation_diagnostic_report(53,r);assert(r[6]==1 && value(r,8)==9 && value(r,12)==4);
 unsigned seen=0;
 for(unsigned page=54;page<=57;++page) {
  memset(r,0,64);bridge::lwip_allocation_diagnostic_report(page,r);
  unsigned seq=value(r,8);assert(seq>=6 && seq<=9);seen|=1U<<(seq-6);
  assert(value(r,12)==100+seq && value(r,44)==0x1234 && r[5]==page && r[7]==1);
 }
 assert(seen==15);
#else
 bridge::lwip_allocation_diagnostic_report(53,r);assert(r[6]==0 && value(r,8)==0);
#endif
}
''')
            for enabled, lifecycle in ((0,0),(0,1),(1,0),(1,1)):
                binary = temp/'test'
                subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=undefined',f'-DBL616CL_LWIP_ALLOC_FAILURE_DIAGNOSTIC={enabled}','-I'+str(temp),'-I'+str(ROOT/'UNOR4BL616CLBridge'),str(harness),str(ROOT/'UNOR4BL616CLBridge/lwip_allocation_diagnostic.cpp'),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)
                symbols = subprocess.check_output(['nm',str(binary)],text=True)
                self.assertEqual(' T bridge_lwip_mem_failure' in symbols,bool(enabled))

if __name__ == '__main__': unittest.main()
