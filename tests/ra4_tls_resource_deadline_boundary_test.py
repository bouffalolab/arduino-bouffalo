"""Execute the real receive loop with delayed calls and a preexisting RA4 FIFO."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'examples/ra4-wifis3-tls-resource-gate/ra4-wifis3-tls-resource-gate.ino.in').read_text()
start = source.index('bool receive_burst(size_t round) {')
end = source.index('\nbool run_round(', start)
function = source[start:end]
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
using std::size_t;
unsigned long ticks;
unsigned long millis() { return ticks; }
void delay(unsigned long value) { ticks += value; }
struct Output {
 std::ostringstream text;
 template<class T> void print(T value) { text << +value; }
 void print(const char *value) { text << value; }
 void print(char value) { text << value; }
 template<class T> void println(T value) { print(value); text << '\n'; }
 void println() { text << '\n'; }
} Serial;
constexpr size_t kTlsClientCount=8, kReceiveBurstBytes=4096;
constexpr int kTlsSocketBase=2000;
constexpr unsigned long kReadTimeoutMs=10000;
unsigned available_delay=1, read_delay=1;
size_t fifo_bytes=511;
bool realistic_fifo=false;
struct Client {
 size_t slot=0, delivered=0, fifo=0;
 size_t write(const uint8_t*,size_t length) { return length; }
 int available() {
  ticks+=available_delay;
  return realistic_fifo ? (fifo ? fifo : 4096-delivered) : 1024;
 }
 int read(uint8_t *bytes,size_t length) {
  ticks+=read_delay;
  if(realistic_fifo) {
   if(fifo<length) {
    size_t remaining=4096-delivered-fifo;
    fifo+=remaining<1023-fifo ? remaining : 1023-fifo;
   }
   assert(fifo>=length); fifo-=length;
  }
  for(size_t i=0;i<length;++i) bytes[i]=uint8_t(slot+(delivered+i)*19U);
  delivered+=length; return length;
 }
 size_t buffered_bytes() const { return realistic_fifo ? fifo : fifo_bytes; }
} clients[kTlsClientCount];
void print_tls_diagnostic(const char *,int) { ticks+=500; }
'''
suffix = r'''
int main() {
 for(size_t i=0;i<8;++i) clients[i].slot=i;
 assert(receive_burst(1));
 std::string result=Serial.text.str();
 assert(result.find("RX_BURST_BOUNDARY=1,272,8\n")!=std::string::npos);
 assert(result.find("RX_BURST_SLOT_TIMING=1,2000,4096,2,257,1,1,511\n")!=std::string::npos);
 assert(result.find("RX_BURST_SLOT_TIMING=1,2007,4096,16,271,1,1,511\n")!=std::string::npos);
 assert(result.find("RX_BURST_SLOT_BUDGET=1,2000,16,0,0,16,0,16,0,16,0,4096\n")!=std::string::npos);
 assert(result.find("RX_BURST_PASS=1,4096\n")!=std::string::npos);
 for(auto &client:clients) client.delivered=0;
 Serial.text.str(""); Serial.text.clear(); ticks=0;
 available_delay=700; read_delay=0; fifo_bytes=0;
 assert(!receive_burst(2));
 result=Serial.text.str();
 // Preserve the original loop's full inner pass. The actual boundary can
 // cross 10s inside an AT call; never label it exactly 10s or extend its limit.
 assert(result.find("RX_BURST_BOUNDARY=2,11202,0\n")!=std::string::npos);
 assert(result.find("RX_BURST_SLOT_TIMING=2,2000,512,700,6301,700,0,0\n")!=std::string::npos);
 assert(result.find("RX_BURST_FAILURE_COUNTS=512,512,512,512,512,512,512,512\n")!=std::string::npos);
 assert(result.find("RX_BURST_SLOT_BUDGET=2,2000,2,2,0,1400,1400,2,2,0,0,512\n")!=std::string::npos);
 assert(ticks>11202+500); // Later diagnostics cannot alter the frozen boundary.
 for(auto &client:clients) {client.delivered=0;client.fifo=0;}
 Serial.text.str(""); Serial.text.clear(); ticks=0;
 realistic_fifo=true; available_delay=1; read_delay=1;
 assert(receive_burst(3));
 result=Serial.text.str();
 assert(result.find("RX_BURST_BOUNDARY=3,289,8\n")!=std::string::npos);
 assert(result.find("RX_BURST_SLOT_BUDGET=3,2000,17,5,0,17,5,17,5,17,5,4096\n")!=std::string::npos);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(prefix+function+suffix)
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('RA4_TLS_RESOURCE_DEADLINE_BOUNDARY_PASS')
