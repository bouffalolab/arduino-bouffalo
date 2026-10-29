"""Run production cache query and dispatch: hits do no RF work, misses retain it."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
backend = (ROOT/'UNOR4BL616CLBridge/bouffalo_tls_client_backend.cpp').read_text()
start = backend.index('bool cached_available_tls(')
end = backend.index('\n#endif', start)
source = r'''
#include "tls_client_service.h"
#include "tls_window_timing.h"
#include <cassert>
#include <cstring>
#include <string>
static unsigned pauses, resumes, fallback_calls;
unsigned long micros() { return 0; }
static bool pause(void*) { ++pauses;return true; }
static void resume(void*) { ++resumes; }
static bool create(void*,int *handle) { *handle=1;return true; }
static bool close_client(void*,int) { return true; }
static bool available(void*,int,size_t *bytes) { ++fallback_calls;*bytes=23;return true; }
static bool peek(void*,int,int *value) { *value=65;return true; }
static bool receive(void*,int,uint8_t *data,size_t,size_t *bytes) { data[0]=65;*bytes=1;return true; }
struct TlsConnection { bool connected,has_peeked;size_t ssl; } connection;
static TlsConnection *get_connection(int h) { return h==1 ? &connection : nullptr; }
static size_t mbedtls_ssl_get_bytes_avail(const size_t *ssl) { return *ssl; }
''' + backend[start:end] + r'''
static std::string output;
static size_t write(void*,const uint8_t *data,size_t n) { output.append((const char*)data,n);return n; }
static bridge::at::CommandResult dispatch(void *p,const bridge::at::Command &c,bridge::at::AtTransport &t) {
 return static_cast<bridge::TlsClientService*>(p)->handle_command(c,t);
}
static bool zero_cache(void*,int,size_t *bytes) { *bytes=0;return true; }
int main() {
 size_t bytes=777;
 connection={true,true,1023};
 assert(!cached_available_tls(nullptr,0,&bytes) && bytes==777);
 assert(!cached_available_tls(nullptr,1,nullptr));
 connection.connected=false;
 assert(!cached_available_tls(nullptr,1,&bytes) && bytes==777);
 connection={true,false,0};
 assert(!cached_available_tls(nullptr,1,&bytes) && bytes==777);
 connection={true,true,0};
 assert(cached_available_tls(nullptr,1,&bytes) && bytes==1);
 connection={true,false,1023};
 assert(cached_available_tls(nullptr,1,&bytes) && bytes==1023);
 connection.has_peeked=true;
 assert(cached_available_tls(nullptr,1,&bytes) && bytes==1024);
 assert(connection.has_peeked && connection.ssl==1023);
 bridge::TlsClientBackend b={};b.create=create;b.close=close_client;b.available=available;
 b.peek=peek;b.receive=receive;
#if BL616CL_TLS_CACHED_AVAILABLE
 b.cached_available=cached_available_tls;
#endif
 bridge::TlsClientService service(b);service.set_scan_control(pause,resume,nullptr);
 bridge::at::AtTransport transport({256,64,512,20},write,nullptr);
 transport.set_command_handler(dispatch,&service);
 auto command=[&](const char *s,bool ok=true) {
  output.clear();transport.feed((const uint8_t*)s,strlen(s),1);
  while(transport.output_pending())transport.flush_output();
  assert(output.find(ok?"OK\r\n":"ERROR\r\n")!=std::string::npos);
 };
 command("AT+SSLBEGINCLIENT\r\n");
 command("AT+SSLAVAILABLE=2000\r\n");
#if BL616CL_TLS_CACHED_AVAILABLE
 assert(output=="+SSLAVAILABLE: 1024\r\nOK\r\n" && pauses==0 && fallback_calls==0);
 assert(bridge::tls_window_timing_counters().groups[0].count==1);
 assert(bridge::tls_window_timing_counters().groups[0].paused==0);
#else
 assert(output=="+SSLAVAILABLE: 23\r\nOK\r\n" && pauses==1 && fallback_calls==1);
#endif
 unsigned prior=pauses;
 connection={true,false,0};command("AT+SSLAVAILABLE=2000\r\n");
 assert(pauses==prior+1 && output=="+SSLAVAILABLE: 23\r\nOK\r\n");
 connection={true,true,1023};prior=pauses;
 command("AT+SSLPEEK=2000\r\n");command("AT+SSLCLIENTRECEIVE=2000,1\r\n");
 assert(pauses==prior+2);
 prior=pauses;
 command("AT+SSLAVAILABLE=2007\r\n",false);
 command("AT+SSLAVAILABLE=2000,1\r\n",false);
 command("AT+SSLAVAILABLE?\r\n",false);
 assert(pauses==prior+3 && pauses==resumes);
#if BL616CL_TLS_CACHED_AVAILABLE
 // Missing and invalid zero-positive callback results must retain RF protection.
 b.cached_available=nullptr;
 bridge::TlsClientService missing(b);missing.set_scan_control(pause,resume,nullptr);
 transport.set_command_handler(dispatch,&missing);command("AT+SSLBEGINCLIENT\r\n");
 prior=pauses;command("AT+SSLAVAILABLE=2000\r\n");assert(pauses==prior+1);
 b.cached_available=zero_cache;
 bridge::TlsClientService zero(b);zero.set_scan_control(pause,resume,nullptr);
 transport.set_command_handler(dispatch,&zero);command("AT+SSLBEGINCLIENT\r\n");
 prior=pauses;command("AT+SSLAVAILABLE=2000\r\n");assert(pauses==prior+1);
#else
 (void)zero_cache;
#endif
}
'''
with tempfile.TemporaryDirectory() as directory:
    work=Path(directory)
    (work/'Arduino.h').write_text('unsigned long micros();\n')
    (work/'test.cpp').write_text(source)
    for enabled in (0,1):
        binary=work/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-DBL616CL_TLS_WINDOW_TIMING=1',
                        f'-DBL616CL_TLS_CACHED_AVAILABLE={enabled}',
                        '-I'+str(work),'-I'+str(ROOT/'UNOR4BL616CLBridge'),
                        str(work/'test.cpp'),str(ROOT/'UNOR4BL616CLBridge/at_transport.cpp'),
                        str(ROOT/'UNOR4BL616CLBridge/tls_client_service.cpp'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print('BL616CL_TLS_CACHED_AVAILABLE_PASS')
