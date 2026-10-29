#!/usr/bin/env python3
"""Exercise actual passive UART observer, including masked/sticky errors."""
from pathlib import Path
import subprocess
import tempfile
from bl616cl_sta_disconnect_wait_test import function
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'UNOR4BL616CLBridge/uart_transport.cpp').read_text()
body = function(source, 'void UartTransport::observe_rx_errors()')
# All MMIO is read-only in the actual function; error masks remain unchanged.
assert 'putreg' not in body and 'int_clear' not in body and 'int_mask' not in body
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
constexpr unsigned UART_INT_STS_OFFSET=0x20, UART_FIFO_CONFIG_0_OFFSET=0x80;
constexpr unsigned UART_INTSTS_RX_FER=128, UART_INTSTS_PCE=32, UART_INTSTS_RX_LSE=256;
constexpr unsigned UART_RX_FIFO_OVERFLOW=64, UART_RX_FIFO_UNDERFLOW=128;
struct Device {uintptr_t reg_base;};
static uint32_t status, fifo;
static unsigned reads;
uint32_t getreg32(uintptr_t address) {
 ++reads;
 assert(address==0x1020 || address==0x1080);
 return address==0x1020 ? status : fifo;
}
struct UartTransport {
 Device *device_=nullptr;
 volatile uint32_t rx_error_status_seen_=0, rx_fifo_errors_seen_=0;
 void observe_rx_errors();
};
'''
main = r'''
int main() {
 UartTransport uart;
 uart.observe_rx_errors(); assert(reads==0);
 Device device{0x1000}; uart.device_=&device;
 status=8|16|128|32; fifo=1|64;
 uart.observe_rx_errors();
 assert(uart.rx_error_status_seen_==160 && uart.rx_fifo_errors_seen_==64);
 // Sticky flags are bitsets, not falsely increasing event counts.
 uart.observe_rx_errors();
 assert(uart.rx_error_status_seen_==160 && uart.rx_fifo_errors_seen_==64);
 status=256; fifo=128;
 uart.observe_rx_errors();
 assert(uart.rx_error_status_seen_==416 && uart.rx_fifo_errors_seen_==192);
 status=0; fifo=0; uart.observe_rx_errors();
 assert(uart.rx_error_status_seen_==416 && uart.rx_fifo_errors_seen_==192);
 assert(reads==8);
}
'''
with tempfile.TemporaryDirectory(prefix='bl616cl-uart-observer-') as temp:
 path=Path(temp); (path/'test.cpp').write_text(prefix+body+main)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
print('BL616CL_UART_ERROR_OBSERVER_PASS')
