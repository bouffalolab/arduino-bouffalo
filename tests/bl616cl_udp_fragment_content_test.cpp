#include "udp_fragment_content_diagnostic.h"
#include <cassert>
#include <cstring>
#include <vector>
using bridge::UdpFragmentContentDiagnostic;
static void be16(uint8_t *p, unsigned value) { p[0]=value>>8; p[1]=value; }
static std::vector<uint8_t> frame(const std::vector<uint8_t> &data, size_t index) {
    size_t offset=index*1480, n=data.size()+8-offset;
    if(n>1480)n=1480;
    std::vector<uint8_t> f(34+n);
    be16(f.data()+12,0x800);f[14]=0x45;f[23]=17;
    be16(f.data()+16,20+n);be16(f.data()+18,4321);
    be16(f.data()+20,offset/8+(offset+n<data.size()+8?0x2000:0));
    f[30]=192;f[31]=0;f[32]=2;f[33]=1;
    if(index==0) {
        be16(f.data()+36,18448);be16(f.data()+38,data.size()+8);
        memcpy(f.data()+42,data.data(),n-8);
    } else memcpy(f.data()+34,data.data()+offset-8,n);
    unsigned sum=0;
    for(size_t i=14;i<34;i+=2)sum+=UdpFragmentContentDiagnostic::be16(f.data()+i);
    while(sum>>16)sum=(sum&65535)+(sum>>16);
    be16(f.data()+24,(~sum)&65535);
    return f;
}
static void observe(UdpFragmentContentDiagnostic &d,const std::vector<uint8_t> &f,size_t head=0) {
    std::vector<uint8_t> padded(head+f.size());memcpy(padded.data()+head,f.data(),f.size());
    d.observe([&](uint8_t *out,size_t n,size_t offset){
        if(offset>padded.size())return size_t(0);
        if(n>padded.size()-offset)n=padded.size()-offset;
        memcpy(out,padded.data()+offset,n);return n;
    },head);
}
int main() {
    uint8_t destination[]={192,0,2,1};std::vector<uint8_t> payload(16384);
    for(size_t i=0;i<payload.size();++i)payload[i]=(i*19+7)&255;
    for(size_t head: {size_t(0),size_t(128)}) {
        UdpFragmentContentDiagnostic d;d.prepare(payload.data(),payload.size(),destination,18448);
        for(size_t i=0;i<12;++i)observe(d,frame(payload,i),head);
        assert(d.active==1&&d.checked==12&&d.mask==4095);
        assert(d.header_errors==0&&d.payload_errors==0&&d.identification==4321);
    }
    UdpFragmentContentDiagnostic d;d.prepare(payload.data(),payload.size(),destination,18448);
    auto first=frame(payload,0);observe(d,first);
    auto bad=frame(payload,1);bad[40]^=1;observe(d,bad);
    assert(d.payload_errors==1&&d.header_errors==0);
    bad=frame(payload,2);bad[24]^=1;observe(d,bad);
    assert(d.header_errors==1);
    bad=frame(payload,3);bad.resize(40);observe(d,bad);
    assert(d.payload_errors==2);
    d.prepare(payload.data(),payload.size(),destination,18448);
    bad=frame(payload,0);be16(bad.data()+40,1);observe(d,bad);
    assert(d.udp_checksum_errors==1&&d.udp_checksum_residual!=0);
    d.prepare(payload.data(),payload.size(),destination,18448);
    unsigned udp_sum=d.payload_sum+17+UdpFragmentContentDiagnostic::be16(bad.data()+38);
    be16(bad.data()+40,0);
    for(size_t i=26;i<42;i+=2)udp_sum+=UdpFragmentContentDiagnostic::be16(bad.data()+i);
    while(udp_sum>>16)udp_sum=(udp_sum&65535)+(udp_sum>>16);
    be16(bad.data()+40,(~udp_sum)&65535);observe(d,bad);
    assert(d.udp_checksum_errors==0&&d.udp_checksum_residual==0);
    auto other=frame(payload,4);other[33]^=1;unsigned checked=d.checked;observe(d,other);
    assert(d.checked==checked);
    d.prepare(payload.data(),1472,destination,18448);assert(d.active==0);
    d.prepare(payload.data(),16385,destination,18448);assert(d.active==0);
}
