// btblecontroller port implementation backed by the HCIVirtualTransport
// queues instead of a hardware UART.
//
// The controller library references btble_uart_read / btble_uart_write /
// btble_uart_flow_on / btble_uart_flow_off through its rwip_eif_api table
// (plf/refip/src/arch/main/arch_main.c), so defining them here wires the
// controller's HCI engine to the AT-facing virtual transport:
//
//   host HCI bytes (AT+HCI_WRITE) -> hci_h2c_push -> armed read -> controller
//   controller events/ACL -> btble_uart_write -> hci_c2h_push -> AT+HCI_READ
//
// The stock port drives these functions from a UART ISR; here the read
// callback is invoked from the task that delivered the data (see
// hci_port_deliver), which is a strictly safer context.

#include "ble/ble_hci_port.h"
#include "utility/hci_transport_internal.h"

#include <Arduino.h>
#include <FreeRTOS.h>

#include "bflb_uart.h"
#include "rfparam_adapter.h"
#include "mm.h"

extern "C" void ensure_rfparam(void);
extern "C" void btble_controller_init(uint8_t task_priority);
extern "C" void btble_ke_event_schedule(void);

static bool s_started = false;

extern "C" uint32_t compat_get_free_heap(void)
{
    return (uint32_t)kfree_size(0);
}

/* BFLB_LOG=n stubs out printf in this build; write debug marks straight to
 * the board console UART0 (GPIO34/35 @ 2 Mbit/s, the FT232 on the bench). */
static void ble_dbg(const char *msg)
{
    struct bflb_device_s *uart = bflb_device_get_by_name("uart0");
    if (uart == NULL) {
        return;
    }
    while (*msg != '\0') {
        bflb_uart_putchar(uart, *msg++);
    }
}

bool ble_controller_start(void)
{
    if (s_started) {
        return true;
    }
    // Mark started before initialising so a re-entrant begin() (the AT
    // command retrying after a timeout) cannot start the controller twice.
    s_started = true;

    ble_dbg("[ble] rfparam start\r\n");
    ensure_rfparam();
    ble_dbg("[ble] rfparam done\r\n");
    hci_transport_reset();
    ble_dbg("[ble] btble_controller_init ...\r\n");
    btble_controller_init((uint8_t)(configMAX_PRIORITIES - 1));
    ble_dbg("[ble] btble_controller_init done\r\n");
    return true;
}

extern "C" {

void btble_uart_pin_config(uint8_t uartid, uint8_t tx, uint8_t rx,
                           uint8_t cts, uint8_t rts)
{
    (void)uartid;
    (void)tx;
    (void)rx;
    (void)cts;
    (void)rts;
}

void btble_uart_init(uint8_t uartid)
{
    (void)uartid;
    hci_transport_reset();
}

int8_t btble_uart_reconfig(uint32_t baudrate, uint8_t flow_ctl_en,
                           uint8_t cts_pin, uint8_t rts_pin)
{
    (void)baudrate;
    (void)flow_ctl_en;
    (void)cts_pin;
    (void)rts_pin;
    return 0;
}

void btble_uart_flow_on(void)
{
}

bool btble_uart_flow_off(void)
{
    return true;
}

void btble_uart_finish_transfers(void)
{
}

void btble_uart_read(uint8_t *bufptr, uint32_t size,
                     void (*callback)(void *, uint8_t), void *dummy)
{
    hci_port_arm_read(bufptr, size, callback, dummy);
    hci_port_deliver();
}

void btble_uart_write(const uint8_t *bufptr, uint32_t size,
                      void (*callback)(void *, uint8_t), void *dummy)
{
    hci_transport_note_write();
    hci_c2h_push(bufptr, size);
    if (callback != nullptr) {
        // The stock port signals completion from the UART ISR; calling the
        // callback inline from the controller task is equivalent here since
        // nothing asynchronous is involved.
        callback(dummy, 0);
        // The H4TL tx callback registers a deferred job (tx_done) which the
        // ROM wake chain never runs here; drain it synchronously so the HCI
        // TL tx state machine advances to the next queued event.
        btble_ke_event_schedule();
    }
}

}  // extern "C"
