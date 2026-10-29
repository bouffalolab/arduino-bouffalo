/* Actual SDK LittleFS/EasyFlash, with NOR Flash mutation interruption. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lfs.h"
#include "lfs_port.h"
#include "easyflash.h"

size_t strlcpy(char *dst,const char *src,size_t n) {
    size_t size=strlen(src);if(n){size_t copied=size<n-1?size:n-1;memcpy(dst,src,copied);dst[copied]=0;}return size;
}
#ifndef FLASH_CAPACITY_BYTES
#define FLASH_CAPACITY_BYTES (32*1024)
#endif
static unsigned char flash[FLASH_CAPACITY_BYTES], baseline[sizeof flash];
extern int record_write(int mode, const void *value, size_t size);
extern size_t record_read(int mode, void *value, size_t capacity);
extern void record_legacy_tests(void);
static int fixture;
static unsigned mutation, cut, partial, fail_sync;
static jmp_buf reset_point;
static lfs_t fs;
static struct lfs_config config;
static int write_value(const unsigned char *value,size_t size) {
    if(fixture>=3)return record_write(fixture,value,size);
    if(!fixture)return ef_set_env_blob("fault/value",value,size)==EF_NO_ERR;
    lfs_file_t file={0};
    int flags=LFS_O_WRONLY|LFS_O_CREAT|(fixture==2?LFS_O_APPEND:LFS_O_TRUNC);
    if(lfs_file_open(&fs,&file,"/ordinary",flags))return 0;
    int n=lfs_file_write(&fs,&file,value,size);
    int sync=lfs_file_sync(&fs,&file);int close=lfs_file_close(&fs,&file);
    return n==(int)size&&!sync&&!close;
}
static int read_flash(const struct lfs_config *c,lfs_block_t b,lfs_off_t o,void *p,lfs_size_t n) {
    (void)c; assert(b*4096+o+n<=sizeof flash); memcpy(p,flash+b*4096+o,n); return 0;
}
static int program_flash(const struct lfs_config *c,lfs_block_t b,lfs_off_t o,const void *p,lfs_size_t n) {
    (void)c; assert(b*4096+o+n<=sizeof flash); ++mutation;
    unsigned limit=(cut==mutation && partial)?n/2:n;
    for(unsigned i=0;i<limit;i++) flash[b*4096+o+i]&=((const unsigned char*)p)[i];
    if(cut==mutation) longjmp(reset_point,1); return 0;
}
static int erase_flash(const struct lfs_config *c,lfs_block_t b) {
    (void)c; assert((b+1)*4096<=sizeof flash); ++mutation;
    memset(flash+b*4096,255,(cut==mutation && partial)?2048:4096);
    if(cut==mutation) longjmp(reset_point,1); return 0;
}
static int sync_flash(const struct lfs_config *c) { (void)c; return fail_sync?LFS_ERR_IO:0; }
lfs_t *lfs_xip_init(struct lfs_context *ctx,struct lfs_config *cfg) {
    (void)ctx;(void)cfg;
    memset(&fs,0,sizeof fs); memset(&config,0,sizeof config);
    config.read=read_flash; config.prog=program_flash; config.erase=erase_flash; config.sync=sync_flash;
    config.read_size=256;config.prog_size=256;config.block_size=4096;
    config.block_count=8;config.block_cycles=500;config.cache_size=512;config.lookahead_size=256;
    return lfs_mount(&fs,&config)==0?&fs:NULL;
}
static void boot(void) { assert(easyflash_init()==EF_NO_ERR); }
static void read_value(const unsigned char *old,size_t old_size,const unsigned char *value,size_t size) {
    unsigned char output[8192];size_t actual=0;
    size_t n;
    if(fixture>=3) {actual=record_read(fixture,output,sizeof output);n=actual;}
    else if(!fixture) {n=ef_get_env_blob("fault/value",output,sizeof output,&actual);}
    else {
        lfs_file_t f={0};assert(!lfs_file_open(&fs,&f,"/ordinary",LFS_O_RDONLY));
        actual=lfs_file_size(&fs,&f);n=lfs_file_read(&fs,&f,output,sizeof output);
        assert(!lfs_file_close(&fs,&f));
    }
    assert(n==actual);
    assert((actual==old_size && !memcmp(old,output,old_size)) || (actual==size && !memcmp(value,output,size)));
}
int main(void) {
    static unsigned char old[6000],value[7000];
    for(unsigned i=0;i<sizeof old;i++)old[i]=(i*37)^0x5a;
    for(unsigned i=0;i<sizeof value;i++)value[i]=(i*71)^0xa5;
    unsigned cases=0;
    for(fixture=0;fixture<5;fixture++) {
    memset(flash,255,sizeof flash);lfs_xip_init(NULL,NULL);
    assert(lfs_format(&fs,&config)==0);boot();
    assert(write_value(old,sizeof old));
    assert(lfs_unmount(&fs)==0);memcpy(baseline,flash,sizeof flash);
    mutation=0;boot();
    if(fixture==2)assert(write_value(value,1000));else assert(write_value(value,sizeof value));
    unsigned count=mutation;assert(lfs_unmount(&fs)==0);assert(count>0);
    /* Every boundary, both complete and torn NOR mutations. */
    for(unsigned mode=0;mode<2;mode++)for(unsigned point=1;point<=count;point++) {
        partial=mode;cut=point;memcpy(flash,baseline,sizeof flash);mutation=0;boot();
        if(!setjmp(reset_point)) {write_value(value,fixture==2?1000:sizeof value);assert(!"cut not reached");}
        cut=0;boot();
        unsigned char appended[7000];memcpy(appended,old,sizeof old);memcpy(appended+sizeof old,value,1000);
        read_value(old,sizeof old,fixture==2?appended:value,sizeof value);++cases;
        assert(ef_set_env_blob("fault/recover",value,1024)==EF_NO_ERR);
        assert(lfs_unmount(&fs)==0);
    }
    printf("PERSISTENCE_FIXTURE_PASS fixture=%d boundaries=%u torn_cases=%u\n",fixture,count,count*2);
    }
    fixture=0;
    memcpy(flash,baseline,sizeof flash);boot();fail_sync=1;
    assert(ef_set_env_blob("fault/value",value,sizeof value)==EF_WRITE_ERR);
    fail_sync=0;
    fail_sync=0;
    assert(ef_set_env_blob("fault/delete",value,1000)==EF_NO_ERR);
    fail_sync=1;assert(ef_del_env("fault/delete")==EF_WRITE_ERR);fail_sync=0;
    record_legacy_tests();
    /* A full partition must preserve an existing atomic record on failure. */
    memset(flash,255,sizeof flash);lfs_xip_init(NULL,NULL);
    assert(!lfs_format(&fs,&config));boot();fixture=3;
    assert(write_value(old,sizeof old));
    lfs_file_t filler={0};assert(!lfs_file_open(&fs,&filler,"/filler",LFS_O_WRONLY|LFS_O_CREAT));
    for(unsigned i=0;i<1000;i++) {
        int n=lfs_file_write(&fs,&filler,value,512);
        if(n!=512)break;
        if(lfs_file_sync(&fs,&filler))break;
    }
    (void)lfs_file_close(&fs,&filler);
    assert(!write_value(value,sizeof value));
    read_value(old,sizeof old,old,sizeof old);
    assert(!lfs_remove(&fs,"/filler"));assert(write_value(value,sizeof value));
    read_value(old,sizeof old,value,sizeof value);
    printf("PERSISTENCE_NOR_RESET_PASS torn_cases=%u sync_and_remove_errors_propagated=1 legacy_migration=1 full_partition_recovery=1\n",cases);
    return 0;
}
