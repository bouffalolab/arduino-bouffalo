#include "board.h"

#include "bflb_mtd.h"
#include "easyflash.h"
#include "fhost_api.h"
#include "lfs.h"
#include "lwip/tcpip.h"
#include "mbedtls/sha256.h"
#include "usbd_cdc_acm.h"
#include "usbd_core.h"
#include "usbd_hid.h"
#include "wifi_mgmr_ext.h"

__attribute__((weak)) void fhost_bridge_ap_start_progress(uint8_t stage,
                                                          int result)
{
    (void)stage;
    (void)result;
}

__attribute__((weak)) void fhost_bridge_ap_start_pointer(uint8_t location,
                                                         uint32_t address)
{
    (void)location;
    (void)address;
}

__attribute__((weak)) void fhost_bridge_ap_start_channel_counts(
    uint32_t chan2g4_count,
    uint32_t chan5g_count)
{
    (void)chan2g4_count;
    (void)chan5g_count;
}

__attribute__((weak)) void fhost_bridge_ap_start_heap_before_allocate(
    uint32_t size)
{
    (void)size;
}

__attribute__((weak)) void fhost_bridge_ap_start_heap_after_allocate(
    uint32_t address)
{
    (void)address;
}

__attribute__((weak)) void fhost_bridge_ap_start_allocator_progress(
    uint8_t stage)
{
    (void)stage;
}

__attribute__((weak)) void fhost_bridge_ap_start_trace(
    uint8_t event,
    uint32_t semaphore,
    int result,
    uint32_t count)
{
    (void)event;
    (void)semaphore;
    (void)result;
    (void)count;
}

__attribute__((weak)) void fhost_bridge_ap_start_allocator_event(
    uint8_t stage,
    uint32_t value0,
    uint32_t value1,
    uint32_t value2)
{
    (void)stage;
    (void)value0;
    (void)value1;
    (void)value2;
}

int main(void)
{
    board_init();

    /*
     * This is a link-profile probe, not bridge firmware. It compiles the
     * public service families needed by the future bridge.
     */
    (void)sizeof(mbedtls_sha256_context);
    (void)sizeof(struct usbd_endpoint);
    (void)sizeof(lfs_t);

    while (1) {
    }
}
