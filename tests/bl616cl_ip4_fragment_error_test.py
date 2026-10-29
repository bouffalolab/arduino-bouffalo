#!/usr/bin/env python3
"""Inject netif errors into the actual SDK IPv4 fragmentation function."""
import json
from pathlib import Path
import subprocess
import tempfile
from bl616cl_sta_disconnect_wait_test import function

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches'
TARGET = 'components/net/lwip/lwip/src/core/ipv4/ip4_frag.c'
PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int err_t;
typedef int ip4_addr_t;
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define BL616CL_BRIDGE_IP_FRAG_RETRY 1
#define CHECKSUM_GEN_IP 0
#define IP_HLEN 20
#define IP_OFFMASK 0x1fff
#define IP_MF 0x2000
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_VAL -6
#define PBUF_IP 0
#define PBUF_RAM 0
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#define LWIP_ASSERT(s,x) assert(x)
#define LWIP_ERROR(s,x,action) do { if (!(x)) { action; } } while(0)
#define SMEMCPY memcpy
#define lwip_ntohs ntohs
#define lwip_htons htons
struct ip_hdr { uint8_t version, tos; u16_t len, id, offset, checksum; uint8_t rest[10]; };
#define IPH_HL_BYTES(h) (((h)->version & 15)*4)
#define IPH_OFFSET(h) ((h)->offset)
#define IPH_OFFSET_SET(h,v) ((h)->offset=(v))
#define IPH_LEN_SET(h,v) ((h)->len=(v))
#define IPH_CHKSUM_SET(h,v) ((h)->checksum=(v))
struct pbuf { void *payload; u16_t len, tot_len; struct pbuf *next; void *allocation; };
struct netif { u16_t mtu; err_t (*output)(struct netif *,struct pbuf *,const ip4_addr_t *); };
static struct { int xmit, err; } ip_frag;
static struct { int ipfragoks, ipfragfails; } mib2;
#define IPFRAG_STATS_INC(x) (++(x))
#define MIB2_STATS_INC(x) (++(x))
static int live, calls, fail_at, failure, allocation_fail;
static unsigned char *original_data;
static unsigned checked_payload_bytes;
static u32_t ticks;
static u32_t sys_now(void) { return ticks; }
static void sys_msleep(u32_t n) { ticks+=n; }
static struct pbuf *pbuf_alloc(int layer,u16_t n,int type) {
 (void)layer; (void)type;
 if (allocation_fail) return NULL;
 struct pbuf *p=calloc(1,sizeof(*p)); assert(p);
 p->allocation=calloc(1,n+IP_HLEN); assert(p->allocation);
 p->payload=(char *)p->allocation+IP_HLEN; p->len=p->tot_len=n; ++live; return p;
}
static u16_t pbuf_copy_partial(struct pbuf *p,void *dest,u16_t n,u16_t offset) {
 memcpy(dest,(char *)p->payload+offset,n); return n;
}
static int pbuf_add_header(struct pbuf *p,u16_t n) {
 p->payload=(char *)p->payload-n; p->len+=n;p->tot_len+=n;return 0;
}
static void pbuf_free(struct pbuf *p) { free(p->allocation);free(p);--live; }
static err_t output(struct netif *n,struct pbuf *p,const ip4_addr_t *d) {
 (void)n;(void)d;assert(p->len<=1500);
 struct ip_hdr *header=p->payload;
 unsigned flags=ntohs(header->offset);
 unsigned offset=(flags & IP_OFFMASK)*8U;
 unsigned bytes=p->len-IP_HLEN;
 assert(ntohs(header->len)==p->len);
 assert(offset==checked_payload_bytes);
 assert(offset+bytes<=16412-IP_HLEN);
 assert(memcmp((char *)p->payload+IP_HLEN, original_data+IP_HLEN+offset,bytes)==0);
 assert(header->id==((struct ip_hdr *)original_data)->id);
 assert(!!(flags & IP_MF)==(offset+bytes<16412-IP_HLEN));
 checked_payload_bytes+=bytes;
 return ++calls==fail_at?failure:ERR_OK;
}
'''
MAIN = r'''
int main(void) {
 unsigned char data[16412]={0}; ((struct ip_hdr *)data)->version=0x45;
 ((struct ip_hdr *)data)->id=htons(0x1234);
 for (unsigned i=IP_HLEN;i<sizeof(data);++i) data[i]=(unsigned char)(i*19U+7U);
 original_data=data;
 struct pbuf input={data,sizeof(data),sizeof(data),NULL,NULL};
 struct netif net={1500,output};
 for (int error=-1;error>=-12;--error) {
  for (int at=0;at<=12;++at) {
   checked_payload_bytes=0;calls=live=ticks=0;ip_frag.xmit=ip_frag.err=mib2.ipfragoks=mib2.ipfragfails=0;
   fail_at=at;failure=error;
   int result=ip4_frag(&input,&net,NULL);
   assert(live==0);
   assert(result==(at?error:ERR_OK));
   assert(calls==(at?at:12));
   if (!at) assert(checked_payload_bytes==sizeof(data)-IP_HLEN);
   assert(ip_frag.xmit==(at?at-1:12));
   assert(ip_frag.err==(at?1:0));
   assert(mib2.ipfragfails==(at?1:0));
   assert(mib2.ipfragoks==(at?0:1));
  }
 }
 allocation_fail=1;calls=0;ticks=0;
 assert(ip4_frag(&input,&net,NULL)==ERR_MEM);
 assert(live==0 && calls==0 && ticks==100);
 return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        target = folder / TARGET
        target.parent.mkdir(parents=True)
        target.write_bytes((ROOT / 'third_party/bouffalo_sdk' / TARGET).read_bytes())
        for record in json.loads((PROFILE / 'manifest.json').read_text())['patches']:
            if any(t['path'] == TARGET for t in record['targets']):
                subprocess.run(['patch', '--batch', '-p1', '-i', str(PROFILE / record['path'])],
                               cwd=folder, check=True, stdout=subprocess.DEVNULL)
        source = PREFIX + function(target.read_text(), 'err_t\nip4_frag(') + MAIN
        harness = folder / 'fragment.c'
        harness.write_text(source)
        binary = folder / 'fragment'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(harness), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('BL616CL_IP4_FRAGMENT_ERROR_PASS')

if __name__ == '__main__':
    main()
