#!/usr/bin/env python3
"""Run the fixture's actual restoration parser/wait loop with a fake clock."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'examples/ra4-wifis3-tls-resource-gate/ra4-wifis3-tls-resource-gate.ino.in').read_text()
start = source.index('bool scan_window_restored(')
end = source.index('\n#endif', start)
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / 'test.cpp'
    cpp.write_text(r'''
#include <cassert>
#include <cstdlib>
#include <string>
static unsigned long ticks;
unsigned long millis() { return ticks; }
void delay(unsigned long n) { ticks += n; }
static int queries, polls, drains, markers;
static bool malformed, failed, pending;
struct {
 bool write(const char*, std::string &s, const char*) {
  ++queries;
  s=malformed?"invalid":"0,0,0,0,0,0,0,1,1,1," + std::string(pending?"0":"1") + ",0,0";
  return !failed;
 }
} modem;
struct { void println(const char*) { ++markers; } } Serial;
struct { void poll() { ++polls; } void available() { ++drains; } } BLE;
''' + source[start:end] + r'''
int main() {
 bool restored=false;
 assert(scan_window_restored("0,0,0,0,0,0,0,1,1,1,0,0",restored) && !restored);
 assert(scan_window_restored("0,0,0,0,0,0,0,1,1,1,1,0",restored) && restored);
 for (auto s : {"", "0,0", "0,0,0,0,0,0,0,1,-1,1,1", "0,0,0,0,0,0,0,1,x,1,1", "0,0,0,0,0,0,0,1,1,1,1x"})
  assert(!scan_window_restored(s, restored));
 std::string base="0,0,0,0,0,0,0,1,1,1,1";
 for (int i=11;i<29;++i) base+=",0";
 assert(scan_window_restored(base+",1,1,0,0,0,0",restored) && !restored);
 assert(scan_window_restored(base+",0,1,1,1,0,100",restored) && restored);
 assert(wait_for_ap_scan_restore());
 assert(queries==1 && markers==1 && drains==50 && polls==50 && ticks==250);
 pending=true; queries=0; ticks=0;
 assert(!wait_for_ap_scan_restore());
 assert(ticks==10000 && queries==500 && markers==1 && drains==50);
 pending=false; malformed=true; queries=0; ticks=0;
 assert(!wait_for_ap_scan_restore() && queries==1 && ticks==0);
 malformed=false; failed=true;
 assert(!wait_for_ap_scan_restore());
}
''')
    exe = Path(temp) / 'test'
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('RA4_WIFIS3_AP_SCAN_RESTORE_PASS')
