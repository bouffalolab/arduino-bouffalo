"""Run complete SDK IPv6 reassembly/pbuf/native heap units under sanitizers.

Uses host pointer alignment and NO_SYS; this is not a target RAM measurement or
an Ethernet/IP/TCP integration test. SDK sources and frozen artifacts stay intact.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'third_party/bouffalo_sdk'
PATCHES = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches'


class IPv6ReassemblyTest(unittest.TestCase):
    def test_native_reassembly_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            core = temp / 'components/net/lwip/lwip/src/core'
            core.mkdir(parents=True)
            for name in ('mem.c', 'pbuf.c', 'ipv6/ip6_frag.c'):
                (core / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(SDK / 'components/net/lwip/lwip/src/core' / name, core / name)
            for name in ('0076-bridge-lwip-allocation-failure-diagnostic.patch',
                         '0078-bridge-lwip-atomic-rx-reserve.patch',
                         '0079-bridge-lwip-memory-lifecycle.patch'):
                subprocess.run(['patch', '-s', '-p1', '-i', str(PATCHES / name)], cwd=temp, check=True)
            (temp / 'arch').mkdir()
            (temp / 'sdk/mm').mkdir(parents=True)
            (temp / 'sdk/mm/mm.h').write_text('#include <stdint.h>\nuintptr_t mm_lock_save(void);\nvoid mm_unlock_restore(uintptr_t);\n')
            (temp / 'arch/cc.h').write_text('#include <assert.h>\n#define LWIP_PLATFORM_ASSERT(s) assert(!s)\n')
            (temp / 'lwipopts.h').write_text('''
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_IPV4 0
#define LWIP_IPV6 1
#define LWIP_IPV6_REASS 1
#define LWIP_IPV6_FRAG 0
#define LWIP_IPV6_SCOPES 0
#define LWIP_IPV6_MLD 0
#define LWIP_ND6_QUEUEING 0
#define LWIP_ICMP6 0
#define LWIP_TCP 0
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_TIMERS 0
#define LWIP_STATS 1
#define MEM_STATS 1
#define MEMP_STATS 0
#define MEM_ALIGNMENT 8
#define MEM_SIZE 24576
#define PBUF_POOL_SIZE 0
#define LWIP_SUPPORT_CUSTOM_PBUF 1
#define MEMP_NUM_REASSDATA 5
#define MAC_RXQ_DEPTH 12
#define IP_REASS_MAX_PBUFS (2 * MAC_RXQ_DEPTH - 2)
#define IPV6_FRAG_COPYHEADER 1
#define IPV6_TIMER_PRECISE_NEEDED 0
''')
            native = SDK / 'components/net/lwip/lwip/src'
            binary = temp / 'reassembly'
            ingress = temp / 'ingress.o'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I' + str(temp), '-I' + str(native / 'include'),
                            '-I' + str(ROOT / 'UNOR4BL616CLBridge'),
                            '-DBL616CL_STA_TCP_OWNED_RX=1', '-DBL616CL_AP_TCP_OWNED_RX=1',
                            '-DBL616CL_STA_TCP_RX_RESERVE_BYTES=4096',
                            '-DBL616CL_STA_TCP_RX_CHUNK_BYTES=1024',
                            '-DBL616CL_TCP_RX_ATOMIC_RESERVE=1',
                            '-DBL616CL_TCP_RX_MEMORY_OWNER=1', '-DBL616CL_TCP_RX_FAIR_WINDOW=1',
                            '-c', str(ROOT / 'tests/native_ipv6/ingress.cpp'), '-o', str(ingress)], check=True)
            # SDK mem_size_t is 16 bit here; its arithmetic promotes to signed
            # int on the host. Keep SDK/patch bytes intact for this test.
            command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                            '-I' + str(temp), '-I' + str(native / 'include'),
                            str(core / 'mem.c'), str(core / 'pbuf.c'),
                            str(native / 'core/memp.c'), str(native / 'core/def.c'),
                            str(core / 'ipv6/ip6_frag.c'),
                            str(ROOT / 'tests/native_ipv6/reassembly.c'), str(ingress),
                            '-lstdc++', '-o', str(binary)]
            # First require the untouched supplier unit to reproduce all 26
            # header-length errors; then run the identical corrected cases.
            subprocess.run(command + ['-DEXPECT_LENGTH_BUG=1'], check=True)
            subprocess.run([str(binary)], check=True)
            subprocess.run(['patch', '-s', '-p1', '-i', str(PATCHES /
                            '0084-bridge-ipv6-reassembly-payload-length.patch')], cwd=temp, check=True)
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
