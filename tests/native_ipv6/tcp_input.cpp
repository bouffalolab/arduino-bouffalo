// Complete native IPv6/TCP processing; only the radio and output link are simulated.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include "lwip/ip6.h"
#include "lwip/ip6_frag.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
struct stats_ lwip_stats;
void *test_detach(int, void *);
u32_t sys_now(void) { return 1000; }
}
#include "tcp_rx_native_policy.h"
static netif iface;
static tcp_pcb *accepted;
static pbuf *held;
static unsigned radio_freed, deliveries, outputs;
static uint32_t server_seq;
static uint8_t source[16] = {0x20,1,0xd,0xb8,0,0,0,0,0,0,0,0,0,0,0,1};
static uint8_t dest[16] = {0x20,1,0xd,0xb8,0,0,0,0,0,0,0,0,0,0,0,2};
static unsigned word(const uint8_t *p) {
  return unsigned(p[0]) | unsigned(p[1])<<8 | unsigned(p[2])<<16 | unsigned(p[3])<<24;
}
static unsigned counter(unsigned index) {
  uint8_t report[64] = {}; bridge::tcp_rx_memory_report(report);
  assert(!word(report+36)); return word(report+8+4*index);
}
static void put16(uint8_t *p, unsigned v) { p[0]=v>>8; p[1]=v; }
static void put32(uint8_t *p, uint32_t v) { put16(p,v>>16); put16(p+2,v); }
static uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | unsigned(p[2])<<8 | p[3];
}
static unsigned checksum(const uint8_t *p, unsigned n, unsigned sum=0) {
  while(n>=2) { sum+=(unsigned(p[0])<<8)|p[1]; p+=2; n-=2; }
  if(n) sum+=unsigned(*p)<<8;
  while(sum>>16) sum=(sum&65535)+(sum>>16);
  return sum;
}
static std::vector<uint8_t> segment(uint32_t seq, unsigned flags, unsigned size,
                                    unsigned offset=0, unsigned port=9000) {
  std::vector<uint8_t> b(20+size);
  put16(b.data(),50000); put16(b.data()+2,port);
  put32(b.data()+4,seq); put32(b.data()+8,server_seq+1);
  b[12]=0x50; b[13]=flags; put16(b.data()+14,4096);
  for(unsigned i=0;i<size;++i) b[20+i]=uint8_t(offset+i);
  unsigned sum=checksum(source,16)+checksum(dest,16)+b.size()+6;
  put16(b.data()+16,~checksum(b.data(),b.size(),sum));
  return b;
}
static err_t output(netif *,pbuf *p,const ip6_addr_t *) {
  std::vector<uint8_t> b(p->tot_len);
  assert(pbuf_copy_partial(p,b.data(),b.size(),0)==b.size());
  ++outputs;
  if(b.size()>=60 && b[6]==6 && (b[53]&TCP_SYN)) server_seq=get32(b.data()+44);
  return ERR_OK;
}
static void radio_free(pbuf *) { ++radio_freed; }
static void input(const std::vector<uint8_t>& tcp, int sta, int extension,
                  unsigned id=0, unsigned start=0, unsigned count=0, bool more=false) {
  const bool frag=id!=0;
  if(!count) count=tcp.size();
  unsigned headers=40+(extension?8:0)+(frag?8:0);
  std::vector<uint8_t> frame(14+headers+count);
  frame[12]=0x86; frame[13]=0xdd;
  uint8_t *ip=frame.data()+14; ip[0]=0x60;
  put16(ip+4,headers-40+count); ip[6]=extension?60:(frag?44:6); ip[7]=64;
  memcpy(ip+8,source,16); memcpy(ip+24,dest,16);
  if(extension) ip[40]=frag?44:6;
  if(frag) { uint8_t *f=ip+40+(extension?8:0); f[0]=6;
    put16(f+2,start|(more?1:0)); put32(f+4,id); }
  assert(start+count<=tcp.size()); memcpy(ip+headers,tcp.data()+start,count);
  pbuf_custom radio={}; radio.custom_free_function=radio_free;
  pbuf *p=pbuf_alloced_custom(PBUF_RAW,frame.size(),PBUF_REF,&radio,frame.data(),frame.size());
  unsigned before=radio_freed;
  p=static_cast<pbuf *>(test_detach(sta,p));
  assert(p && radio_freed==before+1);
  assert(!pbuf_remove_header(p,14));
  assert(ip6_input(p,&iface)==ERR_OK);
  assert(bridge::tcp_rx_native_policy_healthy());
}
static err_t receive(void *,tcp_pcb *,pbuf *p,err_t err) {
  assert(err==ERR_OK && p && !held); held=p; ++deliveries; return ERR_OK;
}
static err_t accept(void *,tcp_pcb *pcb,err_t err) {
  assert(err==ERR_OK && !accepted); accepted=pcb; tcp_recv(pcb,receive); return ERR_OK;
}
static void consume(unsigned size,unsigned offset) {
  assert(held && held->tot_len==size);
  std::vector<uint8_t> b(size);
  assert(pbuf_copy_partial(held,b.data(),size,0)==size);
  for(unsigned i=0;i<size;++i) assert(b[i]==uint8_t(offset+i));
  assert(counter(1)>size && counter(6)>0);
  pbuf_free(held);held=nullptr;tcp_recved(accepted,size);
  assert(!counter(1) && !counter(6));
}
int main() {
  mem_init();memp_init();tcp_init();
  iface.flags=NETIF_FLAG_UP|NETIF_FLAG_LINK_UP;iface.mtu=1500;
  iface.output_ip6=output;ip6_addr_t address;memcpy(&address,dest,16);
  netif_ip6_addr_set(&iface,0,&address);
  netif_ip6_addr_set_state(&iface,0,IP6_ADDR_PREFERRED);
  netif_list=netif_default=&iface;
  auto *pcb=tcp_new_ip_type(IPADDR_TYPE_V6);assert(pcb);
  assert(tcp_bind(pcb,IP6_ADDR_ANY,9000)==ERR_OK);
  auto *listener=tcp_listen(pcb);assert(listener);tcp_accept(listener,accept);
  const uint32_t initial=0xffffff00U;
  input(segment(initial,TCP_SYN,0),1,0);
  assert(server_seq && !accepted);
  input(segment(initial+1,TCP_ACK,0),1,0);assert(accepted && accepted->state==ESTABLISHED);
  assert(!counter(1));
  uint32_t seq=initial+1;
  for(int sta=0;sta<2;++sta) for(int ext=0;ext<2;++ext) for(int reverse=0;reverse<2;++reverse) {
    auto data=segment(seq,TCP_ACK|TCP_PSH,128);
    unsigned first=reverse?64:0,second=reverse?0:64;
    unsigned old_deliveries=deliveries,old_admitted=counter(8);
    input(data,sta,ext,100+deliveries,first,reverse?84:64,!reverse);
    assert(!held && deliveries==old_deliveries && !counter(1) && counter(8)==old_admitted);
    assert(lwip_stats.mem.used>0); // incomplete reassembly is shared, not owner-bound
    input(data,sta,ext,100+deliveries,second,reverse?64:84,reverse);
    assert(held && deliveries==old_deliveries+1 && counter(8)==old_admitted+1);
    consume(128,0);seq+=128;
  }
  // Real TCP out-of-order queue holds its owner charge. A duplicate must
  // release only the new allocation; filling the gap delivers both buffers.
  unsigned before=deliveries;
  input(segment(seq+64,TCP_ACK,64,64),1,1);
  assert(!held && deliveries==before && counter(1)>64);
  unsigned queued=counter(1);
  input(segment(seq+64,TCP_ACK,64,64),1,1);
  assert(!held && counter(1)==queued);
  input(segment(seq,TCP_ACK,64),1,1);
  assert(held && deliveries==before+1);consume(128,0);seq+=128;
  // Consume the minimum to obtain discretionary credit, then accept a larger
  // native two-fragment data chain without changing the production cap.
  input(segment(seq,TCP_ACK,256),0,0);consume(256,0);seq+=256;
  assert(accepted->rcv_wnd>=700);
  auto large=segment(seq,TCP_ACK,700);
  input(large,0,1,500,512,208,false);
  assert(!held && !counter(1));
  input(large,0,1,500,0,512,true);
  consume(700,0);seq+=700;
  // Sixteen tiny allocations exhaust this owner's physical block quota.
  // The denied packet must not advance native rcv_nxt; the same sequence is
  // accepted after the application releases the old allocations.
  std::vector<pbuf *> retained;
  for(unsigned i=0;i<16;++i) {
    input(segment(seq,TCP_ACK,1,i),1,0);assert(held);
    retained.push_back(held);held=nullptr;++seq;
  }
  unsigned denied=counter(9);before=deliveries;unsigned output_before=outputs;
  input(segment(seq,TCP_ACK,1),1,0);
  assert(!held && deliveries==before && counter(9)==denied+1 && accepted->rcv_nxt==seq && outputs==output_before);
  for(auto *p:retained) pbuf_free(p);
  tcp_recved(accepted,16);assert(!counter(1));
  input(segment(seq,TCP_ACK,1),1,0);consume(1,0);++seq;
  // TCP checksum rejection and unmatched port must never bind to an owner.
  unsigned admitted=counter(8),delivered=deliveries;
  auto bad=segment(seq,TCP_ACK,32);bad.back()^=1;input(bad,1,1);
  input(segment(seq,TCP_ACK,32,0,9001),0,1);
  assert(deliveries==delivered && counter(8)==admitted && !counter(1));
  // Partial consumption and an extra native reference retain full allocation
  // charge. Abort retires the owner until the final application reference dies.
  input(segment(seq,TCP_ACK,128),1,1);assert(held);
  pbuf *extra=held;pbuf_ref(extra);unsigned charge=counter(1);
  held=pbuf_free_header(held,64);tcp_recved(accepted,64);
  assert(held && held->tot_len==64 && counter(1)==charge);
  pbuf_free(held);held=nullptr;tcp_recved(accepted,64);
  assert(counter(1)==charge);
  tcp_abort(accepted);accepted=nullptr;
  assert(counter(5)==1 && counter(1)==charge);
  pbuf_free(extra);assert(!counter(5) && !counter(1));
  assert(tcp_close(listener)==ERR_OK);
  ip6_reass_cleanup();
  assert(!counter(0) && !counter(1) && !counter(4) && !counter(5) && !counter(6));
  assert(!lwip_stats.mem.used && !lwip_stats.mem.err);
  printf("PASS full native ip6_input/TCP: handshake, 9 fragmented STA/AP deliveries, TCP out-of-order/duplicate, wrap, quota denial/recovery, partial/retired owner release; radio=%u output=%u\n",radio_freed,outputs);
}
