#!/usr/bin/env python3
"""Run the actual backend policy setter with a live/stopped/failing WPA control."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'UNOR4BL616CLBridge/bouffalo_wifi_backend.cpp').read_text()
a=source.index('bool set_auto_reconnect(void *, bool enabled)')
b=source.index('\nbool set_sta_role_enabled',a)
code=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#define MGMR_VIF_STA 0
#define FHOST_WPA_STATE_STOPPED 0
struct { uint8_t disable_autoreconnect; } wifiMgmr;
static int state, result, calls;
static unsigned policy;
int fhost_wpa_get_state(int index) { assert(index==0);return state; }
int fhost_wpa_execute_cmd(int index, char *response, int *length,
                          const char *format, unsigned enabled) {
 assert(index==0 && !response && !length);
 assert(strcmp(format,"STA_AUTOCONNECT %u")==0);
 ++calls; policy=enabled; return result;
}
'''+source[a:b]+r'''
int main() {
 assert(set_auto_reconnect(nullptr,false));
 assert(wifiMgmr.disable_autoreconnect==1 && calls==0);
 state=2;
 assert(set_auto_reconnect(nullptr,true));
 assert(wifiMgmr.disable_autoreconnect==0 && policy==1 && calls==1);
 assert(set_auto_reconnect(nullptr,false));
 assert(wifiMgmr.disable_autoreconnect==1 && policy==0 && calls==2);
 state=1; // VIF survives an AP outage and still needs live policy updates.
 assert(set_auto_reconnect(nullptr,true));
 assert(wifiMgmr.disable_autoreconnect==0 && policy==1 && calls==3);
 result=-1;
 assert(!set_auto_reconnect(nullptr,false));
 assert(wifiMgmr.disable_autoreconnect==0 && calls==4);
}
'''
with tempfile.TemporaryDirectory() as directory:
    folder=Path(directory);(folder/'test.cpp').write_text(code)
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',str(folder/'test.cpp'),'-o',str(folder/'test')],check=True)
    subprocess.run([str(folder/'test')],check=True)
print('BL616CL_LIVE_RECONNECT_POLICY_PASS')
