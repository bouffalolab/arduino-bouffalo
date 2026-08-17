/**
 ****************************************************************************************
 *
 * @file coexm.h
 *
 * @brief WiFi/BLE Coexistence Power Management module - Public API
 *
 * This module manages PTA (Packet Traffic Arbiter) switching for WiFi/BLE coexistence.
 * It coordinates RF access between WiFi and BLE through time-division multiplexing.
 *
 * This is the PUBLIC header for external modules. MACSW internal modules should
 * include both this file and coexm_int.h for internal APIs.
 *
 * Copyright (C) Bouffalo Lab 2024
 *
 ****************************************************************************************
 */

#ifndef __MACSW_COEXM_PUBLIC_H__
#define __MACSW_COEXM_PUBLIC_H__

/**
 ****************************************************************************************
 * @defgroup COEXM WiFi/BLE Coexistence Manager
 * @ingroup MACSW
 * @brief WiFi/BLE Coexistence power management module
 *
 * @{
 ****************************************************************************************
 */

/*
 * INCLUDE FILES
 ****************************************************************************************
 */
#include <stdbool.h>
#include <stdint.h>

struct mac_chan_op;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DEFINITIONS
 ****************************************************************************************
 */

/**
 * @brief PTA (Packet Traffic Arbiter) role definitions
 *
 * Defines the different operating modes for the PTA hardware which controls
 * RF access arbitration between WiFi, BLE, Thread, and other protocols.
 */
enum pta_role {
    PTA_ROLE_BT = 0,                    ///< BLE exclusive mode (BLE has priority)
    PTA_ROLE_WIFI,                      ///< WiFi exclusive mode (WiFi has priority)
    PTA_ROLE_WIFI_AND_BT_DEFAULT,       ///< Compete mode (WiFi and BLE use default priority)
    PTA_ROLE_THREAD,                    ///< Thread exclusive mode
    PTA_ROLE_PTI,                       ///< PTI (Packet Traffic Information) mode
};

/** Fixed coexistence configurations selected for the current Wi-Fi link. */
enum coex_config_id {
    COEX_CONFIG_COMBO = 0,
    COEX_CONFIG_STANDALONE_DUAL_ANT = 1,
    COEX_CONFIG_STANDALONE_SPDT = 2,
    COEX_CONFIG_MAX = 3,
};

/** Result codes returned while resolving or applying a fixed configuration. */
enum coex_config_status {
    COEX_CONFIG_OK = 0,
    COEX_CONFIG_ERR_INVALID_PARAM = -1,
    COEX_CONFIG_ERR_UNSUPPORTED = -2,
    COEX_CONFIG_ERR_MODE_REQUIRED = -3,
    COEX_CONFIG_ERR_INVALID_COMBINATION = -4,
    COEX_CONFIG_ERR_HARDWARE_NOT_READY = -5,
    COEX_CONFIG_ERR_BUSY = -6,
};

/** Raw request received from the Wi-Fi control layer. */
struct coex_config_request {
    bool config_present;
    uint8_t config_id;
    bool ps_pta_requested;
};

/** Chip backend result used by the common control flow. */
struct coex_config_resolved {
    enum coex_config_id config_id;
    bool ps_pta_enable;
};

/*
 * TYPE DEFINITIONS
 ****************************************************************************************
 */

/**
 * @brief Initialization parameters for PM COEX module.
 */
typedef struct {
    bool pta_force_enable;              ///< Force enable PTA on init
} pm_coex_init_params_t;

/*
 * FUNCTION DECLARATIONS
 ****************************************************************************************
 */

/**
 * @brief Initialize the PM COEX module.
 * @param params Initialization parameters (must not be NULL).
 * @return 0 on success, -1 on failure.
 */
int pm_coex_init(const pm_coex_init_params_t *params);

/**
 * @brief De-initialize the PM COEX module.
 * @note Restores the PTA state to its original configuration.
 * @return 0 on success, -1 on failure.
 */
int pm_coex_deinit(void);

/**
 * @brief Enter sleep mode.
 * @note Performs a pre-check to ensure the system can sleep safely.
 *
 * @return 0 on success. On failure returns a non-zero bitmask (see
 *         PM_COEX_SLEEP_FAIL_*).
 */
int pm_coex_sleep(void);

/**
 * @brief Wake the system from sleep.
 * @return 0 on success, -1 on failure.
 */
int pm_coex_wakeup(void);

/**
 * @brief Pause the power management functionality.
 * @details When paused, pm_coex_sleep and pm_coex_wakeup calls will be ignored.
 * @return 0 on success, -1 if PM not initialized.
 */
