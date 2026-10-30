#!/usr/bin/env python3
"""Validate passive SDK patch and execute its weak hook/filter and app counters."""
from pathlib import Path
import subprocess,tempfile,re
from bl616cl_sta_disconnect_wait_test import function
ROOT=Path(__file__).resolve().parents[1]
p='components/net/lwip/lwip/src/core/tcp_in.c'
patchfile=ROOT/'tools/bridge_runtime/profiles/bridge/fhost_patches/0070-bridge-tcp-listen-ingress-diagnostic.patch'
with tempfile.TemporaryDirectory(prefix='tcp-listen-observer-') as temp:
 work=Path(temp);target=work/p;target.parent.mkdir(parents=True)
 original=(ROOT/'third_party/bouffalo_sdk'/p).read_text();target.write_text(original)
 subprocess.run(['patch','--batch','-p1','-i',str(patchfile)],cwd=work,check=True,stdout=subprocess.DEVNULL)
 patched=target.read_text()
 start=patched.index('/* Passive events for validated SYNs');end=patched.index('\nvoid\ntcp_input(',start)
 hook=patched[start:end]
 restored=patched[:start]+patched[end+1:]
 restored=re.sub(r'^  +BRIDGE_TCP_LISTEN_OBSERVE\([0-5]\);\n','',restored,flags=re.M)
 restored=restored.replace('\n\n  /* Demultiplex','\n  /* Demultiplex',1)
 # The patch adds only observer declarations/calls; original TCP code is exact.
 assert restored==original
 prefix='''
#include <assert.h>
#define TCP_SYN 2
#define TCP_ACK 16
static unsigned flags;
static struct {unsigned dest;} header={25081};
#define tcphdr (&header)
static unsigned events[6];
#ifndef NO_HOOK
void bridge_tcp_listen_observer(unsigned event,unsigned port) {
 assert(event<6 && port==25081);++events[event];
}
#endif
'''
 main='''
int main(void) {
 flags=TCP_SYN;
 for(unsigned i=0;i<6;++i) {BRIDGE_TCP_LISTEN_OBSERVE(i);}
 flags=TCP_SYN|TCP_ACK;BRIDGE_TCP_LISTEN_OBSERVE(0);
 flags=TCP_ACK;BRIDGE_TCP_LISTEN_OBSERVE(1);
 flags=0;BRIDGE_TCP_LISTEN_OBSERVE(2);
 for(unsigned i=0;i<6;++i) {
#ifndef NO_HOOK
 assert(events[i]==1);
#else
 assert(events[i]==0);
#endif
 }
}
'''
 (work/'hook.c').write_text(prefix+hook+main)
 for opts in ([],['-DNO_HOOK']):
  subprocess.run(['gcc','-Wall','-Wextra','-Werror',*opts,str(work/'hook.c'),'-o',str(work/'hook')],check=True)
  subprocess.run([str(work/'hook')],check=True)
 source=(ROOT/'UNOR4BL616CLBridge/coex_diagnostic.cpp').read_text()
 code='#include <cassert>\n#include "coex_diagnostic.h"\nnamespace bridge {\n'+function(source,'extern "C" void bridge_tcp_listen_observer(')+'\n}\n'
 code+='''int main() {
 using namespace bridge;
 bridge_tcp_listen_observer(0,123);bridge_tcp_listen_observer(1,25081);
 bridge_tcp_listen_observer(1,25081);bridge_tcp_listen_observer(99,999);
 auto &c=tcp_listen_ingress_counters();
 assert(c.events[0]==1 && c.events[1]==2 && c.events[2]==0 && c.last_port==25081);
}'''
 (work/'app.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'UNOR4BL616CLBridge'),str(work/'app.cpp'),'-o',str(work/'app')],check=True)
 subprocess.run([str(work/'app')],check=True)
print('BL616CL_TCP_LISTEN_OBSERVER_PASS')
