"""Compile the production bounded credit state and run it under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class CreditPoolTest(unittest.TestCase):
    def test_production_credit_state(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'test'
            subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I'+str(ROOT/'UNOR4BL616CLBridge'),
                str(ROOT/'tests/bl616cl_tcp_rx_credit_pool_test.cpp'),
                str(ROOT/'UNOR4BL616CLBridge/tcp_rx_credit_pool.cpp'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


    def test_native_pbuf_lifetime_with_retired_owner(self):
        from bl616cl_pbuf_release_lifecycle_test import function
        native = (ROOT/'third_party/bouffalo_sdk/components/net/lwip/lwip/src/core/pbuf.c').read_text()
        source = r"""
#include "tcp_rx_credit_pool.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
typedef uint8_t u8_t; typedef uint16_t u16_t;
typedef unsigned LWIP_PBUF_REF_T;
struct pbuf { struct pbuf *next; void *payload; u16_t tot_len, len; unsigned ref, type; };
#define LWIP_ASSERT(m,c) assert(c)
#define LWIP_ERROR(m,c,h) do { if (!(c)) { h; } } while (0)
#define LWIP_UNUSED_ARG(x) (void)(x)
#define LWIP_DEBUGF(a,b)
#define PERF_START
#define PERF_STOP(m)
#define SYS_ARCH_DECL_PROTECT(x)
#define SYS_ARCH_PROTECT(x)
#define SYS_ARCH_UNPROTECT(x)
#define LWIP_SUPPORT_CUSTOM_PBUF 0
#define PBUF_TYPE_ALLOC_SRC_MASK_STD_MEMP_PBUF_POOL 1
#define PBUF_TYPE_ALLOC_SRC_MASK_STD_MEMP_PBUF 2
#define PBUF_TYPE_ALLOC_SRC_MASK_STD_HEAP 3
#define pbuf_get_allocsrc(p) ((p)->type)
#define memp_free(pool,p) abort()
static bridge::TcpRxCreditPool pool(12288,24576,256,2048,256);
static void mem_free(void *p) { assert(pool.resize((uintptr_t)p,0)); free(p); }
"""
        for signature, result in (
            ('pbuf_free(struct pbuf *p)', 'u8_t'),
            ('pbuf_remove_header(struct pbuf *p, size_t header_size_decrement)', 'u8_t'),
            ('pbuf_free_header(struct pbuf *q, u16_t size)', 'struct pbuf *')):
            source += '\n'+result+'\n'+function(native, signature)
        source += r"""
int main() {
 auto initial=pool.open(1); assert(pool.commit(initial));
 assert(pool.request(initial.token,2048));
 for(;;) { auto g=pool.next_grant(); if(!g.token) break; assert(pool.commit(g)); }
 struct pbuf *p=(struct pbuf *)calloc(1,sizeof(*p)+1024); assert(p);
 p->payload=p+1; p->len=p->tot_len=1024; p->ref=2;
 p->type=PBUF_TYPE_ALLOC_SRC_MASK_STD_HEAP;
 const unsigned charge=sizeof(*p)+1024; // host block charge, not BL allocator charge
 assert(pool.retain(initial.token,(uintptr_t)p,charge));
 assert(pool.received(initial.token,1024));
 assert(pbuf_free_header(p,1023)==p && pool.recved(initial.token,1023));
 assert(pool.snapshot().physical_used==charge && p->len==1);
 assert(pbuf_free_header(p,1)==nullptr && pool.recved(initial.token,1));
 assert(pool.snapshot().physical_used==charge && p->ref==1);
 assert(pool.retire(initial.token));
 auto replacement=pool.open(1); assert(replacement.token!=initial.token);
 assert(pool.commit(replacement) && pool.snapshot().retired_flows==1);
 const auto before=pool.snapshot().sequence_used;
 assert(pbuf_free(p)==1); // final reference, after native PCB lifetime ended
 assert(pool.snapshot().physical_used==0 && pool.snapshot().retired_flows==0);
 assert(pool.snapshot().sequence_used==before && pool.healthy());
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'test.cpp'; path.write_text(source)
            binary = Path(directory)/'test'
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-I'+str(ROOT/'UNOR4BL616CLBridge'),
                str(path),str(ROOT/'UNOR4BL616CLBridge/tcp_rx_credit_pool.cpp'),
                '-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__ == '__main__':
    unittest.main()
