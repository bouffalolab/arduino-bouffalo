#!/usr/bin/env python3
"""Execute the fixture's bounded, failure-only UDP trace buffer."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'examples/ra4-wifis3-tls-resource-gate/ra4-wifis3-tls-resource-gate.ino.in').read_text()
start=s.index('class UdpWriteTrace :');end=s.index('\nUdpWriteTrace udp_write_trace;',start)
program=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <string>
struct Stream {
 virtual ~Stream()=default;
 virtual size_t write(uint8_t)=0;
 virtual int available()=0,read()=0,peek()=0;
 virtual void flush()=0;
};
struct SerialOutput {
 std::string output;
 void print(const char *p) {output+=p;}
 size_t write(uint8_t b) {output+=static_cast<char>(b);return 1;}
 void println() {output+='\n';}
} Serial;
'''+s[start:end]+r'''
int main() {
 UdpWriteTrace trace;
 for(unsigned i=0;i<700;++i) assert(trace.write('x')==1);
 assert(Serial.output.empty());
 trace.write('\r');trace.write('\n');trace.dump();
 assert(Serial.output==std::string("RA4_TLS_RESOURCE:UDP_WRITE_TRACE=")+std::string(254,'x')+"<CR><LF>\n");
 Serial.output.clear();trace.reset();trace.write('O');trace.write('K');trace.dump();
 assert(Serial.output=="RA4_TLS_RESOURCE:UDP_WRITE_TRACE=OK\n");
 assert(trace.available()==0 && trace.read()==-1 && trace.peek()==-1);
}
'''
with tempfile.TemporaryDirectory(prefix='ra4-udp-trace-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(program)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('RA4_WIFIS3_UDP_WRITE_TRACE_PASS')