int pm_coex_pause(void);

/**
 * @brief Resume the power management functionality.
 * @details Allows pm_coex_sleep and pm_coex_wakeup calls to function normally.
 * @return 0 on success, -1 if PM not initialized.
 */
int pm_coex_resume(void);

/**
 * @brief Coex coordinator "gate" (master enable).
 *
 * Persistent enable gate used by coex coordinator to decide whether coex-related
 * behaviors may be attached (regardless of runtime pause state).
 *
 * @return true if coexistence mode is enabled, false otherwise.
 */
bool coex_coord_is_enabled(void);

bool coex_coord_is_active(void);

/**
 * @brief Backward-compatible alias of @ref coex_coord_is_enabled.
 *
 * @deprecated Prefer @ref coex_coord_is_enabled for coordinator semantics.
 */
bool ps_is_coex_mode(void);

/** Return true when the selected platform supports explicit 2.4 GHz configs. */
bool coexm_supports_explicit_2g_config(void);

/** Resolve a user request for the specified Wi-Fi band. */
int coexm_resolve_config(const struct coex_config_request *request,
                         uint8_t band,
                         struct coex_config_resolved *resolved);

/** Apply a previously resolved hardware configuration. */
int coexm_apply_config(const struct coex_config_resolved *resolved,
                       uint8_t band);

/*
 * COEX coordinator - Stage 1 entrypoints
 ****************************************************************************************
 */

/// TXQ pending query callback (implemented by fhost, registered at runtime)
typedef bool (*coex_txq_has_pending_cb_t)(uint8_t vif_idx);

/**
 * @brief Register a callback for querying whether Host TXQ has pending packets.
 *
 * This avoids tight linkage between macsw (coex coordinator) and fhost.
 * The callback SHALL be fast and non-blocking.
 */
void coex_coord_register_txq_has_pending_cb(coex_txq_has_pending_cb_t cb);

/**
 * @brief TBTT hook (Wi-Fi RX anchor) for coex coordinator.
 * @param tbtt_time   TBTT timestamp (us, MAC timer).
 * @param vif_index   MAC VIF index associated with this TBTT.
 */
void coex_coord_on_tbtt(uint32_t tbtt_time, uint8_t vif_index);

/**
 * @brief Called by platform right before Wi-Fi task blocks.
 *
 * @return true if `pm_coex_sleep()` commit succeeded and caller MUST pair with
 *         `coex_coord_on_wifi_wake(true)`.
 */
bool coex_coord_on_wifi_suspend_enter(void);

/**
 * @brief Called by platform right after Wi-Fi task is woken up.
 * @param slept_committed  Return value from @ref coex_coord_on_wifi_suspend_enter.
 */
void coex_coord_on_wifi_wake(bool slept_committed);

/**
 * @brief Coex coordinator runtime hooks (called by PS layer).
 *
 * These hooks control the explicitly enabled PS_PTA runtime for the current
 * connection. A real disconnect invokes disable; reconnect does not restore it.
 */
void coex_coord_on_enable(void);
void coex_coord_on_disable(void);
void coex_coord_on_runtime_pause(void);
void coex_coord_on_runtime_resume(void);

/*
 * pm_coex_sleep failure bitmask (Stage 1)
 ****************************************************************************************
 */

/// pm_coex_sleep(): guard / not ready (not init, runtime paused, wrong state)
#define PM_COEX_SLEEP_FAIL_GUARD         (1u << 0)
/// PS is OFF
#define PM_COEX_SLEEP_FAIL_PS_OFF        (1u << 1)
/// prevent_sleep is active (global or per-VIF)
#define PM_COEX_SLEEP_FAIL_PREVENT_SLEEP (1u << 2)
/// TX path is not sleepable (TX inflight / pck_cnt etc)
#define PM_COEX_SLEEP_FAIL_TX_INFLIGHT   (1u << 3)
/// HW timer / MAC state blocks sleep
#define PM_COEX_SLEEP_FAIL_HW_TIMER      (1u << 4)
/// KE msg queue not empty / state not allowing sleep
#define PM_COEX_SLEEP_FAIL_KE_MSG        (1u << 5)
/// CPU / events block sleep
#define PM_COEX_SLEEP_FAIL_CPU           (1u << 6)
/// pm_coex_sleep_ctl(true) failed
#define PM_COEX_SLEEP_FAIL_SLEEP_CTL     (1u << 7)

