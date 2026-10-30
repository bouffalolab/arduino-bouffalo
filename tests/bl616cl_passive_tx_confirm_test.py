#!/usr/bin/env python3
"""Verify passive FHOST observation preserves release order and filters frames."""
from pathlib import Path
import subprocess
import tempfile
from bl616cl_sta_disconnect_wait_test import function

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches'
TARGET = 'components/wireless/wifi6/fhost/src/fhost_tx.c'
PREFIX = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "coex_diagnostic.h"
#include "dhcp_ack_packet.h"
#include "dhcp_tx_diagnostic.h"
#include "sta_tcp_diagnostic.h"
using bridge::wifi_syn_counters;
using bridge::ap_join_scan_counters;
using bridge::is_dhcp_ack_packet;
using bridge::dhcp_server_message_type;
#if BL616CL_DHCP_TX_DIAGNOSTIC
using bridge::dhcp_tx_diagnostic_note;
#endif
using bridge::is_ap_client_ip_reply;
using net_al_tx_t = void *;
using cb_fhost_tx = void (*)(uintptr_t, uint32_t, void *);
struct fhost_rx_buf_tag {};
struct pbuf { unsigned char data[1024]; unsigned size; };
static int stage, observations, releases;
#if BL616CL_STA_TCP_DIAGNOSTIC
static unsigned tcp_release_calls;
namespace bridge {
void sta_tcp_tx_release(void *, uint32_t, uint32_t) { ++tcp_release_calls; }
}
using bridge::sta_tcp_tx_release;
#endif
static bool active;
static int fragment_observer_handle;
static uint32_t fragment_confirmed, fragment_acknowledged, fragment_discarded;
constexpr unsigned kEthernetHeaderBytes=14,kIpv4MinimumHeaderBytes=20;
constexpr unsigned kEthernetTypeIpv4=0x800,kIpv4ProtocolOffset=23,kIpv4ProtocolUdp=17;
#define TX_STATUS_ACKNOWLEDGED 1U
#define PBUF_LINK_ENCAPSULATION_HLEN 388
extern "C" void bridge_wifi_tx_release_observer(uintptr_t,uint32_t,uint32_t) __attribute__((weak));
extern "C" void bridge_wifi_tx_discard_site_observer(uintptr_t,uintptr_t) __attribute__((weak));
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static uint16_t read_be16(const uint8_t *p) { return (p[0]<<8)|p[1]; }
static unsigned pbuf_copy_partial(pbuf *p,void *dest,unsigned n,unsigned off) {
 if (off>=p->size) return 0;
 n=std::min(n,p->size-off);memcpy(dest,p->data+off,n);return n;
}
static void net_buf_tx_free(void *) { assert(stage==2);stage=3;++releases; }
static void fhost_tx_release_buf_rx(fhost_rx_buf_tag *) { assert(stage==3);stage=4; }
static void callback(uintptr_t,uint32_t,void *) {
#ifdef NO_OBSERVER
 assert(stage==0);
#else
 assert(stage==1);
#endif
 stage=2;
}
'''
MAIN = r'''
int main() {
#ifdef NO_OBSERVER
 stage=0;fhost_tx_release_buf(nullptr,1,callback,nullptr,nullptr);
 assert(stage==4 && releases==1);return 0;
#else
 pbuf packet={}; packet.size=40; packet.data[12]=8;packet.data[14]=0x45;
 packet.data[20]=0x20;packet.data[23]=17;
 fragment_observer_handle=5;
 for (unsigned status: {0U,1U,2U}) {
   stage=0;fragment_confirmed=fragment_acknowledged=fragment_discarded=0;
   fhost_tx_release_buf(&packet,status,callback,nullptr,nullptr);
   assert(stage==4 && fragment_confirmed==1);
   assert(fragment_acknowledged==(status==1));
   assert(fragment_discarded==(status==0));
 }
 // Real SDK net_buf_tx_info leaves payload at the 388-byte TX headroom.
 memcpy(packet.data+388,packet.data,40);memset(packet.data,0,388);packet.size=428;
 fragment_confirmed=fragment_acknowledged=fragment_discarded=0;
 stage=0;fhost_tx_release_buf(&packet,1,callback,nullptr,nullptr);
 assert(fragment_confirmed==1 && fragment_acknowledged==1);
 memcpy(packet.data,packet.data+388,40);packet.size=40;
 // Invoke the actual observer on non-UDP, unfragmented, short, inactive frames.
 fragment_confirmed=0;
 packet.data[23]=6; bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 packet.data[23]=17;packet.data[20]=0;bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 packet.data[20]=0x20;packet.size=20;bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 packet.size=40;fragment_observer_handle=-1;bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(fragment_confirmed==0 && releases==4);
#if BL616CL_STA_TCP_DIAGNOSTIC
 assert(tcp_release_calls==8);
#endif
 // SYNs count independently of UDP observer activity; SYN-ACK and other
 // packets do not count. Validate both SDK headroom and IPv4 options.
 auto &syn=wifi_syn_counters();
 packet={};packet.size=54;packet.data[12]=8;packet.data[14]=0x45;
 packet.data[0]=0x22;packet.data[6]=0x44;
 packet.data[23]=6;packet.data[47]=2;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(syn.confirmed.load()==1 && syn.acknowledged.load()==1);
 packet.data[47]=0x12;bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(syn.confirmed.load()==1);
 packet.data[47]=2;packet.size=47;bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(syn.confirmed.load()==1);
 packet.size=58;packet.data[14]=0x46;packet.data[51]=2;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,0,1);
 assert(syn.confirmed.load()==2 && syn.discarded.load()==1);
 memcpy(packet.data+388,packet.data,58);memset(packet.data,0,388);packet.size=446;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(syn.confirmed.load()==3 && syn.acknowledged.load()==2);
 #ifndef NO_SITE_OBSERVER
 // The optional discard hook reports only SYN sites, without freeing data.
 bridge_wifi_tx_discard_site_observer((uintptr_t)&packet,0x1234);
 assert(syn.last_discard_site.load()==0x1234 && releases==4);
 assert(syn.last_discard_destination[0]==0x22 && syn.last_discard_source[0]==0x44);
 packet.data[388+51]=0x12;
 bridge_wifi_tx_discard_site_observer((uintptr_t)&packet,0x5678);
 assert(syn.last_discard_site.load()==0x1234 && releases==4);
 #endif
 // DHCP ACK classification must remain passive and only count transmissions
 // while the experimental join window is active, including SDK headroom.
 packet={};packet.size=286;packet.data[12]=8;packet.data[14]=0x45;
 packet.data[16]=1;packet.data[17]=16;packet.data[23]=17;
 packet.data[35]=67;packet.data[37]=68;packet.data[38]=0;packet.data[39]=252;
 packet.data[42]=2;packet.data[43]=1;packet.data[44]=6;
 packet.data[278]=99;packet.data[279]=130;packet.data[280]=83;packet.data[281]=99;
 packet.data[282]=53;packet.data[283]=1;packet.data[284]=5;packet.data[285]=255;
 assert(bridge::dhcp_server_message_type(packet.data,packet.size)==5);
 packet.data[284]=2;assert(bridge::dhcp_server_message_type(packet.data,packet.size)==2);
 assert(!is_dhcp_ack_packet(packet.data,packet.size));
 packet.data[284]=6;assert(bridge::dhcp_server_message_type(packet.data,packet.size)==6);
 packet.data[284]=9;assert(bridge::dhcp_server_message_type(packet.data,packet.size)==0);
 packet.data[284]=5;
 auto &join=ap_join_scan_counters();join.active=0;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,0,0);
 assert(join.dhcp_ack_transmitted.load()==0);
