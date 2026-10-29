#!/usr/bin/env python3
"""Compile and exercise the actual bridge SDK patch's bounded record cache."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches/candidates/0062-bridge-bounded-tls-record-cache.patch'


def main():
    added = '\n'.join(line[1:] for line in PATCH.read_text().splitlines()
                      if line.startswith('+') and not line.startswith('+++'))
    start = added.index('/* Bridge profile:')
    end = added.index('/* End bridge record cache. */')
    helper = added[start:end]
    prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MBEDTLS_SSL_IN_BUFFER_LEN 2217U
#define MBEDTLS_SSL_OUT_BUFFER_LEN 2229U
static int critical_depth, allocations, frees, fail_allocation;
static void *mbedtls_calloc(size_t count, size_t length) {
    assert(critical_depth == 0);
    if (fail_allocation) return NULL;
    ++allocations;
    return calloc(count, length);
}
static void mbedtls_free(void *buffer) {
    assert(critical_depth == 0);
    if (buffer != NULL) ++frees;
    free(buffer);
}
static void mbedtls_platform_zeroize(void *buffer, size_t length) {
    assert(critical_depth == 0);
    memset(buffer, 0, length);
}
'''
    checks = r'''
int main(void) {
    uint32_t count, bytes, hits;
    unsigned char *in = bridge_record_alloc(MBEDTLS_SSL_IN_BUFFER_LEN);
    unsigned char *out = bridge_record_alloc(MBEDTLS_SSL_OUT_BUFFER_LEN);
    unsigned char *extra = bridge_record_alloc(MBEDTLS_SSL_IN_BUFFER_LEN);
    assert(in && out && extra && allocations == 3);
    memset(in, 0xA5, MBEDTLS_SSL_IN_BUFFER_LEN);
    memset(out, 0xA5, MBEDTLS_SSL_OUT_BUFFER_LEN);
    bridge_record_free(in, MBEDTLS_SSL_IN_BUFFER_LEN);
    bridge_record_free(out, MBEDTLS_SSL_OUT_BUFFER_LEN);
    bridge_record_free(extra, MBEDTLS_SSL_IN_BUFFER_LEN);
    bridge_tls_record_cache_stats(&count, &bytes, &hits);
    assert(count == 2 && bytes == 4446 && hits == 0 && frees == 1);
    fail_allocation = 1;
    assert(bridge_record_alloc(1024) == NULL);
    in = bridge_record_alloc(MBEDTLS_SSL_IN_BUFFER_LEN);
    out = bridge_record_alloc(MBEDTLS_SSL_OUT_BUFFER_LEN);
    assert(in && out && allocations == 3);
    for (size_t i = 0; i < MBEDTLS_SSL_IN_BUFFER_LEN; ++i) assert(in[i] == 0);
    for (size_t i = 0; i < MBEDTLS_SSL_OUT_BUFFER_LEN; ++i) assert(out[i] == 0);
    bridge_tls_record_cache_stats(&count, &bytes, &hits);
    assert(count == 0 && bytes == 0 && hits == 2);
    assert(bridge_record_alloc(MBEDTLS_SSL_IN_BUFFER_LEN) == NULL);
    fail_allocation = 0;
    unsigned char *small = bridge_record_alloc(1024);
    bridge_record_free(small, 1024);
    bridge_record_free(NULL, MBEDTLS_SSL_IN_BUFFER_LEN);
    bridge_tls_record_cache_stats(&count, &bytes, &hits);
    assert(count == 0 && allocations == 4 && frees == 2);
    bridge_record_free(in, MBEDTLS_SSL_IN_BUFFER_LEN);
    bridge_record_free(out, MBEDTLS_SSL_OUT_BUFFER_LEN);
    bridge_tls_record_cache_stats(&count, &bytes, &hits);
    assert(count == 2 && bytes == 4446);
    // Drain explicitly in this fixture; firmware intentionally retains two.
    mbedtls_free(bridge_record_alloc(MBEDTLS_SSL_IN_BUFFER_LEN));
    mbedtls_free(bridge_record_alloc(MBEDTLS_SSL_OUT_BUFFER_LEN));
    assert(allocations == frees && critical_depth == 0);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        (folder / 'FreeRTOS.h').write_text('')
        (folder / 'task.h').write_text(
            '#define taskENTER_CRITICAL() (++critical_depth)\n'
            '#define taskEXIT_CRITICAL() (--critical_depth)\n')
        source = folder / 'test.c'
        source.write_text(prefix + helper + checks)
        image = folder / 'test'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-I', str(folder), str(source), '-o', str(image)], check=True)
        subprocess.run([str(image)], check=True)
    print('BL616CL_TLS_RECORD_CACHE_PASS')


if __name__ == '__main__':
    main()