/**
 * @brief Pause coexistence runtime behaviors
 * @details Pauses runtime behaviors (PTA switching, TBTT slice, host TX gating)
 *          while preserving the current-connection enable state.
 *
 * This is typically triggered when Wi-Fi PS is turned OFF (e.g. disconnect).
 * Does nothing if coex is disabled.
 *
 * @note Coex protection is controlled separately and remains disabled by
 *       default in the explicit-activation design.
 */
void ps_coex_runtime_pause(void);

/**
 * @brief Resume coexistence runtime behaviors
 * @details Resumes runtime behaviors after Wi-Fi PS is turned ON.
 *          Does nothing if coex is disabled.
 * @note Wi-Fi PS re-enable must be handled separately by caller.
 * @note This function does not enable or resume coex protection.
 */
void ps_coex_runtime_resume(void);

/**
 * @brief Check if currently in WiFi active window (WiFi's time slice).
 * @details Returns true if WiFi is in its allocated time slice and can use PTA_ROLE_WIFI.
 *          Returns false if outside WiFi's time slice (e.g., BLE's time slice), where WiFi
 *          should compete using PTA_ROLE_WIFI_AND_BT_DEFAULT instead.
 * @return true if in WiFi active window, false otherwise.
 */
bool pm_coex_is_wifi_active_window(void);

/*
 * COEX PROTECTION API
 ****************************************************************************************
 */

/**
 * @brief Coex protection types for RF access control
 *
 * Different WiFi operations that require exclusive RF access during coexistence.
 */
typedef enum {
    COEX_PROT_SCAN = 0,      ///< WiFi scan protection
    COEX_PROT_CONNECT,       ///< WiFi connect protection (AUTH/ASSOC)
    COEX_PROT_DHCP,          ///< DHCP protection
    COEX_PROT_DISCONNECT,    ///< Disconnect protection (deauth TX)
    COEX_PROT_KEY_MGMT,      ///< Key management protection (ASSOCIATED->AUTHORIZED)
    COEX_PROT_MAX
} coex_protect_type_t;

/// Timeout for 4-way handshake wait (milliseconds)
#define PM_COEX_HANDSHAKE_WAIT_TIMEOUT_MS  10000

/**
 * @brief Enable/disable coex protection module
 * @param enable true to enable, false to disable
 */
void coex_protect_set_enabled(bool enable);

/**
 * @brief Check if coex protection module is enabled
 * @return true if enabled, false otherwise
 */
bool coex_protect_is_enabled(void);

/**
 * @brief Acquire RF protection for a specific operation type
 * @param type Protection type (COEX_PROT_SCAN, COEX_PROT_CONNECT, etc.)
 * @param band Wi-Fi band value, e.g. PHY_BAND_2G4 or PHY_BAND_5G.
 * @return 0 if protection was acquired, 1 if skipped by band policy, -1 on error
 */
int coex_protect_acquire(coex_protect_type_t type, uint8_t band);

/**
 * @brief Release RF protection for a specific operation type
 * @param type Protection type to release
 * @param band Wi-Fi band value, e.g. PHY_BAND_2G4 or PHY_BAND_5G.
 * @return 0 if protection was released, 1 if skipped by band policy, -1 on error
 */
int coex_protect_release(coex_protect_type_t type, uint8_t band);

/**
 * @brief Release all RF protections (emergency reset)
 */
void coex_protect_release_all(void);

/**
 * @brief Check if any RF protection is currently active
 * @return true if protection is active, false otherwise
 */
bool coex_protect_is_active(void);

/**
 * @brief Get current protection bitmask (read-only)
 * @return Active protection bitmask (bit per type)
 */
uint32_t coex_protect_get_mask(void);

/**
 * @brief Dump coex protection state for debugging
 */
void coex_protect_dump(void);

/*
 * COEX STATUS QUERY API (Read-Only)
 ****************************************************************************************
 */

/**
 * @brief Coex status snapshot for read-only query
 *
 * This structure captures a point-in-time snapshot of coex state.
 * All fields are read-only and do not modify any internal state.
 */
struct pm_coex_status {
    /* PS layer coex gate */
    uint8_t ps_coex_state;          ///< ps_env.coex_state (PS_COEX_DISABLED/ENABLED/RUNNING)

    /* pm_coex state machine */
    uint8_t pm_state;               ///< g_pm_coex_ctx.state (PM_COEX_STATE_*)
    uint8_t pta_current_role;       ///< g_pm_coex_ctx.pta_current_role (PTA_ROLE_*)
    bool wifi_active_window;        ///< g_pm_coex_ctx.wifi_active_window
    bool wifi_connecting;           ///< g_pm_coex_ctx.wifi_connecting

    /* WiFi duty cycle config */
    uint32_t wifi_duty_ms;          ///< WiFi active time in ms (configured via wifi_sta_coex_duty_set)