#if BL616CL_DHCP_TX_DIAGNOSTIC
 auto &dhcp=bridge::dhcp_tx_diagnostic();
 assert(dhcp.released[1]==1 && dhcp.discarded[1]==0 && dhcp.acknowledged[1]==0);
#endif
 join.active=1;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,0,1);
 assert(join.dhcp_ack_transmitted.load()==0);
 bridge_wifi_tx_release_observer((uintptr_t)&packet,0,0);
 assert(join.dhcp_ack_transmitted.load()==1);
 packet.data[284]=2;bridge_wifi_tx_release_observer((uintptr_t)&packet,0,0);
 assert(join.dhcp_ack_transmitted.load()==1);
 packet.data[284]=5;packet.size=284;bridge_wifi_tx_release_observer((uintptr_t)&packet,0,0);
 assert(join.dhcp_ack_transmitted.load()==1);
 packet.size=286;memcpy(packet.data+388,packet.data,286);memset(packet.data,0,388);packet.size=674;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,0,0);
 assert(join.dhcp_ack_transmitted.load()==2);
#if BL616CL_DHCP_TX_DIAGNOSTIC
 assert(dhcp.released[1]==4 && dhcp.discarded[1]==1 && dhcp.acknowledged[1]==0);
 assert(dhcp.released[0]==1 && dhcp.discarded[0]==0);
 packet.data[388+284]=6;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(dhcp.released[2]==1 && dhcp.acknowledged[2]==1 && dhcp.discarded[2]==0);
 uint8_t report[64]={};bridge::dhcp_tx_diagnostic_report(report);
 assert(report[5]==52 && report[6]==1 && report[7]==1);
 assert(report[8]==1 && report[20]==4 && report[24]==1 && report[32]==1 && report[40]==1);
