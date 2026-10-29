"""Time real TLS dispatch and RAII exits with a deterministic host clock."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / 'Arduino.h').write_text('unsigned long micros();\n')
    (work / 'test.cpp').write_text(r'''
#include "tls_client_service.h"
#include "tls_window_timing.h"
#include <cassert>
#include <cstring>
#include <string>
#include <cstdint>
static uint32_t now;
unsigned long micros() { return now; }
static unsigned pauses, resumes;
static bool pause(void*) { ++pauses; now+=20000; return true; }
static void resume(void*) { ++resumes; now+=21000; }
static bool create(void*,int *handle) { *handle=1; return true; }
static bool close_client(void*,int) { return true; }
static bool available(void*,int,size_t *size) { now+=37;*size=10;return true; }
static bool peek(void*,int,int *value) { now+=19;*value=0x41;return true; }
static bool receive(void*,int,uint8_t *data,size_t,size_t *size) { now+=53;data[0]='A';*size=1;return true; }
static std::string output;
static size_t write(void*,const uint8_t *data,size_t n) { output.append((const char*)data,n);return n; }
static bridge::at::CommandResult dispatch(void *p,const bridge::at::Command &c,bridge::at::AtTransport &t) {
 return static_cast<bridge::TlsClientService*>(p)->handle_command(c,t);
}
int main() {
 bridge::TlsClientBackend backend={};backend.create=create;backend.close=close_client;
 backend.available=available;backend.peek=peek;backend.receive=receive;
 bridge::TlsClientService service(backend);service.set_scan_control(pause,resume,nullptr);
 bridge::at::AtTransport transport({256,64,512,20},write,nullptr);
 transport.set_command_handler(dispatch,&service);
 auto command=[&](const char *s,bool ok=true) {
  output.clear();transport.feed((const uint8_t*)s,strlen(s),1);
  while(transport.output_pending())transport.flush_output();
  assert(output.find(ok?"OK\r\n":"ERROR\r\n")!=std::string::npos);
 };
 command("AT+SSLBEGINCLIENT\r\n");
 command("AT+SSLAVAILABLE=2000\r\n");
 command("AT+SSLPEEK=2000\r\n");
 command("AT+SSLCLIENTRECEIVE=2000,1\r\n");
 assert(pauses==3 && resumes==3);
 command("AT+TLSWINDOWDIAG?\r\n");
#if BL616CL_TLS_WINDOW_TIMING
 assert(output.find("+TLSWINDOWDIAG: 1,1,0,")!=std::string::npos);
 auto &c=bridge::tls_window_timing_counters();
 const uint32_t body[]={37,19,53};
 for(unsigned i=0;i<3;++i) {
  const auto &g=c.groups[i];assert(g.count==1 && g.paused==1);
  assert(g.sum[0]==20000 && g.sum[1]==body[i] && g.sum[2]==21000);
  for(unsigned j=0;j<3;++j)assert(g.maximum[j]==g.sum[j]);
 }
 // Early error still runs all destructors, and unsigned timing crosses wrap.
 now=UINT32_MAX-10000;
 command("AT+SSLAVAILABLE=2007\r\n",false);
 assert(c.groups[0].count==2 && c.groups[0].sum[0]==40000 && c.groups[0].sum[2]==42000);
 assert(pauses==4 && resumes==4);
 c.groups[0].sum[0]=UINT32_MAX-1;
 command("AT+SSLAVAILABLE=2000\r\n");
 assert(c.overflow==1 && c.groups[0].sum[0]==UINT32_MAX);
 service.set_scan_control(nullptr,nullptr,nullptr);
 command("AT+SSLPEEK=2000\r\n");
 assert(c.groups[1].count==2 && c.groups[1].paused==1 && c.groups[1].sum[1]==38);
 assert(c.groups[1].sum[0]==20000 && c.groups[1].sum[2]==21000);
#else
 assert(output=="+TLSWINDOWDIAG: 1,0\r\nOK\r\n");
#endif
 const unsigned before=pauses;
 command("AT+TLSWINDOWDIAG=1\r\n",false);
 assert(pauses==before);
}
''')
    for enabled in (0, 1):
        binary = work / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DBL616CL_TLS_WINDOW_TIMING={enabled}',
                        '-I'+str(work), '-I'+str(ROOT/'UNOR4BL616CLBridge'),
                        str(work/'test.cpp'), str(ROOT/'UNOR4BL616CLBridge/at_transport.cpp'),
                        str(ROOT/'UNOR4BL616CLBridge/tls_client_service.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('BL616CL_TLS_WINDOW_TIMING_PASS')
