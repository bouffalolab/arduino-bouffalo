#include "self_fota_service.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
using namespace bridge;
using namespace bridge::at;
static int start_result, update_result, finish_result;
static unsigned starts, updates, finishes, aborts;
static uint32_t written;
extern "C" int bflb_ota_start(uint32_t) { ++starts; written=0; return start_result; }
extern "C" int bflb_ota_update(uint32_t total,uint32_t offset,uint8_t *,uint32_t size) {
    assert(total==4096 && offset==written);++updates;written+=size;return update_result;
}
extern "C" int bflb_ota_finish(uint8_t check) { assert(check==1);++finishes;return finish_result; }
extern "C" void bflb_ota_abort() { ++aborts; }
struct Fixture {
    SelfFotaService service;
    std::string out;
    static size_t output(void *ctx,const uint8_t *data,size_t size) {
        static_cast<Fixture *>(ctx)->out.append(reinterpret_cast<const char *>(data),size);return size;
    }
    static CommandResult handle(void *ctx,const Command &cmd,AtTransport &t) {
        return static_cast<Fixture *>(ctx)->service.handle_command(cmd,t);
    }
    AtTransport transport{{1023,16384,17424,1000},output,this};
    Fixture() {transport.set_command_handler(handle,this);}
    void feed(const std::string &bytes) {
        assert(transport.feed(reinterpret_cast<const uint8_t *>(bytes.data()),bytes.size(),0)==bytes.size());
        while(transport.output_pending())transport.flush_output();
    }
    void exact(const std::string &cmd,const char *want) {
        out.clear();feed(cmd);assert(out==want);
    }
    void begin() {exact("AT+BLFOTABEGIN=4096\r\n","+BLFOTABEGIN: 0\r\nOK\r\n");}
    void body() {exact("AT+BLFOTADATA=4096\r\n","");exact((std::string("BFNP")+std::string(4092,'x')),"OK\r\n");}
};
int main() {
    Fixture f;
    for(const char *arg:{"-1","+4096","4095","1474561","4294971392","4096x"})
        f.exact(std::string("AT+BLFOTABEGIN=")+arg+"\r\n","ERROR\r\n");
    assert(starts==0);
    start_result=-1;f.exact("AT+BLFOTABEGIN=4096\r\n","ERROR\r\n");assert(!f.service.active());start_result=0;
    f.begin();f.exact("AT+BLFOTAFINISH\r\n","ERROR\r\n");assert(finishes==0);
    f.exact("AT+BLFOTABEGIN=4096\r\n","ERROR\r\n");
    f.exact("AT+BLFOTADATA=4097\r\n","ERROR\r\n");
    f.exact("AT+BLFOTAABORT\r\n","+BLFOTAABORT: 0\r\nOK\r\n");assert(aborts==1);
    f.begin();f.exact("AT+BLFOTADATA=4096\r\n","");f.feed("BFNP");
    f.out.clear();f.transport.tick(1001);while(f.transport.output_pending())f.transport.flush_output();
    assert(f.out=="ERROR\r\n"&&written==0&&f.service.active());
    f.exact("AT+BLFOTAFINISH\r\n","ERROR\r\n");
    f.exact("AT+BLFOTAABORT\r\n","+BLFOTAABORT: 0\r\nOK\r\n");
    f.begin();f.exact("AT+BLFOTADATA=4096\r\n","");f.exact(std::string(4096,'x'),"ERROR\r\n");assert(!f.service.active()&&aborts==3);
    f.begin();update_result=-1;f.exact("AT+BLFOTADATA=4096\r\n","");f.exact((std::string("BFNP")+std::string(4092,'x')),"ERROR\r\n");assert(!f.service.active()&&aborts==4);update_result=0;
    f.begin();f.body();finish_result=-1;f.exact("AT+BLFOTAFINISH\r\n","ERROR\r\n");assert(!f.service.active()&&!f.service.take_reset_request());finish_result=0;
    f.begin();f.body();f.exact("AT+BLFOTAFINISH\r\n","+BLFOTAFINISH: 0\r\nOK\r\n");assert(!f.service.active()&&f.service.take_reset_request()&&!f.service.take_reset_request());
    f.exact("AT+BLFOTAFINISH\r\n","ERROR\r\n");
    assert(finishes==2&&updates==3);
    std::puts("BL616CL_SELF_FOTA_STATE_AND_RETRY_PASS");
}
