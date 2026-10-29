#include "sta_tcp_diagnostic.h"
#include <cassert>
#include <cstring>
#include <vector>
using bridge::StaTcpDiagnostic;
void be16(uint8_t *p, unsigned n) {p[0]=n>>8;p[1]=n;}
void be32(uint8_t *p, uint32_t n) {be16(p,n>>16);be16(p+2,n);}
std::vector<uint8_t> packet(bool tx, uint32_t seq, uint32_t ack, uint8_t flags, unsigned n=0, unsigned options=0) {
    std::vector<uint8_t> p(54+options+n); be16(&p[12],0x800);p[14]=0x45+options/4;
    be16(&p[16],40+options+n);p[23]=6;be32(&p[26],tx?1:2);be32(&p[30],tx?2:1);
    auto *t=p.data()+34+options;
    be16(t,tx?1234:BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT);
    be16(t+2,tx?BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT:1234);
    be32(t+4,seq);be32(t+8,ack);t[12]=0x50;t[13]=flags;be16(t+14,8000);return p;
}
void observe(StaTcpDiagnostic &d,bool tx,const std::vector<uint8_t>&p) {
    auto before=p;
    d.observe(tx,p.size(),[&](uint8_t *out,size_t n,size_t offset){
        if(offset>p.size()||n>p.size()-offset)return size_t(0);
        memcpy(out,p.data()+offset,n);return n;
    });assert(p==before);
}
int main() {
    StaTcpDiagnostic d;
    observe(d,false,packet(false,100,0,24,512));assert(d.rx_packets==0);
    observe(d,true,packet(true,10,0,2));assert(d.generation==1);
    auto foreign=packet(true,20,0,2);
    be16(foreign.data()+36,BL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT==53?18444:53);
    observe(d,true,foreign);assert(d.generation==1);
    const auto release = [&](const std::vector<uint8_t> &p, unsigned status) {
        d.observe(true,p.size(),[&](uint8_t *out,size_t n,size_t offset) {
            if(offset>p.size()||n>p.size()-offset)return size_t(0);
            memcpy(out,p.data()+offset,n);return n;
        },status);
    };
    auto events=d.event_count;
    release(packet(true,11,101,16),3);
    release(packet(true,11,102,16),1);
    release(packet(true,11,103,16),5);
    assert(d.released==3&&d.acknowledged==1&&d.discarded==1);
    assert(d.released_ack==103&&d.acknowledged_ack==101&&d.discarded_ack==103);
    assert(d.event_count==events&&d.tx_acks==0&&d.rx_packets==0);
    auto stale=packet(true,11,200,16);be16(stale.data()+34,1235);
    release(stale,3);release(packet(true,20,0,2),3);
    assert(d.released==3&&d.generation==1);
    observe(d,true,packet(true,10,0,2));assert(d.generation==1);
    observe(d,false,packet(false,100,11,18));assert(d.highest_end==101);
    observe(d,true,packet(true,11,101,16));
    observe(d,false,packet(false,101,11,24,512,12));
    observe(d,true,packet(true,11,613,16));
    observe(d,false,packet(false,1125,11,24,512));
    observe(d,true,packet(true,11,613,16));
    assert(d.rx_packets==3&&d.rx_bytes==1024&&d.highest_end==1637);
    assert(d.last_ack==613&&d.repeated_acks==1&&d.last_window==8000);
    observe(d,false,packet(false,613,11,24,512));
    assert(d.highest_end==1637&&d.last_seq==613);
    auto bad=packet(false,1,1,24,1);bad[20]=0x20;observe(d,false,bad);assert(d.invalid_frames==1);
    bad=packet(false,1,1,24,1);bad.resize(54);observe(d,false,bad);assert(d.invalid_frames==2);
    bad=packet(false,1,1,24,1);bad[46]=0x10;observe(d,false,bad);assert(d.invalid_frames==3);
    auto other=packet(false,1,1,24,512);other[27]^=1;auto count=d.rx_packets;
    observe(d,false,other);assert(d.rx_packets==count);
    observe(d,true,packet(true,11,0,2));assert(d.generation==2&&d.rx_packets==0);
    observe(d,false,packet(false,0xffffff00,12,18));
    observe(d,false,packet(false,0xffffff01,12,24,512));assert(d.highest_end==257);
    observe(d,false,packet(false,257,12,17));assert(d.highest_end==258);
    for(size_t length=0;length<54;++length)observe(d,false,std::vector<uint8_t>(length));
    assert(d.rx_packets==3);
}
