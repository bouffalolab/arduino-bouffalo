"""Compile actual SDK storage with the project patch and inject NOR resets."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'third_party/bouffalo_sdk'
PATCHES = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches'
with tempfile.TemporaryDirectory(prefix='bl-persistence-') as directory:
    work = Path(directory)
    record = next(r for r in json.loads((PATCHES/'manifest.json').read_text())['patches'] if r['id'].startswith('0085-'))
    for target in record['targets']:
        p=work/target['path'];p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((SDK/target['path']).read_bytes())
        assert hashlib.sha256(p.read_bytes()).hexdigest()==target['before_sha256']
    subprocess.run(['patch','--batch','-p1','-i',str(PATCHES/record['path'])],cwd=work,check=True)
    for target in record['targets']:
        assert hashlib.sha256((work/target['path']).read_bytes()).hexdigest()==target['after_sha256']
    (work/'log.h').write_text('#define LOG_E(...) ((void)0)\n#define LOG_W(...) ((void)0)\n#define LOG_I(...) ((void)0)\n#define LOG_F(...) ((void)0)\n')
    (work/'compat.h').write_text('#include <stddef.h>\nsize_t strlcpy(char *, const char *, size_t);\n')
    base=SDK/'components/fs/littlefs'
    common=['-g','-O0','-DLFS_NO_DEBUG','-DLFS_NO_WARN','-DLFS_NO_ERROR',
             '-I'+str(work),'-I'+str(base/'littlefs'),'-I'+str(base/'port'),'-I'+str(base/'easyflash_port'),
             '-I'+str(ROOT/'UNOR4BL616CLBridge')]
    objects=[]
    for index,source in enumerate([ROOT/'tests/bl616cl_persistence_fault_test.c',
             work/'components/fs/littlefs/easyflash_port/lfs_easyflash.c',
             base/'littlefs/lfs.c',base/'littlefs/lfs_util.c',
             ROOT/'tests/bl616cl_persistent_record_test.cpp']):
        obj=work/f'{index}.o';objects.append(str(obj))
        cpp=source.suffix=='.cpp'
        subprocess.run(['g++' if cpp else 'gcc','-std=gnu++17' if cpp else '-std=gnu11',
                        *common,*([] if cpp else ['-include',str(work/'compat.h')]),
                        '-c',str(source),'-o',str(obj)],check=True)
    subprocess.run(['g++',*objects,'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
    (work/'bflb_flash.h').write_text('int bflb_flash_read(unsigned,unsigned char *,unsigned);\nint bflb_flash_write(unsigned,unsigned char *,unsigned);\nint bflb_flash_erase(unsigned,unsigned);\n')
    (work/'bflb_l1c.h').write_text('')
    (work/'bflb_mtd.h').write_text('''#pragma once
typedef void *bflb_mtd_handle_t;
typedef struct {unsigned offset,size,xip_addr;} bflb_mtd_info_t;
#define BFLB_MTD_OPEN_FLAG_BUSADDR 1
int bflb_mtd_open(const char *,bflb_mtd_handle_t *,int);
int bflb_mtd_info(bflb_mtd_handle_t,bflb_mtd_info_t *);
int bflb_mtd_close(bflb_mtd_handle_t);
''')
    subprocess.run(['gcc','-std=gnu11',*common,'-include',str(work/'compat.h'),
                    str(ROOT/'tests/bl616cl_persistence_mount_test.c'),
                    str(work/'components/fs/littlefs/port/lfs_xip_flash.c'),
                    str(base/'littlefs/lfs.c'),str(base/'littlefs/lfs_util.c'),
                    '-o',str(work/'mount-test')],check=True)
    subprocess.run([str(work/'mount-test')],check=True)
    subprocess.run(['g++','-std=gnu++17',*common,
                    str(ROOT/'tests/bl616cl_ota_littlefs_capacity_test.cpp'),
                    str(ROOT/'UNOR4BL616CLBridge/ra4_ota_service.cpp'),
                    str(ROOT/'UNOR4BL616CLBridge/ra4_ota_container.cpp'),
                    objects[2],objects[3],'-o',str(work/'capacity-test')],check=True)
    subprocess.run([str(work/'capacity-test')],check=True)
