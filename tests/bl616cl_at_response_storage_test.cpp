#include "at_transport.h"
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>

using namespace bridge::at;
static unsigned allocations, releases;
static bool fail_allocation;
extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" void *__wrap_malloc(size_t size) {
    ++allocations;
    return fail_allocation ? nullptr : __real_malloc(size);
}
extern "C" void __wrap_free(void *data) {
    if (data) ++releases;
    __real_free(data);
}
static_assert(sizeof(AtTransport) < kTransportRawPayloadCapacity + 5500U,
              "response metadata must replace the permanent 17 KiB copy");
struct State {
    uint8_t output[kTransportResponseCapacity] = {};
    uint8_t body[kTransportRawPayloadCapacity] = {};
    size_t size=0, limit=3;
    unsigned mode=0;
    AtTransport *transport=nullptr;
};
size_t output(void *context, const uint8_t *data, size_t size) {
    auto &s=*static_cast<State *>(context);
    size_t count=size<s.limit ? size : s.limit;
    assert(s.size+count<=sizeof(s.output));
    memcpy(s.output+s.size,data,count); s.size+=count; return count;
}
CommandResult raw(void *context,const uint8_t *data,size_t size) {
    auto &s=*static_cast<State *>(context);
    assert(size==sizeof(s.body));
    assert(s.transport->write_data(data,size));
    assert(memcmp(data,s.body,size)==0); // Owning writes preserve input bytes.
    return CommandResult::Ok;
}
CommandResult command(void *context,const Command &,AtTransport &t) {
    auto &s=*static_cast<State *>(context); s.transport=&t;
    if(s.mode==4) {
        assert(t.request_raw_payload(sizeof(s.body),raw,&s));
        return CommandResult::Deferred;
    }
    if(s.mode==3) {
        assert(t.write_data(s.body,sizeof(s.body)));
        return CommandResult::Deferred; // Discard any provisional response.
    }
    assert(t.write_cstr("+R: 16384|"));
    if(s.mode==1) {
        (void)t.write_data(s.body,sizeof(s.body));
    } else {
        uint8_t *workspace=t.temporary_workspace(sizeof(s.body)); assert(workspace);
        memcpy(workspace,s.body,sizeof(s.body));
        (void)t.write_borrowed_data(workspace,sizeof(s.body));
    }
    (void)t.write_cstr("\r\n");
    return CommandResult::Ok;
}
void drain(AtTransport &t) {
    unsigned calls=0;
    while(t.output_pending()) { t.flush_output(); assert(++calls<100000); }
}
AtTransport::Config config(size_t capacity=kTransportResponseCapacity) {
    return {1023,kTransportRawPayloadCapacity,capacity,1000};
}
void initialize(State &s) {
    for(size_t i=0;i<sizeof(s.body);++i) s.body[i]=static_cast<uint8_t>(i);
}
void verify(const State &s) {
    constexpr char header[]="+R: 16384|", tail[]="\r\nOK\r\n";
    assert(s.size==sizeof(header)-1+sizeof(s.body)+sizeof(tail)-1);
    assert(memcmp(s.output,header,sizeof(header)-1)==0);
    assert(memcmp(s.output+sizeof(header)-1,s.body,sizeof(s.body))==0);
    assert(memcmp(s.output+sizeof(header)-1+sizeof(s.body),tail,sizeof(tail)-1)==0);
}
int main() {
    constexpr char request[]="AT+READ\r\n";
    { // A full payload works under heap failure and UART backpressure.
        State s; initialize(s); AtTransport t(config(),output,&s);
        t.set_command_handler(command,&s); allocations=releases=0; fail_allocation=true;
        assert(t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,0)==sizeof(request)-1);
        assert(t.output_pending() && allocations==0);
        assert(t.temporary_workspace(1)==nullptr);
        assert(t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,1)==0);
        s.limit=0; t.flush_output(); assert(s.size==0 && t.output_pending());
        s.limit=3; drain(t); verify(s); assert(allocations==0 && releases==0);
        assert(t.temporary_workspace(1)!=nullptr); fail_allocation=false;
    }
    { // Existing copying API still supports the maximum sized response.
        State s; initialize(s); s.mode=1; AtTransport t(config(),output,&s);
        t.set_command_handler(command,&s); allocations=releases=0;
        t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,0);
        assert(allocations==1 && releases==0); drain(t); verify(s); assert(releases==1);
        t.reset(); assert(releases==1);
    }
    for(unsigned mode=0;mode<2;++mode) {
        State s; initialize(s); s.mode=mode;
        AtTransport t(config(mode==0 ? 100 : kTransportResponseCapacity),output,&s);
        t.set_command_handler(command,&s); allocations=releases=0; fail_allocation=true;
        t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,0);
        drain(t); assert(s.size==7 && memcmp(s.output,"ERROR\r\n",7)==0);
        assert(releases==0); fail_allocation=false;
    }
    { // Even a tail overflow rolls the whole borrowed response back to ERROR.
        State s; initialize(s);
        AtTransport t(config(sizeof(s.body)+10U),output,&s);
        t.set_command_handler(command,&s); allocations=releases=0;
        t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,0);
        drain(t); assert(s.size==7 && memcmp(s.output,"ERROR\r\n",7)==0);
        assert(allocations==0 && t.temporary_workspace(1)!=nullptr);
    }
    { // Reset cancels a heap-owned response and frees it exactly once.
        State s; initialize(s); s.mode=1; AtTransport t(config(),output,&s);
        t.set_command_handler(command,&s); allocations=releases=0;
        t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,0);
        t.reset(); assert(releases==1 && !t.output_pending());
        t.reset(); assert(releases==1);
    }
    { // Deferred completion releases the discarded heap buffer.
        State s; initialize(s); s.mode=3; AtTransport t(config(),output,&s);
        t.set_command_handler(command,&s); allocations=releases=0;
        t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,0);
        assert(allocations==1 && releases==1 && t.response_deferred());
        assert(t.complete_deferred_response(CommandResult::Ok)); drain(t);
        assert(s.size==4 && memcmp(s.output,"OK\r\n",4)==0);
    }
    { // Destruction also releases an unconsumed owning response.
        State s; initialize(s); s.mode=1; allocations=releases=0;
        {
            AtTransport t(config(),output,&s); t.set_command_handler(command,&s);
            t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,0);
            assert(t.output_pending() && releases==0);
        }
        assert(allocations==1 && releases==1);
    }
    { // Raw echo can copy 16 KiB without modifying its handler input.
        State s; initialize(s); s.mode=4; AtTransport t(config(),output,&s);
        t.set_command_handler(command,&s); allocations=releases=0;
        t.feed(reinterpret_cast<const uint8_t *>(request),sizeof(request)-1,0);
        assert(t.feed(s.body,sizeof(s.body),1)==sizeof(s.body)); drain(t);
        assert(s.size==sizeof(s.body)+4 && memcmp(s.output,s.body,sizeof(s.body))==0);
        assert(allocations==1 && releases==1);
    }
    std::puts("BL616CL_AT_RESPONSE_STORAGE_PASS");
}
