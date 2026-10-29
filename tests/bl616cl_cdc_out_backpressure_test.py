"""Execute production CDC receive/rearm/drain under a stalled UART consumer."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
def function(source,name):
    start=source.index('void '+name+'(') if 'void '+name+'(' in source else source.index('bool '+name+'(')
    brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
class BackpressureTest(unittest.TestCase):
    def test_slow_consumer_has_no_loss_or_double_arm(self):
        source=(ROOT/'UNOR4BL616CLBridge/usb_cmsis_dap.cpp').read_text()
        code=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <vector>
constexpr uint16_t kCdcToUartCapacity=512;
constexpr uint8_t kUsbBusId=0,kCdcOutEndpoint=2;
uint8_t cdc_rx[64],cdc_to_uart[512];
volatile uint16_t cdc_to_uart_head=0,cdc_to_uart_tail=0;
bool usb_configured=true,cdc_rx_armed=false,user_uart_bridge_enabled=true;
uint32_t cdc_out_packet_count=0,cdc_out_byte_count=0,cdc_to_uart_drop_count=0;
uint32_t uart_tx_failure_count=0,uart_tx_byte_count=0;
bool hardware_armed=false;int arms=0;bool fail_arm=false;
uintptr_t mm_lock_save(){return 0;}
void mm_unlock_restore(uintptr_t){}
int usbd_ep_start_read(uint8_t,uint8_t,uint8_t*,size_t){
 assert(!hardware_armed);if(fail_arm)return -1;
 hardware_armed=true;++arms;return 0;
}
std::vector<uint8_t> output;
struct {bool blocked=false;size_t write(uint8_t c){if(blocked)return 0;output.push_back(c);return 1;}} Serial;
'''
        for name in ('rearm_cdc_read','enqueue_cdc_byte','dequeue_cdc_byte','cdc_out_callback','forward_cdc_to_uart'):
            code+='\n'+function(source,name)+'\n'
        code+=r'''
int main(){
 rearm_cdc_read();rearm_cdc_read();assert(arms==1);
 std::vector<uint8_t> sent;
 // More packets than the ring can hold, with an arbitrarily stalled consumer.
 for(int packet=0;packet<2000;++packet){
  if(!hardware_armed){forward_cdc_to_uart();rearm_cdc_read();}
  assert(hardware_armed);hardware_armed=false;
  for(unsigned i=0;i<64;++i){cdc_rx[i]=(packet*29+i)%251;sent.push_back(cdc_rx[i]);}
  cdc_out_callback(0,2,64);
  if(packet%19==0){Serial.blocked=true;forward_cdc_to_uart();Serial.blocked=false;}
  if(packet%23==0)forward_cdc_to_uart();
 }
 forward_cdc_to_uart();rearm_cdc_read();
 assert(sent==output);assert(cdc_to_uart_drop_count==0);
 assert(cdc_out_byte_count==128000);assert(uart_tx_byte_count==128000);
 hardware_armed=false;cdc_rx_armed=false;fail_arm=true;
 rearm_cdc_read();assert(!cdc_rx_armed);
 fail_arm=false;rearm_cdc_read();assert(cdc_rx_armed);
 hardware_armed=false;cdc_rx_armed=false;usb_configured=false;
 int prior=arms;rearm_cdc_read();assert(arms==prior);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.cpp').write_text(code)
            subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
