// Production OTA decoder/service with real SDK LittleFS and simulated NOR.
#include "ra4_ota_service.h"
extern "C" {
#include "lfs.h"
}
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using bridge::Ra4OtaContainer;
using bridge::Ra4OtaService;
static uint8_t flash[320*1024];
static lfs_t fs;
static lfs_file_t file;
static bool opened;
static int rd(const lfs_config*,lfs_block_t b,lfs_off_t o,void *p,lfs_size_t n) {
    assert(b*4096+o+n<=sizeof flash);memcpy(p,flash+b*4096+o,n);return 0;
}
static int wr(const lfs_config*,lfs_block_t b,lfs_off_t o,const void *p,lfs_size_t n) {
    assert(b*4096+o+n<=sizeof flash);for(unsigned i=0;i<n;i++)flash[b*4096+o+i]&=((const uint8_t*)p)[i];return 0;
}
static int erase(const lfs_config*,lfs_block_t b) {memset(flash+b*4096,255,4096);return 0;}
static int sync(const lfs_config*) {return 0;}
extern "C" size_t strlcpy(char *d,const char *s,size_t n) {
    size_t z=strlen(s);if(n){size_t c=z<n-1?z:n-1;memcpy(d,s,c);d[c]=0;}return z;
}
static bool begin(void*,const char*,bool) {
    (void)lfs_remove(&fs,"/cache");memset(&file,0,sizeof file);
    opened=lfs_file_open(&fs,&file,"/cache",LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC)==0;return opened;
}
static bool write(void*,const uint8_t *p,size_t n) {return lfs_file_write(&fs,&file,p,n)==(int)n;}
static bool commit(void*) {
    int a=lfs_file_sync(&fs,&file),b=lfs_file_close(&fs,&file);opened=false;return !a&&!b;
}
static void discard(void*) {if(opened)lfs_file_close(&fs,&file);opened=false;(void)lfs_remove(&fs,"/cache");}
static size_t maximum(void*) {return 240*1024;}
static std::vector<uint8_t> package(size_t size) {
    std::vector<uint8_t> bytes(20);uint32_t bits=0;unsigned count=0;
    for(size_t i=0;i<size;i++) {
        bits=(bits<<9)|0x100|uint8_t(i*37+11);count+=9;
        while(count>=8){count-=8;bytes.push_back(uint8_t(bits>>count));}
    }
    if(count)bytes.push_back(uint8_t(bits<<(8-count)));
    auto set=[&](size_t at,uint32_t n){for(unsigned i=0;i<4;i++)bytes[at+i]=n>>(8*i);};
    set(0,bytes.size()-8);set(8,Ra4OtaContainer::kUnoR4Magic);
    set(4,Ra4OtaContainer::crc32_update(0xffffffff,bytes.data()+8,bytes.size()-8)^0xffffffff);
    return bytes;
}
static Ra4OtaService::Error download(Ra4OtaService &s,size_t size) {
    auto error=s.begin("/cache",false);if(error!=Ra4OtaService::Error::None)return error;
    auto bytes=package(size);
    for(size_t i=0;i<bytes.size();i+=511) {
        error=s.feed(bytes.data()+i,std::min(size_t(511),bytes.size()-i));
        if(error!=Ra4OtaService::Error::None)return error;
    }
    return s.finish();
}
int main() {
    memset(flash,255,sizeof flash);
    lfs_config c={};c.read=rd;c.prog=wr;c.erase=erase;c.sync=sync;c.block_count=80;
    c.read_size=256;c.prog_size=256;c.block_size=4096;c.cache_size=256;c.lookahead_size=64;c.block_cycles=500;
    assert(!lfs_format(&fs,&c));assert(!lfs_mount(&fs,&c));
    Ra4OtaService s({begin,write,commit,discard,maximum,nullptr});
    lfs_file_t filler={};assert(!lfs_file_open(&fs,&filler,"/filler",LFS_O_WRONLY|LFS_O_CREAT));
    uint8_t block[512]={};
    for(unsigned i=0;i<1000;i++) {
        if(lfs_file_write(&fs,&filler,block,sizeof block)!=(int)sizeof block)break;
        if(lfs_file_sync(&fs,&filler))break;
    }
    (void)lfs_file_close(&fs,&filler);
    const auto error=download(s,32768);
    assert(error==Ra4OtaService::Error::StorageEnd || error==Ra4OtaService::Error::StorageInit);
    assert(s.state()==Ra4OtaService::State::Failed);
    assert(!lfs_remove(&fs,"/filler"));
    assert(download(s,240*1024)==Ra4OtaService::Error::None);
    assert(s.state()==Ra4OtaService::State::Ready && s.decoded_size()==240*1024);
    lfs_file_t read={};assert(!lfs_file_open(&fs,&read,"/cache",LFS_O_RDONLY));
    for(unsigned offset=0;offset<240*1024;offset+=sizeof block) {
        assert(lfs_file_read(&fs,&read,block,sizeof block)==(int)sizeof block);
        for(unsigned i=0;i<sizeof block;i++)assert(block[i]==uint8_t((offset+i)*37+11));
    }
    assert(!lfs_file_close(&fs,&read));
    assert(download(s,240*1024+1)==Ra4OtaService::Error::HeaderLength);
    assert(s.state()==Ra4OtaService::State::Failed);
    assert(download(s,32768)==Ra4OtaService::Error::None);
    assert(!lfs_unmount(&fs));
    printf("OTA_REAL_LITTLEFS_CAPACITY_PASS partition=327680 max_decoded=245760 full_error=%d recovery=1 over_limit_rejected=1\n",int(error));
}
