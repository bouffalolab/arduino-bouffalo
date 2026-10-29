"""Full native IPv6/TCP input with production ingress and owner accounting.

NO_SYS host model; no claim of target RAM measurements or IPv6 radio delivery.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'third_party/bouffalo_sdk/components/net/lwip/lwip/src'
PATCHES = ROOT / 'tools/bridge_runtime/profiles/bridge/fhost_patches'


class IPv6TcpInputTest(unittest.TestCase):
    def test_native_input(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            native = temp / 'components/net/lwip/lwip/src'
            shutil.copytree(SDK, native)
            for number in (70, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84):
                patch, = PATCHES.glob(f'{number:04d}-*.patch')
                subprocess.run(['patch', '-s', '-p1', '-i', str(patch)], cwd=temp, check=True)
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
#define LWIP_IPV6_AUTOCONFIG 0
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS 0
#define LWIP_ND6_QUEUEING 0
#define LWIP_ICMP6 1
#define LWIP_TCP 1
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_TIMERS 0
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_STATS 1
#define MEM_STATS 1
#define MEMP_STATS 0
#define MEM_ALIGNMENT 8
#define MEM_SIZE 24576
#define PBUF_POOL_SIZE 0
#define LWIP_SUPPORT_CUSTOM_PBUF 1
#define MEMP_NUM_REASSDATA 5
#define MEMP_NUM_TCP_PCB 40
#define MAC_RXQ_DEPTH 12
#define IP_REASS_MAX_PBUFS (2 * MAC_RXQ_DEPTH - 2)
#define IPV6_FRAG_COPYHEADER 1
#define IPV6_TIMER_PRECISE_NEEDED 0
#define TCP_TIMER_PRECISE_NEEDED 0
#define TCP_MSS 1460
#define TCP_WND 35040
#define TCP_SND_BUF 5840
''')
            common = ['-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                      '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                      '-I' + str(temp), '-I' + str(native / 'include'),
                      '-I' + str(ROOT / 'UNOR4BL616CLBridge')]
            objects = []
            units = ['core/' + n + '.c' for n in ('mem', 'memp', 'pbuf', 'def', 'timeouts',
                     'ip', 'netif', 'inet_chksum', 'tcp', 'tcp_in', 'tcp_out')]
            units += ['netif/ethernet.c']
            units += ['core/ipv6/' + n + '.c' for n in ('ip6', 'ip6_addr', 'ip6_frag', 'icmp6', 'nd6')]
            for index, unit in enumerate(units):
                obj = temp / f'native{index}.o'
                subprocess.run(['gcc', '-std=c11', *common, '-c', str(native / unit), '-o', str(obj)], check=True)
                objects.append(obj)
            flags = ['-DBL616CL_STA_TCP_OWNED_RX=1', '-DBL616CL_AP_TCP_OWNED_RX=1',
                     '-DBL616CL_STA_TCP_RX_RESERVE_BYTES=4096', '-DBL616CL_STA_TCP_RX_CHUNK_BYTES=1024',
                     '-DBL616CL_TCP_RX_ATOMIC_RESERVE=1', '-DBL616CL_TCP_RX_MEMORY_OWNER=1',
                     '-DBL616CL_TCP_RX_FAIR_WINDOW=1', '-DBL616CL_TCP_RX_PHYSICAL_FAIR=1']
            sources = [ROOT / 'tests/native_ipv6/ingress.cpp', ROOT / 'tests/native_ipv6/tcp_input.cpp',
                       ROOT / 'UNOR4BL616CLBridge/tcp_rx_native_policy.cpp',
                       ROOT / 'UNOR4BL616CLBridge/tcp_rx_credit_pool.cpp']
            binary = temp / 'tcp_input'
            subprocess.run(['g++', '-std=c++17', *common, *flags, *map(str, sources),
                            *map(str, objects), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
