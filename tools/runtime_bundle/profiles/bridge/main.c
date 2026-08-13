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
