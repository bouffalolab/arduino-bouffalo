"""The bridge must retain its validated runtime policy without shell-only flags."""
from pathlib import Path
import re,subprocess
root=Path(__file__).resolve().parents[1]
platform=root/'.'
header=platform/'variants/unor4_bl616cl_provisional/bridge_profile.h'
def macros(*flags):
    output=subprocess.check_output(['g++',*flags,'-include',str(header),'-E','-dM','-x','c++','/dev/null'],text=True)
    return dict(re.findall(r'^#define (BL616CL_\w+) (\S+)$',output,re.M))
values=macros()
expected={'BL616CL_RA4_NORMAL_BOOT_ON_STARTUP':'1','BL616CL_AT_RX_DMA':'1','BL616CL_RUNTIME_BALANCED_HEAP':'1',
'BL616CL_RUNTIME_INIT_WRAM_FIRST':'1','BL616CL_STA_TCP_OWNED_RX':'1',
'BL616CL_AP_TCP_OWNED_RX':'1','BL616CL_STA_TCP_RX_CHUNK_BYTES':'1024',
'BL616CL_STA_TCP_RX_RESERVE_BYTES':'4096','BL616CL_TCP_RECEIVE_WINDOW_BYTES':'2048',
'BL616CL_TCP_RX_ATOMIC_RESERVE':'1','BL616CL_TCP_RX_FAIR_WINDOW':'1',
'BL616CL_TCP_RX_MEMORY_OWNER':'1','BL616CL_TCP_RX_PHYSICAL_FAIR':'1',
'BL616CL_TLS_COALESCE_READ':'1','BL616CL_TLS_CACHED_AVAILABLE':'1',
'BL616CL_IPV6_TCP_DIAGNOSTIC':'0','BL616CL_RUNTIME_HEAP_OWNERSHIP':'0',
'BL616CL_TLS_WINDOW_TIMING':'0'}
assert all(values.get(k)==v for k,v in expected.items())
overridden=macros('-DBL616CL_RA4_NORMAL_BOOT_ON_STARTUP=0','-DBL616CL_TLS_WINDOW_TIMING=1','-DBL616CL_IPV6_TCP_DIAGNOSTIC=1','-DBL616CL_TLS_CACHED_AVAILABLE=0')
assert overridden['BL616CL_RA4_NORMAL_BOOT_ON_STARTUP']=='0'
assert overridden['BL616CL_TLS_WINDOW_TIMING']=='1' and overridden['BL616CL_IPV6_TCP_DIAGNOSTIC']=='1'
assert overridden['BL616CL_TLS_CACHED_AVAILABLE']=='0'
boards=(platform/'boards.txt').read_text()
assert len(re.findall(r'^\w+\.build\.bridge_cpp_profile=.*$',boards,re.M))==1
assert 'unor4_bl616cl_provisional.build.bridge_cpp_profile=-include ' in boards
cpp_recipe=next(x for x in boards.splitlines() if x.startswith('unor4_bl616cl_provisional.recipe.cpp.o.pattern='))
assert '{build.bridge_cpp_profile}' in cpp_recipe
print('BL616CL_BRIDGE_DEFAULT_NETWORK_PROFILE_AND_EXPLICIT_OVERRIDE_PASS')
