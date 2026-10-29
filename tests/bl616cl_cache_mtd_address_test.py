"""Run actual SDK MTD/partition lookup for cache located between FW slots."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SDK=ROOT/"third_party/bouffalo_sdk"
with tempfile.TemporaryDirectory() as directory:
    work=Path(directory)
    (work/"bflb_core.h").write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define FLASH_XIP_BASE 0x80000000U
#define arch_memcpy_fast memcpy
size_t strlcpy(char *,const char *,size_t);
''')
    (work/"bflb_flash.h").write_text('''#pragma once
int bflb_flash_read(uint32_t,uint8_t *,uint32_t);
int bflb_flash_write(uint32_t,uint8_t *,uint32_t);
int bflb_flash_erase(uint32_t,uint32_t);
''')
    source=work/"test.c"
    source.write_text('''#include <assert.h>
#include "bflb_boot2.h"
#include "bflb_mtd.h"
size_t strlcpy(char *d,const char *s,size_t n) {
    size_t z=strlen(s);if(n){size_t c=z<n-1?z:n-1;memcpy(d,s,c);d[c]=0;}return z;
}
int main(void) {
    pt_table_stuff_config *p=bflb_boot2_get_pt_config();
    memset(p,0,sizeof(*p));p->pt_table.magicCode=0x54504642;p->pt_table.entryCnt=2;
    strcpy((char *)p->pt_entries[0].name,"FW");
    p->pt_entries[0].start_address[0]=0x10000;p->pt_entries[0].start_address[1]=0x210000;
    strcpy((char *)p->pt_entries[1].name,"RA4_OTA");
    p->pt_entries[1].start_address[0]=0x1c0000;p->pt_entries[1].max_len[0]=0x50000;
    bflb_mtd_handle_t h=NULL;bflb_mtd_info_t info;
    assert(bflb_mtd_open("RA4_OTA",&h,BFLB_MTD_OPEN_FLAG_BUSADDR)<0);
    assert(bflb_mtd_open("RA4_OTA",&h,0)==0);
    assert(bflb_mtd_info(h,&info)==0);
    assert(info.offset==0x1c0000 && info.size==0x50000 && !info.xip_addr);
    assert(!bflb_mtd_close(h));
    puts("CACHE_ACTUAL_SDK_RAW_MTD_LOOKUP_PASS between_firmware_slots=1");
}
''')
    base=SDK/"components/utils/bflb_mtd"
    command=['gcc','-std=gnu11','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
             '-I'+str(work),'-I'+str(base/'include'),'-I'+str(SDK/'components/utils/partition'),
             str(source),str(base/'bflb_mtd.c'),str(base/'bflb_boot2.c'),'-o',str(work/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(work/'test')],check=True)
