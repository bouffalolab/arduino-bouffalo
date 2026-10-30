#!/usr/bin/env python3
"""Inject failures into actual patched mbedTLS setup and its allocation prefix."""
from pathlib import Path
import subprocess
import tempfile
from bl616cl_sta_disconnect_wait_test import function

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'components/crypto/mbedtls/mbedtls_v3/library/ssl_tls.c'
PATCH = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches/0064-bridge-tls-setup-allocation-diagnostic.patch'

PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED -1
#define MBEDTLS_ERR_SSL_ALLOC_FAILED -32512
#define MBEDTLS_SSL_IN_BUFFER_LEN 2349
#define MBEDTLS_SSL_OUT_BUFFER_LEN 1325
#define MBEDTLS_SSL_DEBUG_MSG(...) ((void)0)
typedef struct { char data[128]; } mbedtls_ssl_transform;
typedef struct { char data[96]; } mbedtls_ssl_session;
typedef struct { char data[2400]; } mbedtls_ssl_handshake_params;
typedef struct { int max_tls_version; } mbedtls_ssl_config;
typedef struct {
    const mbedtls_ssl_config *conf;
    int tls_version;
    unsigned char *in_buf, *out_buf;
    void *in_hdr, *in_ctr, *in_len, *out_hdr, *out_ctr, *out_len;
    void *in_iv, *in_msg, *out_iv, *out_msg;
    mbedtls_ssl_transform *transform_negotiate;
    mbedtls_ssl_session *session_negotiate;
    mbedtls_ssl_handshake_params *handshake;
} mbedtls_ssl_context;
static int calls, fail_at, live, stage;
static size_t requested;
static void *context;
static void *mbedtls_calloc(size_t count, size_t size) {
    if (++calls == fail_at) return NULL;
    void *p = calloc(count, size); assert(p); ++live; return p;
}
static void mbedtls_free(void *p) { if (p) { --live; free(p); } }
static void mbedtls_ssl_transform_free(void *p) { (void)p; }
static void mbedtls_ssl_session_free(void *p) { (void)p; }
static void mbedtls_ssl_handshake_free(void *p) { (void)p; }
static void mbedtls_ssl_reset_in_pointers(void *p) { (void)p; }
static void mbedtls_ssl_reset_out_pointers(void *p) { (void)p; }
static int ssl_conf_check(void *p) { (void)p; return 0; }
#ifndef NO_CALLBACK
void bridge_mbedtls_alloc_failed(const void *ssl, unsigned s, size_t n) {
    if (!stage) { stage = s; requested = n; context = (void *)ssl; }
}
#endif
'''
MAIN = r'''
int main(void) {
    const int expected[6] = {0, 1, 2, 3, 4, 5};
    const size_t sizes[6] = {0, 2349, 1325, 128, 96, 2400};
    for (int failure = 0; failure <= 5; ++failure) {
        mbedtls_ssl_context ssl = {0}; mbedtls_ssl_config config = {0};
        calls = stage = 0; requested = 0; context = NULL; fail_at = failure;
        int result = mbedtls_ssl_setup(&ssl, &config);
        assert(result == (failure ? MBEDTLS_ERR_SSL_ALLOC_FAILED : 0));
#ifndef NO_CALLBACK
        assert(stage == expected[failure]);
        assert(requested == sizes[failure]);
        if (failure) assert(context == &ssl);
#else
        (void)expected; (void)sizes; assert(stage == 0);
#endif
        mbedtls_free(ssl.in_buf); mbedtls_free(ssl.out_buf);
        mbedtls_free(ssl.transform_negotiate); mbedtls_free(ssl.session_negotiate);
        mbedtls_free(ssl.handshake); assert(live == 0);
    }
    return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        target = folder / TARGET
        target.parent.mkdir(parents=True)
        target.write_text((ROOT / 'third_party/bouffalo_sdk' / TARGET).read_text())
        subprocess.run(['patch', '--batch', '-p1', '-i', str(PATCH)], cwd=folder,
                       check=True, stdout=subprocess.DEVNULL)
        source = target.read_text()
        hook = source[source.index('extern void bridge_mbedtls_alloc_failed'):source.index('MBEDTLS_CHECK_RETURN_CRITICAL\nstatic int ssl_handshake_init')]
        allocation = function(source, 'static int ssl_handshake_init(')
        allocation = allocation[:allocation.index('#if defined(MBEDTLS_SSL_EARLY_DATA)')] + '    (void)ret; return 0;\n}\n'
        setup = function(source, 'int mbedtls_ssl_setup(')
        harness = folder / 'setup.c'
        harness.write_text(PREFIX + hook + allocation + setup + MAIN)
        for flags in ([], ['-DNO_CALLBACK']):
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                            str(harness), '-o', str(folder / 'setup')], check=True)
            subprocess.run([str(folder / 'setup')], check=True)
        backend = (ROOT / 'UNOR4BL616CLBridge/bouffalo_tls_client_backend.cpp').read_text()
        callback = function(backend, 'extern "C" void bridge_mbedtls_alloc_failed(')
        harness = folder / 'context.cpp'
        harness.write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
struct Slot { int ssl; bool ssl_initialized; uint8_t diagnostic_phase; };
Slot connections[8] = {};
''' + callback + r'''
int main() {
    connections[0] = {0, true, 32}; connections[1] = {0, true, 32};
    int other = 0;
    bridge_mbedtls_alloc_failed(&other, 1, 2381);
    assert(connections[0].diagnostic_phase == 32);
    bridge_mbedtls_alloc_failed(&connections[1].ssl, 1, 2381);
    assert(connections[1].diagnostic_phase == 51);
    assert(connections[0].diagnostic_phase == 32);
    bridge_mbedtls_alloc_failed(&connections[1].ssl, 5, 2400);
    assert(connections[1].diagnostic_phase == 51);
    for (unsigned stage = 1; stage <= 5; ++stage) {
        connections[0].diagnostic_phase = 32;
        bridge_mbedtls_alloc_failed(&connections[0].ssl, stage, 1);
        assert(connections[0].diagnostic_phase == 50 + stage);
    }
    connections[0].diagnostic_phase = 32;
    bridge_mbedtls_alloc_failed(&connections[0].ssl, 0, 1);
    bridge_mbedtls_alloc_failed(&connections[0].ssl, 6, 1);
    assert(connections[0].diagnostic_phase == 32);
    connections[0].ssl_initialized = false;
    bridge_mbedtls_alloc_failed(&connections[0].ssl, 1, 1);
    assert(connections[0].diagnostic_phase == 32);
    connections[0].ssl_initialized = true;
    connections[0].diagnostic_phase = 43;
    bridge_mbedtls_alloc_failed(&connections[0].ssl, 1, 1);
    assert(connections[0].diagnostic_phase == 43);
}
''')
        subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        str(harness), '-o', str(folder / 'context')], check=True)
        subprocess.run([str(folder / 'context')], check=True)
    print('BL616CL_TLS_SETUP_ALLOCATION_PASS')

if __name__ == '__main__':
    main()
