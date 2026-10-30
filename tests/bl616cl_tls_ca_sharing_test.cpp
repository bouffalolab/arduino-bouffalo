#include "../UNOR4BL616CLBridge/tls_client_service.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdio>

static unsigned allocations, frees;
static bool fail_allocation;
extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" void *__wrap_malloc(size_t n) {
    if (fail_allocation) return nullptr;
    ++allocations; return __real_malloc(n);
}
extern "C" void __wrap_free(void *p) {
    if (p) ++frees;
    __real_free(p);
}
struct State { int next=0; const uint8_t *ca=nullptr; size_t size=0; std::string output; };
static bool create(void *p,int *h) { *h=static_cast<State *>(p)->next++; return true; }
static bool close_client(void *,int) { return true; }
static bool connect_client(void *p,int,const char *,uint16_t,uint32_t,const uint8_t *ca,size_t n,
                           const uint8_t *,size_t,const uint8_t *,size_t) {
    auto &s=*static_cast<State *>(p);s.ca=ca;s.size=n;return true;
}
static size_t output(void *p,const uint8_t *b,size_t n) {
    static_cast<State *>(p)->output.append(reinterpret_cast<const char *>(b),n);return n;
}
static bridge::at::CommandResult dispatch(void *p,const bridge::at::Command &c,bridge::at::AtTransport &t) {
    return static_cast<bridge::TlsClientService *>(p)->handle_command(c,t);
}
int main() {
    State state;
    bridge::TlsClientBackend backend={};backend.context=&state;backend.create=create;
    backend.close=close_client;backend.connect_hostname=connect_client;
    {
        bridge::TlsClientService service(backend);
        bridge::at::AtTransport transport({256,64,512,20},output,&state);
        transport.set_command_handler(dispatch,&service);
        auto command=[&](const std::string &s,bool ok=true) {
            state.output.clear();
            transport.feed(reinterpret_cast<const uint8_t *>(s.data()),s.size(),1);
            while (transport.output_pending()) transport.flush_output();
            assert(state.output.find(ok?"OK\r\n":"ERROR\r\n")!=std::string::npos);
        };
        for (int i=0;i<8;++i) {
            command("AT+SSLBEGINCLIENT\r\n");
            command("AT+SETCAROOT="+std::to_string(2000+i)+",3\r\nabc");
        }
        assert(allocations==1 && frees==0);
        command("AT+SSLCLIENTCONNECT=2000,peer.test,443\r\n");
        const uint8_t *shared=state.ca;assert(state.size==4 && !memcmp(shared,"abc\0",4));
        command("AT+SSLCLIENTCONNECT=2007,peer.test,443\r\n");assert(state.ca==shared);
        // Same-slot update and sharing require no allocation, even under pressure.
        fail_allocation=true;
        command("AT+SETCAROOT=2000,3\r\nabc");
        command("AT+SETCAROOT=2000,3\r\nxyz",false);
        command("AT+SSLCLIENTCONNECT=2000,peer.test,443\r\n");assert(state.ca==shared);
        fail_allocation=false;
        command("AT+SETCAROOT=2000,3\r\nxyz");assert(allocations==2 && frees==0);
        command("AT+SSLCLIENTCONNECT=2000,peer.test,443\r\n");assert(state.ca!=shared && !memcmp(state.ca,"xyz\0",4));
        // Other slots retain their original trust anchor after replacement/close.
        command("AT+SSLCLIENTCONNECT=2001,peer.test,443\r\n");assert(state.ca==shared);
        command("AT+SSLCLIENTCLOSE=2000\r\n");assert(frees==1);
        command("AT+SSLCLIENTCONNECT=2007,peer.test,443\r\n");assert(state.ca==shared && !memcmp(shared,"abc\0",4));
        // Binary comparison includes embedded NUL bytes; equal lengths alone do not share.
        command("AT+SSLBEGINCLIENT\r\n");
        command(std::string("AT+SETCAROOT=2000,3\r\n") + std::string("a\0c",3));
        assert(allocations==3);
        service.close_all();assert(frees==3);
    }
    assert(allocations==frees);
    puts("BL616CL_TLS_CA_SHARING_PASS");
}
