#!/usr/bin/env python3
"""Execute actual DMA drain with short packets, wrap, overflow and MMIO races."""
from pathlib import Path
import subprocess,tempfile
from bl616cl_sta_disconnect_wait_test import function
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'UNOR4BL616CLBridge/uart_transport.cpp').read_text()
body=function(source,'void UartTransport::drain_rx_dma()')+'\n'+function(source,'size_t UartTransport::read(')+'\n'+function(source,'void UartTransport::discard_rx()')
prefix=r'''
#define BL616CL_AT_RX_DMA 1
#include <cassert>
#include <cstdint>
#include <cstddef>
#include "dma_rx_cursor.h"
#include "byte_ring.h"
using namespace bridge;
constexpr uint32_t kDmaRxBytes=2048;
static uint8_t dma_rx_bytes[kDmaRxBytes];
constexpr int DMA_CMD_GET_LLI_COUNT=1,DMA_CMD_GET_LLI_DSTADDR=2;
static uint32_t count,address;
static bool race;
static unsigned count_reads;
int bflb_dma_feature_control(void*,int cmd,int) {
 if(cmd==DMA_CMD_GET_LLI_DSTADDR) return static_cast<int>(address);
 ++count_reads;
 return static_cast<int>(count + (race && count_reads%2==0 ? 1 : 0));
}
uintptr_t bflb_irq_save() {return 42;}
void bflb_irq_restore(uintptr_t state) {assert(state==42);}
struct UartTransport {
 void *rx_dma_device_=nullptr;
 uint32_t dma_last_count_=0,dma_last_offset_=0;
 uint32_t rx_overflow_count_=0,received_bytes_=0;
 uint32_t dma_available_=0,dma_read_offset_=0;
 uint32_t reported_rx_overflow_count_=0;
 void observe_rx_errors() {}
 size_t read(uint8_t *destination,size_t limit);
 void drain_rx_dma();
 void discard_rx();
};
'''
main=r'''
int main() {
 UartTransport u; u.drain_rx_dma();assert(count_reads==0);
 u.rx_dma_device_=&u;
 const uint32_t base=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dma_rx_bytes));
 for(unsigned i=0;i<2048;++i)dma_rx_bytes[i]=i%251;
 uint8_t out[4096]={};
 address=base+7;u.drain_rx_dma(); // RTO/short command before half IRQ.
 assert(u.read(out,4096)==7 && out[6]==6);
 address=base+1800;count=1;u.drain_rx_dma();
 assert(u.read(out,4096)==1793 && out[0]==7 && out[1792]==1799%251);
 address=base+100;count=2;u.drain_rx_dma(); // descriptor + buffer wrap
 assert(u.read(out,4096)==348 && out[247]==2047%251 && out[248]==0);
 address=base+200;race=true;u.drain_rx_dma();
 assert(u.dma_last_offset_==100 && u.dma_available_==0);
 race=false;u.drain_rx_dma();assert(u.read(out,4096)==100);
 address=base+201;count=4;u.drain_rx_dma(); // full overwrite detected
 assert(u.rx_overflow_count_==1 && u.dma_available_==0 && u.dma_last_offset_==201);
 address=base-1;u.drain_rx_dma();assert(u.dma_last_offset_==201);
 // A two-descriptor count change can be less than a ring, if offset decreases.
 assert(!dma_rx_overwritten(5,7,1800,1000));
 assert(dma_rx_overwritten(5,7,1000,1800));
 assert(dma_rx_overwritten(5,8,1800,100));
 assert(!dma_rx_overwritten(1023,0,1800,100));
 // Consumer delay filling the direct DMA ring must trigger recovery.
 u.dma_available_=0;u.dma_last_count_=0;u.dma_last_offset_=0;
 u.dma_read_offset_=0;count=1;address=base+1024;u.drain_rx_dma();
 assert(u.dma_available_==1024);
 count=2;address=base;u.drain_rx_dma();
 assert(u.rx_overflow_count_==2 && u.dma_available_==0);
 // Task-side DMA discovery must not feed queued bytes before reset.
 address=base+3;u.drain_rx_dma();
 assert(u.read(out,3)==0 && u.dma_available_==3);
 u.reported_rx_overflow_count_=u.rx_overflow_count_;
 assert(u.read(nullptr,3)==0 && u.dma_available_==3);
 assert(u.read(out,3)==3 && out[2]==2);
 address=base+10;assert(u.read(out,2)==2 && out[0]==3 && out[1]==4);
 assert(u.dma_available_==5);
 u.discard_rx();assert(u.dma_available_==0 && u.dma_read_offset_==10);
 address=base+11;assert(u.read(out,3)==1 && out[0]==10);
}
'''
with tempfile.TemporaryDirectory(prefix='bl616cl-uart-dma-') as temp:
 p=Path(temp);(p/'test.cpp').write_text(prefix+body+main)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'UNOR4BL616CLBridge'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('BL616CL_UART_DMA_RX_PASS')
