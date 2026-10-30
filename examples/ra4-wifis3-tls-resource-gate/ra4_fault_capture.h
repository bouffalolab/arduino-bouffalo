#pragma once
#include <Arduino.h>
// Capture the original exception before Arduino's printing backtrace can
// fault again. No allocation, serial I/O, or application-stack use here.
extern "C" {
volatile uint32_t ra4_fault_snapshot[18] = {};
volatile uint32_t ra4_test_phase = 0;
alignas(8) uint32_t ra4_fault_stack[64];
__attribute__((used, noinline, noreturn))
void ra4_capture_fault(uint32_t *frame, uint32_t exception_return, uint32_t original_msp) {
  ra4_fault_snapshot[1] = exception_return;
  ra4_fault_snapshot[2] = reinterpret_cast<uint32_t>(frame);
  ra4_fault_snapshot[3] = original_msp;
  ra4_fault_snapshot[4] = SCB->CFSR;
  ra4_fault_snapshot[5] = SCB->HFSR;
  ra4_fault_snapshot[6] = SCB->MMFAR;
  ra4_fault_snapshot[7] = SCB->BFAR;
  ra4_fault_snapshot[16] = ra4_test_phase;
  // Only read an aligned basic exception frame within RA4M1 SRAM. Stacking
  // errors make the frame unreliable; record status and leave frame zeroed.
  if ((reinterpret_cast<uint32_t>(frame) & 3U) == 0U &&
      reinterpret_cast<uint32_t>(frame) >= 0x20000000U &&
      reinterpret_cast<uint32_t>(frame) <= 0x20007fe0U &&
      (SCB->CFSR & 0x00003030U) == 0U) {
    for (size_t i = 0; i < 8U; ++i) ra4_fault_snapshot[8U + i] = frame[i];
    ra4_fault_snapshot[17] = 1U;
  }
  ra4_fault_snapshot[0] = 0x52413446U;
  __DSB();
  while (true) __asm volatile("nop");
}
__attribute__((naked, used)) void ra4_fault_handler() {
  __asm volatile(
      "mrs r2, msp\n"
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "mov r1, lr\n"
      "ldr r3, =ra4_fault_stack + 256\n"
      "msr msp, r3\n"
      "b ra4_capture_fault\n");
}
}

