/* BleAdvTest — BL616CL full-stack BLE advertising bring-up test.
 *
 * Reference: SDK examples btble_cli and wifi/sta/smartconfig_ble (the BL616CL
 * WiFi+BLE coexistence demo).  Both run the same on-chip stack combination on
 * BL616CL: the m2s1 controller flavor plus the blestack HCI host, started as
 *
 *   btble_controller_init() -> hci_driver_init() -> bt_enable(cb)
 *
 * with advertising started from the ready callback.  This sketch reproduces
 * that sequence inside the Arduino platform to verify the known-good SDK
 * libraries can drive BLE from the Arduino build (RF params and the EM window
 * are already set up by the core init() before setup() runs).
 *
 * Build with the m2s1 flavor and the btble_cli-built libraries:
 *   arduino-cli compile --config-file arduino-cli.yaml \
 *     --fqbn bouffalo:bl616cl:unor4_bl616cl \
 *     --build-path .build/BleAdvTest \
 *     --build-property "compiler.ble.libs=<...>/libbtblecontroller_bl616cl_m2s1.a <...>/libblestack.a -lrfparam -lbl616cl_phyrf -lshell" \
 *     --build-property "compiler.c.elf.extra_flags=-Wl,--defsym,__LD_CONFIG_EM_SIZE=16" \
 *     hardware/bouffalo/bl616cl/examples/BleAdvTest
 *
 * The Arduino_DebugUtils.h include pulls in the esp32-compat library, whose
 * lfs/bflb_mtd.c + lfs/lfs_easyflash.c provide the EasyFlash backend the BLE
 * host uses to persist bond keys (btble_cli and smartconfig_ble both init
 * EasyFlash before starting the stack).
 */

#include <Arduino.h>

#include "Arduino_DebugUtils.h" /* pulls in esp32-compat (EasyFlash sources) */

/* Type layouts mirrored from blestack src/include/bluetooth/bluetooth.h. */
struct bt_le_adv_param {
    uint8_t id;
    uint16_t options;
    uint16_t interval_min;
    uint16_t interval_max;
};
struct bt_data {
    uint8_t type;
    uint8_t data_len;
    const uint8_t *data;
};
struct bt_addr_le {
    uint8_t type;
    uint8_t val[6];
};

extern "C" {
/* m2s1 controller (libbtblecontroller_bl616cl_m2s1.a) */
void btble_controller_init(uint8_t task_priority);
/* blestack host */
void hci_driver_init(void);
int bt_enable(void (*cb)(int err));
int bt_set_name(const char *name);
void bt_get_local_public_address(struct bt_addr_le *addr);
int bt_le_adv_start(const struct bt_le_adv_param *param,
                    const struct bt_data *ad, size_t ad_len,
                    const struct bt_data *sd, size_t sd_len);
/* EasyFlash + MTD (esp32-compat sources) */
int bflb_mtd_init(void);
int easyflash_init(void);
}

/* Advertising option bits (bluetooth.h) */
#define BT_LE_ADV_OPT_CONNECTABLE 0x0001
#define BT_LE_ADV_OPT_ONE_TIME    0x0002
/* GAP advertising data types (gap.h) */
#define BT_DATA_FLAGS             0x01
#define BT_DATA_NAME_COMPLETE     0x09
#define BT_DATA_MANUFACTURER_DATA 0xff
/* Fast advertising interval range (gap.h) */
#define BT_GAP_ADV_FAST_INT_MIN_2 0x00a0 /* 100 ms */
#define BT_GAP_ADV_FAST_INT_MAX_2 0x00f0 /* 150 ms */

static const uint8_t s_adv_flags[] = { 0x06 }; /* LE General | BR/EDR not supported */
static const uint8_t s_adv_name[]  = "ZZARDUINO918";
static const uint8_t s_rsp_mfg[]   = { 'B', 'L', '6', '1', '6' };

static const struct bt_data s_adv[] = {
    { BT_DATA_FLAGS, sizeof(s_adv_flags), s_adv_flags },
    { BT_DATA_NAME_COMPLETE, sizeof(s_adv_name) - 1, s_adv_name },
};
static const struct bt_data s_rsp[] = {
    { BT_DATA_MANUFACTURER_DATA, sizeof(s_rsp_mfg), s_rsp_mfg },
};

static void start_advertising(void)
{
    struct bt_le_adv_param param = {};
    param.interval_min = BT_GAP_ADV_FAST_INT_MIN_2;
    param.interval_max = BT_GAP_ADV_FAST_INT_MAX_2;
    param.options = BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME;

    int ret = bt_le_adv_start(&param, s_adv, 2, s_rsp, 1);
    printf("[BleAdvTest] adv_start ret=%d\r\n", ret);
}

static void bt_ready_cb(int err)
{
    if (err != 0) {
        printf("[BleAdvTest] bt_enable failed err=%d\r\n", err);
        return;
    }

    struct bt_addr_le addr = {};
    bt_get_local_public_address(&addr);
    printf("[BleAdvTest] bt ready, BD_ADDR %02X:%02X:%02X:%02X:%02X:%02X\r\n",
           addr.val[5], addr.val[4], addr.val[3],
           addr.val[2], addr.val[1], addr.val[0]);

    int nret = bt_set_name("ZZARDUINO918");
    printf("[BleAdvTest] set_name ret=%d\r\n", nret);

    start_advertising();
}

void setup()
{
    printf("\r\n[BleAdvTest] setup\r\n");

    int mtd_ret = bflb_mtd_init();
    int ef_ret = easyflash_init();
    printf("[BleAdvTest] mtd=%d easyflash=%d\r\n", mtd_ret, ef_ret);

    btble_controller_init((uint8_t)(configMAX_PRIORITIES - 1));
    printf("[BleAdvTest] controller init done\r\n");

    hci_driver_init();
    printf("[BleAdvTest] hci driver init done\r\n");

    bt_enable(bt_ready_cb);
    printf("[BleAdvTest] bt_enable called\r\n");
}

void loop()
{
    static uint32_t n = 0;
    printf("[BleAdvTest] alive %lu\r\n", (unsigned long)n++);
    delay(5000);
}
