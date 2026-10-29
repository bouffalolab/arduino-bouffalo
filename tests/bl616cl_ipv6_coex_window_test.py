"""Run the production periodic window with fake clock/controller callbacks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class WindowTest(unittest.TestCase):
    def test_disabled_policy_links_without_controller_or_wifi(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            (p/'test.cpp').write_text('#include "network_idle_coex.h"\nint main(){bridge::network_idle_coex_poll();}\n')
            subprocess.run(['g++','-std=c++17','-DBL616CL_NETWORK_IDLE_COEX=0',
                '-I',str(ROOT/'UNOR4BL616CLBridge'),str(p/'test.cpp'),
                str(ROOT/'UNOR4BL616CLBridge/network_idle_coex.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_window_restores_and_leaves_ble_time(self):
        source=(ROOT/'UNOR4BL616CLBridge/network_idle_coex.cpp').read_text()
        start=source.index('void network_idle_coex_poll()')
        end=source.rindex('}\n')
        code=r'''
#include <cstdint>
#include <cassert>
#define BL616CL_NETWORK_IDLE_COEX 1
#include "network_scan_window.h"
uint32_t clock_ms=0,ap_state=0;int pauses=0,resumes=0;bool scanning=true,can_pause=true;
uint32_t millis(){return clock_ms;}
void delay(unsigned ms){assert(!scanning);clock_ms+=ms;}
bool bouffalo_wifi_ap_ready(){return (ap_state&0x10)!=0;}
bool pause(void*){++pauses;if(!can_pause)return false;assert(scanning);scanning=false;return true;}
void resume(void*){++resumes;assert(!scanning);scanning=true;}
struct Controller{bool (*pause_scan)(void*)=pause;void (*resume_scan)(void*)=resume;void*context=nullptr;};
struct BouffaloHciController{static Controller active_controller(){return {};}};
namespace bridge {
'''+source[start:end]+r'''
}
int main(){
 bridge::network_idle_coex_poll();assert(pauses==0);
 clock_ms=40;ap_state=0x10;bridge::network_idle_coex_poll();
 assert(pauses==1&&resumes==1&&scanning&&clock_ms==80);
 clock_ms=119;bridge::network_idle_coex_poll();assert(pauses==1);
 clock_ms=120;bridge::network_idle_coex_poll();assert(pauses==2&&resumes==2);
 clock_ms=200;can_pause=false;bridge::network_idle_coex_poll();
 assert(pauses==3&&resumes==2&&scanning);
 clock_ms=240;ap_state=0;bridge::network_idle_coex_poll();assert(pauses==3);
 clock_ms=280;ap_state=0x10;can_pause=true;
 {bridge::NetworkScanWindow network(pause,resume,nullptr);assert(!scanning);}
 assert(pauses==4&&resumes==3&&scanning);
 bridge::network_idle_coex_poll();assert(pauses==4&&clock_ms==280);
 clock_ms=379;bridge::network_idle_coex_poll();assert(pauses==4);
 // Once actual network commands stop, incoming packets regain RF windows.
 clock_ms=380;bridge::network_idle_coex_poll();
 assert(pauses==5&&resumes==4&&scanning&&clock_ms==420);
 clock_ms=421;
 {bridge::NetworkScanWindow network(pause,resume,nullptr);}
 bridge::network_idle_coex_poll();assert(pauses==6&&clock_ms==421);
 clock_ms=520;bridge::network_idle_coex_poll();assert(pauses==6);
 clock_ms=521;bridge::network_idle_coex_poll();assert(pauses==7&&resumes==6);
 // Network activity just before millis() wraps must defer exactly 100ms.
 clock_ms=0xfffffff0U;
 {bridge::NetworkScanWindow network(pause,resume,nullptr);}
 bridge::network_idle_coex_poll();assert(pauses==8);
 clock_ms=83;bridge::network_idle_coex_poll();assert(pauses==8);
 clock_ms=84;bridge::network_idle_coex_poll();assert(pauses==9&&resumes==8&&clock_ms==124);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.cpp').write_text(code)
            subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-I',str(ROOT/'UNOR4BL616CLBridge'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
