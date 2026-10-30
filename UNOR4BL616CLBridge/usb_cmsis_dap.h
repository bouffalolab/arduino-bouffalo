#ifndef UNOR4_BL616CL_USB_CMSIS_DAP_H
#define UNOR4_BL616CL_USB_CMSIS_DAP_H

#include <stdbool.h>
#include <stdint.h>

#include "ra4_bootstrap_control.h"
#include "user_uart_gpio35_control.h"

namespace bridge {

class Ra4AtService;
class Ra4SambaService;
class Ra4SambaProgrammer;

bool usb_cmsis_dap_begin();
void usb_cmsis_dap_poll();
bool usb_cmsis_dap_take_line_coding(uint32_t *baud);
void usb_cmsis_dap_note_line_coding_applied(uint32_t baud);
uint32_t usb_cmsis_dap_current_line_coding_baud();
bool usb_cmsis_dap_set_user_uart_bridge_enabled(bool enabled);
bool usb_cmsis_dap_user_uart_bridge_enabled();
bool usb_cmsis_dap_take_ra4_application_reset_request();
bool usb_cmsis_dap_take_ra4_samba_probe_request();
void usb_cmsis_dap_set_ra4_bootstrap_control(Ra4BootstrapControl *control);
void usb_cmsis_dap_set_user_uart_gpio35_control(UserUartGpio35Control *control);
void usb_cmsis_dap_set_ra4_at_service(Ra4AtService *service);
void usb_cmsis_dap_set_ra4_samba_service(Ra4SambaService *service);
void usb_cmsis_dap_set_ra4_samba_programmer(Ra4SambaProgrammer *programmer);

} // namespace bridge

#endif
