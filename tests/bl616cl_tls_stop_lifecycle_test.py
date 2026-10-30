#!/usr/bin/env python3
"""Exercise the real stop/cleanup functions and baseline socket-close contract."""
from pathlib import Path
import subprocess
import tempfile
from bl616cl_sta_disconnect_wait_test import function
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'UNOR4BL616CLBridge/bouffalo_tls_client_backend.cpp').read_text()
baseline=(ROOT/'tests/fixtures/esp32/ssl_client.cpp').read_text()
assert 'mbedtls_ssl_close_notify' not in function(baseline, 'void stop_ssl_socket(')
program=r'''
#include <cassert>
struct TlsConnection {
 int socket=-1; bool connected=false,has_peeked=false,allocated=false;
 bool ssl_initialized=false,config_initialized=false,ca_initialized=false;
 bool client_certificate_initialized=false,client_private_key_initialized=false;
 int ssl=0,config=0,ca=0,client_certificate=0,client_private_key=0;
};
static TlsConnection slot;
static int closes,frees,alerts;
static TlsConnection *get_connection(int h) { return h==0 && slot.allocated?&slot:nullptr; }
static int closesocket(int h) { assert(h==7); ++closes; return 0; }
static void mbedtls_ssl_free(int*) { ++frees; }
static void mbedtls_ssl_config_free(int*) { ++frees; }
static void mbedtls_x509_crt_free(int*) { ++frees; }
static void mbedtls_pk_free(int*) { ++frees; }
[[maybe_unused]] static int mbedtls_ssl_close_notify(int*) { ++alerts; return 0; }
'''
for signature in ('void free_tls_context(', 'void discard_tls_connection(', 'bool close_tls('):
 program+=function(source, signature)+'\n'
program+=r'''
int main() {
 for (bool connected: {false,true}) {
  slot={};slot.socket=7;slot.allocated=true;slot.connected=connected;slot.has_peeked=true;
  slot.ssl_initialized=slot.config_initialized=slot.ca_initialized=true;
  slot.client_certificate_initialized=slot.client_private_key_initialized=true;
  int c=closes,f=frees;
  assert(close_tls(nullptr,0));assert(closes==c+1 && frees==f+5 && alerts==0);
  assert(slot.socket==-1 && !slot.allocated && !slot.connected && !slot.has_peeked);
  assert(!close_tls(nullptr,0));assert(closes==c+1 && frees==f+5);
 }
 slot={};slot.allocated=true;
 assert(close_tls(nullptr,0));assert(!slot.allocated && closes==2 && frees==10);
}
'''
program=program.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='bl616cl-stop-') as directory:
 work=Path(directory);(work/'test.cpp').write_text(program)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(work/'test.cpp'),'-o',str(work/'test')],check=True)
 subprocess.run([str(work/'test')],check=True)
print('BL616CL_TLS_STOP_LIFECYCLE_PASS')
