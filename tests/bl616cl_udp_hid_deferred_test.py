"""The UDP HID page samples sockets only from poll and publishes atomically."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
def function(source,name):
    a=source.index(name);b=source.index('{',a);depth=1;c=b+1
    while depth:
        depth+=(source[c]=='{')-(source[c]=='}');c+=1
    return source[a:c]
class DeferredUdpHidTest(unittest.TestCase):
    def test_deferred_publication_and_page_switch(self):
        source=(ROOT/'UNOR4BL616CLBridge/usb_cmsis_dap.cpp').read_text()
        encode=function(source,'void encode_udp_socket_feature(')
        sample=function(source,'void sample_udp_socket(')
        code=r'''
#include <cassert>
#include <cstring>
#include "udp_socket_diagnostic.h"
using namespace bridge;
uint8_t hid_feature[64],diagnostic_feature_page=30;
constexpr unsigned kGpio35FeatureVersion=35;
volatile bool udp_socket_pending=false;
bool locked;unsigned queries;
uintptr_t mm_lock_save(){assert(!locked);locked=true;return 1;}
void mm_unlock_restore(uintptr_t flags){assert(locked && flags==1);locked=false;}
void write_u16_le(uint8_t *p,uint16_t n){p[0]=n;p[1]=n>>8;}
void write_u32_le(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=n>>(8*i);}
UdpSocketDiagnostic bridge::udp_socket_diagnostic_snapshot(){
 assert(!locked);++queries;UdpSocketDiagnostic d={};d.socket_local_port=18462;d.receive_success_count=3;return d;
}
'''+encode+sample+r'''
int main(){
 memset(hid_feature,0xa5,sizeof(hid_feature));sample_udp_socket();assert(!queries && hid_feature[0]==0xa5);
 udp_socket_pending=true;sample_udp_socket();assert(queries==1 && !udp_socket_pending && !locked);
 assert(!memcmp(hid_feature,"U35D",4) && hid_feature[5]==30);
 assert((hid_feature[14]|unsigned(hid_feature[15])<<8)==18462 && hid_feature[40]==3);
 sample_udp_socket();assert(queries==1);
 memset(hid_feature,0x7a,sizeof(hid_feature));diagnostic_feature_page=51;udp_socket_pending=true;
 sample_udp_socket();assert(queries==2 && hid_feature[0]==0x7a && !locked);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);(p/'test.cpp').write_text(code)
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'UNOR4BL616CLBridge'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
        refresh=function(source,'void refresh_hid_feature(')
        self.assertIn('diagnostic_feature_page == 30U) return;',refresh)
        self.assertNotIn('udp_socket_diagnostic_snapshot()',refresh)
if __name__=='__main__':unittest.main()
