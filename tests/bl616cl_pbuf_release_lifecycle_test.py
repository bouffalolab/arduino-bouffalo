"""Validate native pbuf release events before designing receive memory credit.

The allocation hook records host blocks, not BL616CL allocator byte charges.
Native partial-consume/refcount logic decides when each block is released.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class PbufReleaseLifecycleTest(unittest.TestCase):
    def test_native_partial_consume_and_shared_references(self):
        native = (ROOT / 'third_party/bouffalo_sdk/components/net/lwip/lwip/src/core/pbuf.c').read_text()
        harness = r'''
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
static unsigned live_blocks, frees;
static void mem_free(void *p) { assert(live_blocks); --live_blocks; ++frees; free(p); }
'''
        harness += '\nu8_t\n' + function(native, 'pbuf_free(struct pbuf *p)')
        harness += '\nu8_t\n' + function(native, 'pbuf_remove_header(struct pbuf *p, size_t header_size_decrement)')
        harness += '\nstruct pbuf *\n' + function(native, 'pbuf_free_header(struct pbuf *q, u16_t size)')
        harness += r'''
static struct pbuf *block(u16_t bytes) {
  struct pbuf *p = calloc(1, sizeof(*p) + bytes); assert(p);
  p->payload = p + 1; p->len = p->tot_len = bytes;
  p->ref = 1; p->type = PBUF_TYPE_ALLOC_SRC_MASK_STD_HEAP; ++live_blocks;
  return p;
}
int main(void) {
  // Almost an entire block can be consumed while its allocation remains live.
  struct pbuf *p = block(1024); void *original = p->payload;
  assert(pbuf_free_header(p, 1023) == p);
  assert(p->len == 1 && p->tot_len == 1 && p->payload == (u8_t *)original + 1023);
  assert(live_blocks == 1 && frees == 0);
  assert(pbuf_free_header(p, 1) == NULL && !live_blocks && frees == 1);

  // Consuming across a chain releases whole nodes, retaining the partial tail.
  struct pbuf *head = block(1024), *tail = block(490);
  head->next = tail; head->tot_len = 1514;
  p = pbuf_free_header(head, 1025);
  assert(p == tail && p->len == 489 && p->tot_len == 489);
  assert(live_blocks == 1 && frees == 2);
  assert(pbuf_free(p) == 1 && !live_blocks && frees == 3);

  // A removed node is still allocated if another owner holds a reference.
  head = block(1024); tail = block(490);
  head->next = tail; head->tot_len = 1514; head->ref = 2;
  assert(pbuf_free_header(head, 1024) == tail);
  assert(head->ref == 1 && head->next == NULL && live_blocks == 2 && frees == 3);
  assert(pbuf_free(tail) == 1 && live_blocks == 1);
  assert(pbuf_free(head) == 1 && !live_blocks && frees == 5);

  // Close/abort freeing a chain stops at a retained reference.
  head = block(1024); tail = block(490);
  head->next = tail; head->tot_len = 1514; tail->ref = 2;
  assert(pbuf_free(head) == 1 && tail->ref == 1 && live_blocks == 1);
  assert(pbuf_free(tail) == 1 && !live_blocks && frees == 7);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            source.write_text(harness)
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
