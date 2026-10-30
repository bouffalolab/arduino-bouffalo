#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "lfs.h"
#include "lfs_port.h"
#include "bflb_mtd.h"

static unsigned char flash[32*1024];
static unsigned erases;
static int fail_read, fail_info;
size_t strlcpy(char *d,const char *s,size_t n) {
    size_t z=strlen(s);if(n){size_t c=z<n-1?z:n-1;memcpy(d,s,c);d[c]=0;}return z;
}
int bflb_mtd_open(const char *name, bflb_mtd_handle_t *h, int flags) {
    (void)name;(void)flags;*h=flash;return 0;
}
int bflb_mtd_info(bflb_mtd_handle_t h,bflb_mtd_info_t *i) {
    (void)h;memset(i,0,sizeof(*i));i->size=sizeof flash;return fail_info?-1:0;
}
int bflb_mtd_close(bflb_mtd_handle_t h) {(void)h;return 0;}
int bflb_flash_read(unsigned a,unsigned char *p,unsigned n) {
    if(fail_read)return -1;assert(a+n<=sizeof flash);memcpy(p,flash+a,n);return 0;
}
int bflb_flash_write(unsigned a,unsigned char *p,unsigned n) {
    assert(a+n<=sizeof flash);for(unsigned i=0;i<n;i++)flash[a+i]&=p[i];return 0;
}
int bflb_flash_erase(unsigned a,unsigned n) {
    assert(a+n<=sizeof flash);memset(flash+a,255,n);++erases;return 0;
}
static struct lfs_config config(void) {
    struct lfs_config c={0};c.read_size=256;c.prog_size=256;c.block_size=4096;
    c.block_cycles=500;c.cache_size=512;c.lookahead_size=256;return c;
}
int main(void) {
    struct lfs_context ctx={0};ctx.partition_name="PSM";
    struct lfs_config cfg=config();
    memset(flash,255,sizeof flash);
    assert(lfs_xip_init(&ctx,&cfg));assert(erases>0);
    assert(lfs_unmount(&ctx.lfs)==0);
    memset(&ctx.lfs,0,sizeof ctx.lfs);erases=0;
    assert(lfs_xip_init(&ctx,&cfg));assert(erases==0);
    assert(lfs_unmount(&ctx.lfs)==0);
    // Nonblank corruption must remain untouched on every retry.
    memset(flash,0,sizeof flash);memset(&ctx.lfs,0,sizeof ctx.lfs);erases=0;
    for(unsigned retry=0;retry<3;retry++) {
        errno=0;assert(!lfs_xip_init(&ctx,&cfg));assert(errno==EIO);assert(!erases);
        for(unsigned i=0;i<sizeof flash;i++)assert(flash[i]==0);
    }
    // Erased Flash with read or partition-query errors must not be formatted.
    memset(flash,255,sizeof flash);memset(&ctx.lfs,0,sizeof ctx.lfs);fail_read=1;
    assert(!lfs_xip_init(&ctx,&cfg));assert(!erases);
    fail_read=0;fail_info=1;assert(!lfs_xip_init(&ctx,&cfg));assert(!erases);
    fail_info=0;assert(lfs_xip_init(&ctx,&cfg));assert(erases>0);
    assert(lfs_unmount(&ctx.lfs)==0);
    puts("PERSISTENCE_MOUNT_GUARD_PASS blank_init valid_mount corruption_retries read_error mtd_error recovery");
    return 0;
}
