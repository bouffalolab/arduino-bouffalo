#!/usr/bin/env python3
"""Run actual optional station-removal hooks without changing driver commands."""
from pathlib import Path
import subprocess,tempfile
from bl616cl_sta_disconnect_wait_test import function
ROOT=Path(__file__).resolve().parents[1]
patches=ROOT/'tools/bridge_runtime/profiles/bridge/fhost_patches'
paths=['components/wireless/wifi6/wpa_supplicant/src/'+p for p in
       ('drivers/driver_macsw.c','ap/sta_info.c','ap/ieee802_11.c')]
with tempfile.TemporaryDirectory(prefix='bl616cl-ap-observer-') as directory:
 work=Path(directory)
 for path in paths:
  p=work/path;p.parent.mkdir(parents=True,exist_ok=True)
  p.write_bytes((ROOT/'third_party/bouffalo_sdk'/path).read_bytes())
 subprocess.run(['patch','--batch','-p1','-i',str(patches/'0069-bridge-ap-station-removal-diagnostic.patch')],cwd=work,check=True,stdout=subprocess.DEVNULL)
 driver=(work/paths[0]).read_text()
 prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned char u8;
struct wpa_macsw_driver_itf_data {int fhost_vif_idx;};
struct header {int id;};
struct cfgmacsw_sta_remove {struct header hdr;int fhost_vif_idx;const struct mac_addr *addr;};
struct cfgmacsw_resp {struct header hdr;int status;};
#define CFGMACSW_STA_REMOVE_CMD 1
#define CFGMACSW_STA_REMOVE_RESP 2
#define CFGMACSW_SUCCESS 0
static int calls,commands,fail_send,response_status;
static uintptr_t site;
static const u8 *expected_address;
extern void bridge_ap_sta_remove_observer(uintptr_t) __attribute__((weak));
#ifndef NO_OBSERVER
void bridge_ap_sta_remove_observer(uintptr_t address) {++calls;site=address;}
#endif
static void wpa_macsw_msg_hdr_init(void *drv,struct header *h,int id,size_t size) {
 (void)drv;(void)size;h->id=id;
}
static int fhost_cntrl_cfgmacsw_cmd_send(struct header *c,struct header *r) {
 struct cfgmacsw_sta_remove *cmd=(void *)c;struct cfgmacsw_resp *resp=(void *)r;
 assert(cmd->fhost_vif_idx==3 && (const u8*)cmd->addr==expected_address);
 assert(c->id==1 && r->id==2);++commands;resp->status=response_status;return fail_send;
}
'''
 main=r'''
int main(void) {
 struct wpa_macsw_driver_itf_data drv={3};u8 addr[6]={0};expected_address=addr;
 assert(wpa_macsw_driver_sta_remove(&drv,addr)==0);
 fail_send=1;assert(wpa_macsw_driver_sta_remove(&drv,addr)==-1);
 fail_send=0;response_status=1;assert(wpa_macsw_driver_sta_remove(&drv,addr)==-1);
 assert(commands==3);
#ifndef NO_OBSERVER
 assert(calls==3 && site!=0);
#else
 assert(calls==0 && site==0);
#endif
}
'''
 (work/'driver.c').write_text(prefix+function(driver,'static int wpa_macsw_driver_sta_remove(')+main)
 for flags in ([],['-DNO_OBSERVER']):
  subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror',*flags,str(work/'driver.c'),'-o',str(work/'driver')],check=True)
  subprocess.run([str(work/'driver')],check=True)
 source=(ROOT/'UNOR4BL616CLBridge/coex_diagnostic.cpp').read_text()
 code='#include <cassert>\n#include "coex_diagnostic.h"\nnamespace bridge {\n'
 for name in ('remove','free','rx_leave'):
  code+=function(source,'extern "C" void bridge_ap_sta_'+name+'_observer(')+'\n'
 code+='''}
int main() {
 using namespace bridge;
 bridge_ap_sta_remove_observer(123);bridge_ap_sta_remove_observer(456);
 bridge_ap_sta_free_observer(789,2,17);bridge_ap_sta_rx_leave_observer(1,8);
 auto &c=ap_station_removal_counters();
 assert(c.removes==2 && c.remove_site==456 && c.free_site==789);
 assert(c.timeout_next==2 && c.flags==17 && c.rx_leave_kind==1 && c.rx_leave_reason==8);
 bridge_ap_sta_rx_leave_observer(2,3);
 assert(c.rx_leave_kind==2 && c.rx_leave_reason==3 && c.removes==2);
}
'''
 (work/'app.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'UNOR4BL616CLBridge'),str(work/'app.cpp'),'-o',str(work/'app')],check=True)
 subprocess.run([str(work/'app')],check=True)
print('BL616CL_AP_STATION_OBSERVER_PASS')