#endif
 // Releasing a broadcast DHCP ACK must not end the priority window. Require
 // a MAC-acknowledged non-DHCP IP reply with the configured AP source address.
 packet={};packet.size=54;packet.data[12]=8;packet.data[14]=0x45;
 packet.data[17]=40;packet.data[23]=6;
 packet.data[26]=192;packet.data[27]=168;packet.data[28]=4;packet.data[29]=1;
 packet.data[30]=192;packet.data[31]=168;packet.data[32]=4;packet.data[33]=100;
 uint32_t ap_ip;memcpy(&ap_ip,packet.data+26,4);join.ap_ipv4=ap_ip;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,0,0);
 assert(join.client_ip_reply_acknowledged==0);
 bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(join.client_ip_reply_acknowledged==1);
 packet.data[29]=2;bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(join.client_ip_reply_acknowledged==1);
 packet.data[29]=1;packet.data[23]=17;packet.data[35]=67;packet.data[37]=68;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(join.client_ip_reply_acknowledged==1);
 packet.data[35]=53;packet.data[37]=100;
 bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(join.client_ip_reply_acknowledged==2);
 packet.data[30]=255;bridge_wifi_tx_release_observer((uintptr_t)&packet,1,0);
 assert(join.client_ip_reply_acknowledged==2);

 return 0;
#endif
}
'''

def main():
    with tempfile.TemporaryDirectory() as directory:
        folder=Path(directory);target=folder/TARGET;target.parent.mkdir(parents=True)
        target.write_bytes((ROOT/'third_party/bouffalo_sdk'/TARGET).read_bytes())
        subprocess.run(['patch','--batch','-p1','-i',str(PROFILE/'0066-bridge-passive-tx-confirm-observer.patch')],cwd=folder,check=True,stdout=subprocess.DEVNULL)
        subprocess.run(['patch','--batch','-p1','-i',str(PROFILE/'0067-bridge-passive-discard-site.patch')],cwd=folder,check=True,stdout=subprocess.DEVNULL)
        release=function(target.read_text(),'void fhost_tx_release_buf(')
        backend=(ROOT/'UNOR4BL616CLBridge/bouffalo_udp_backend.cpp').read_text()
        observer=function(backend,'static bool bridge_wifi_buffer_is_tcp_syn(') + '\n' + function(backend,'extern "C" void bridge_wifi_tx_release_observer(')
        # Wrap the observer to assert it runs before the original callback/free.
        observer=observer.replace('bridge_wifi_tx_release_observer(', 'actual_observer(',1)
        wrapper='''#ifndef NO_OBSERVER
extern "C" void bridge_wifi_tx_release_observer(uintptr_t p,uint32_t a,uint32_t d) {
 if (stage==0) { ++observations;stage=1; } actual_observer(p,a,d);
}
#endif
'''
        site_observer=function(backend,'extern "C" void bridge_wifi_tx_discard_site_observer(')
        source=folder/'confirm.cpp';source.write_text(PREFIX+observer+wrapper+'\n#ifndef NO_SITE_OBSERVER\n'+site_observer+'\n#endif\n'+release+MAIN)
        for flags in ([], ['-DBL616CL_DHCP_TX_DIAGNOSTIC=1'], ['-DBL616CL_STA_TCP_DIAGNOSTIC=1'], ['-DNO_SITE_OBSERVER'], ['-DNO_OBSERVER', '-DNO_SITE_OBSERVER']):
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-I'+str(ROOT/'UNOR4BL616CLBridge'),*flags,str(source),'-o',str(folder/'confirm')],check=True)
            subprocess.run([str(folder/'confirm')],check=True)
    print('BL616CL_PASSIVE_TX_CONFIRM_PASS')

if __name__=='__main__':main()