    /* coex protect */
    bool protect_enabled;           ///< coex_protect module enabled
    bool protect_active;            ///< any protection currently active
    uint32_t protect_mask;          ///< active protection bitmask

    /** BT-path coexistence configuration snapshot */
    bool bt_path_spdt_ctrl_enabled;     ///< User enabled BT path SPDT control.
    bool bt_path_adj_tx_power_enabled;  ///< BT path adjusted TX power configured.
    bool bt_path_channel_overlay_enabled; ///< BT path channel overlay detection configured.

    uint8_t bt_path_channel_overlay_margin; ///< Channel overlay detection margin.
    uint32_t bt_path_adj_tx_power_reg;  ///< RF power control register snapshot for adjusted TX power.
};

/**
 * @brief Get coex status snapshot (read-only)
 *
 * @param[out] out  Pointer to status structure to fill
 * @return 0 on success, -1 if out is NULL or module not initialized
 *
 * @note This function is safe to call from any context.
 *       Uses GLOBAL_INT_DISABLE() internally for atomic read.
 * @note This function does NOT modify any internal state.
 */
int pm_coex_get_status(struct pm_coex_status *out);

/* Print coex register snapshot (current vs default captured at pm_coex_init). */
void pm_coex_dump_registers(void);

/**
 * @brief Error codes returned by BT-path coexistence configuration APIs.
 *
 * Mutating BT-path configuration APIs return
 * COEXM_CONFIG_BT_PATH_COEX_ERR_BUSY while PS_PTA is enabled. Disable the
 * Wi-Fi coex runtime before changing RF topology or its hardware recipe.
 */
typedef enum {
    COEXM_CONFIG_BT_PATH_COEX_OK = 0,                                             ///< BT path coexistence configured.
    COEXM_CONFIG_BT_PATH_COEX_ERR_DEDICATED_ANTENNA_ON_2G_PATH = -1,              ///< Dedicated BT path antenna requested while BT path is on 2G path.
    COEXM_CONFIG_BT_PATH_COEX_ERR_BLE_TX_POWER_RANGE = -6,                        ///< BLE TX power value is out of range.
    COEXM_CONFIG_BT_PATH_COEX_ERR_IEEE802154_TX_POWER_RANGE = -7,                 ///< IEEE 802.15.4 TX power value is out of range.
    COEXM_CONFIG_BT_PATH_COEX_ERR_BT_TX_POWER_RANGE = -8,                         ///< BT TX power value is out of range.
    COEXM_CONFIG_BT_PATH_COEX_ERR_CHANNEL_MARGIN_RANGE = -9,                     ///< Channel margin is out of range.
    COEXM_CONFIG_BT_PATH_COEX_ERR_SPDT_CTRL_ON_2G_PATH = -10,                    ///< SPDT control is only valid when BT uses BT path.
    COEXM_CONFIG_BT_PATH_COEX_ERR_BUSY = -11,                                    ///< PS_PTA is active; disable coex before changing BT-path hardware.
    COEXM_CONFIG_BT_PATH_COEX_ERR_BT_PATH_REQUIRED = -12,                         ///< Requested mode requires BT to use BT path.
} coexm_config_bt_path_coex_err_t;

/**
 * @brief Enable or disable SPDT control for BT-path coexistence.
 *
 * @param enable true to route BT path through the SPDT shared with Wi-Fi 2G
 *               antenna, false to disable SPDT control for dedicated BT path
 *               antenna mode.
 *
 * @note These coexm BT APIs configure BT path coexistence hardware, not the BT
 *       protocol stack.
 * @note Call this function after PHY/RF has routed BT/IEEE 802.15.4 to BT path.
 * @note This API controls the COEXM/PTA SPDT coexistence bit. It does not
 *       configure the RF switch GPIO mux or RF/FEM path-selection registers.
 * @note The application must configure the switch-control GPIO mux function to
 *       25. After BT wins RF access internally, the mux-25 GPIO polarity is
 *       index-based: even GPIOs such as IO0 drive high, and odd GPIOs such as
 *       IO1 drive low.
 *
 * @return COEXM_CONFIG_BT_PATH_COEX_OK on success; otherwise one of
 *         coexm_config_bt_path_coex_err_t error codes.
 */
coexm_config_bt_path_coex_err_t coexm_bt_set_spdt_ctrl(bool enable);

