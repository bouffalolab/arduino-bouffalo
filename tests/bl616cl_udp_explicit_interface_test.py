#!/usr/bin/env python3
"""Execute the native UDP open function with AP/STA binds and device failures."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'UNOR4BL616CLBridge/bouffalo_udp_backend.cpp').read_text()
a=s.index('bool open_udp_socket(');b=s.index('\nbool join_multicast',a)
code=r'''
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cerrno>
#define AF_INET 2
#define SOCK_DGRAM 2
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define INADDR_ANY 0
#define LOCK_TCPIP_CORE() ((void)0)
#define UNLOCK_TCPIP_CORE() ((void)0)
constexpr unsigned kUdpDiagnosticSocketCreated=1,kUdpDiagnosticReuseAddress=2,kUdpDiagnosticLocalBound=4;
struct { int last_errno=0,last_handle=0;unsigned flags=0; } udp_trace;
struct sockaddr {};
struct sockaddr_in { int sin_len=0,sin_family=0;unsigned sin_port=0;struct {uint32_t s_addr=0;} sin_addr; };
static bool ap=true, interface_ok=true, bind_ok=true;
static int interface_calls,close_calls;
static uint32_t bound;
uint32_t htonl(uint32_t n) {return n;}
unsigned htons(unsigned n) {return n;}
int socket(int,int,int) {return 7;}
int setsockopt(int,int,int,const void*,size_t) {return 0;}
int bind(int,const sockaddr *address,size_t) {
 bound=reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr;return bind_ok?0:-1;
}
int closesocket(int) {++close_calls;return 0;}
bool active_ap_address(uint32_t *value) {if(ap)*value=0xc0a80701;return ap;}
bool bind_socket_to_active_ap_interface(int) {++interface_calls;return interface_ok;}
void capture_lwip_start() {}
'''+s[a:b]+r'''
int main() {
 int handle=-1;
 uint8_t sta[4]={192,168,4,2},local_ap[4]={192,168,7,1};
 assert(open_udp_socket(nullptr,4244,nullptr,&handle));
 assert(handle==7 && bound==0xc0a80701 && interface_calls==1);
 assert(open_udp_socket(nullptr,4244,sta,&handle));
 assert(bound==0xc0a80402 && interface_calls==1);
 assert(open_udp_socket(nullptr,4244,local_ap,&handle));
 assert(interface_calls==2);
 ap=false;
 assert(open_udp_socket(nullptr,4244,sta,&handle));
 assert(interface_calls==2);
 ap=true;interface_ok=false;
 assert(!open_udp_socket(nullptr,4244,local_ap,&handle));
 assert(close_calls==1);
 // An unrelated AP interface failure cannot reject a valid explicit STA bind.
 assert(open_udp_socket(nullptr,4244,sta,&handle));
 bind_ok=false;
 assert(!open_udp_socket(nullptr,4244,sta,&handle));
 assert(close_calls==2);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('BL616CL_UDP_EXPLICIT_INTERFACE_PASS')
