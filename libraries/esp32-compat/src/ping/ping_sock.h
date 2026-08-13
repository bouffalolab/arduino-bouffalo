#ifndef BL616CL_ESP32_COMPAT_PING_SOCK_H_
#define BL616CL_ESP32_COMPAT_PING_SOCK_H_

#include <stdint.h>
#include "../lwip/ip_addr.h"

#define ESP_OK 0
#define ESP_FAIL -1

typedef struct esp_ping_handle_t_ *esp_ping_handle_t;

enum {
    ESP_PING_PROF_TIMEGAP = 0
};

typedef struct {
    ip_addr_t target_addr;
    uint8_t ttl;
    uint32_t count;
    uint32_t interval_ms;
} esp_ping_config_t;

typedef struct {
    void (*on_ping_success)(esp_ping_handle_t hdl, void *args);
    void (*on_ping_timeout)(esp_ping_handle_t hdl, void *args);
    void (*on_ping_end)(esp_ping_handle_t hdl, void *args);
    void *cb_args;
} esp_ping_callbacks_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline esp_ping_config_t ESP_PING_DEFAULT_CONFIG(void)
{
    esp_ping_config_t cfg = {};
    cfg.ttl = 64;
    cfg.count = 1;
    cfg.interval_ms = 1000;
    return cfg;
}

int esp_ping_new_session(const esp_ping_config_t *config,
                         const esp_ping_callbacks_t *callbacks,
                         esp_ping_handle_t *out_handle);
int esp_ping_start(esp_ping_handle_t handle);
int esp_ping_stop(esp_ping_handle_t handle);
void esp_ping_delete_session(esp_ping_handle_t handle);
int esp_ping_get_profile(esp_ping_handle_t handle, int profile,
                         void *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