/**
 * @brief Configure BLE adjusted TX power when BT path uses a dedicated
 *        antenna.
 *
 * @param pwr BLE adjusted TX power, in 0.25 dBm units.
 *
 * @note Call this function after PHY/RF has routed BT/IEEE 802.15.4 to BT path.
 * @note This enables BT path TX power reduction and disables channel overlay
 *       detection because the two features are mutually exclusive.
 *
 * @return COEXM_CONFIG_BT_PATH_COEX_OK on success; otherwise one of
 *         coexm_config_bt_path_coex_err_t error codes.
 */
coexm_config_bt_path_coex_err_t coexm_bt_set_adj_ble_tx_power(
    int8_t pwr);

/**
 * @brief Configure IEEE 802.15.4 adjusted TX power when BT path uses a
 *        dedicated antenna.
 *
 * @param pwr IEEE 802.15.4 adjusted TX power, in 0.25 dBm units.
 *
 * @note Call this function after PHY/RF has routed BT/IEEE 802.15.4 to BT path.
 * @note This enables BT path TX power reduction and disables channel overlay
 *       detection because the two features are mutually exclusive.
 *
 * @return COEXM_CONFIG_BT_PATH_COEX_OK on success; otherwise one of
 *         coexm_config_bt_path_coex_err_t error codes.
 */
coexm_config_bt_path_coex_err_t coexm_bt_set_adj_ieee802154_tx_power(
    int8_t pwr);

/**
 * @brief Configure BT adjusted TX power when BT path uses a dedicated
 *        antenna.
 *
 * @param pwr BT adjusted TX power, in 0.25 dBm units.
 *
 * @note Call this function after PHY/RF has routed BT/IEEE 802.15.4 to BT path.
 * @note This enables BT path TX power reduction and disables channel overlay
 *       detection because the two features are mutually exclusive.
 *
 * @return COEXM_CONFIG_BT_PATH_COEX_OK on success; otherwise one of
 *         coexm_config_bt_path_coex_err_t error codes.
 */
coexm_config_bt_path_coex_err_t coexm_bt_set_adj_bt_tx_power(
    int8_t pwr);

/**
 * @brief Disable BT path adjusted TX power.
 *
 * @note This only disables the TX power reduction feature and clears the
 *       corresponding coexm state. It does not change BT path routing.
 *
 * @return COEXM_CONFIG_BT_PATH_COEX_OK on success; otherwise one of
 *         coexm_config_bt_path_coex_err_t error codes.
 */
coexm_config_bt_path_coex_err_t coexm_bt_set_adj_tx_power_off(void);

/**
 * @brief Configure channel overlay detection when BT path uses a dedicated
 *        antenna.
 *
 * @param margin Channel overlay detection margin, valid range is 0..63.
 *
 * @note Call this function after PHY/RF has routed BT/IEEE 802.15.4 to BT path.
 * @note This enables channel overlay detection and disables BT path TX power
 *       reduction because the two features are mutually exclusive.
 *
 * @return COEXM_CONFIG_BT_PATH_COEX_OK on success; otherwise one of
 *         coexm_config_bt_path_coex_err_t error codes.
 */
coexm_config_bt_path_coex_err_t coexm_bt_set_channel_overlay(
    int margin);

/**
 * @brief Disable BT path channel overlay detection.
 *
 * @note This only disables channel overlay detection and clears the configured
 *       margin. It does not change BT path routing.
 *
 * @return COEXM_CONFIG_BT_PATH_COEX_OK on success; otherwise one of
 *         coexm_config_bt_path_coex_err_t error codes.
 */
coexm_config_bt_path_coex_err_t coexm_bt_set_channel_overlay_off(void);

/**
 * @brief Update BT path coexistence configuration with the current Wi-Fi channel.
 *
 * @param chan Current Wi-Fi operating channel.
 *
 * @note This is intended to be called after Wi-Fi connects to an AP or moves to
 *       a stable operating channel. Scan channel changes should not call this
 *       API, so channel overlay detection uses the connected AP channel instead
 *       of temporary scan channels.
 * @note Passing a 2.4 GHz channel with center1_freq set to 0 is treated as a
 *       priority reset before scan activity. It restores default Wi-Fi
 *       priority and does not update channel overlay channel information.
 * @note Configure channel overlay detection or adjusted TX power before Wi-Fi
 *       connects. If either feature is enabled after Wi-Fi is already connected,
 *       call this API again with the current operating channel.
 */
void coexm_bt_update_wifi_channel(struct mac_chan_op const *chan);

/**
 * @brief Check whether BT is routed through the BT path.
 *
 * @return true if BT uses the BT path; false 2G path.
 */
bool coexm_bt_is_bt_path(void);

#ifdef __cplusplus
}
#endif

/// @}

#endif /* __MACSW_COEXM_PUBLIC_H__ */
