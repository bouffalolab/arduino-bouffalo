/*
 * esp_ping_* implementation over lwIP raw ICMP sockets.
 *
 * The ESP-IDF API runs the session in a background task and reports through
 * the callbacks; the bridge's ping.cpp depends on that shape (esp_ping_start()
 * returns immediately and ping_end() flips its completion status).  Keep the
 * same contract here: the echo loop runs in a FreeRTOS task and
 * esp_ping_stop()/esp_ping_delete_session() are safe to call from the
 * callbacks, i.e. from that task.
 */

#include "ping/ping_sock.h"

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>

extern "C" {
#include "lwip/sockets.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
}

#include <string.h>

#define ESP_PING_ICMP_ID            0xAFAF
#define ESP_PING_DATA_SIZE          32
#define ESP_PING_TASK_STACK_WORDS   2048
#define ESP_PING_TASK_PRIORITY      1
#define ESP_PING_FALLBACK_TIMEOUT   1000

struct esp_ping_handle_t_ {
    esp_ping_config_t config;
    esp_ping_callbacks_t callbacks;
    TaskHandle_t task;
    volatile bool stop;
    volatile bool deleted;
    uint32_t last_timegap_ms;
};

static int esp_ping_open_socket(uint32_t timeout_ms, uint8_t ttl)
{
    int sock = lwip_socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (sock < 0) {
        return -1;
    }

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int ttl_value = ttl;
    lwip_setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl_value, sizeof(ttl_value));

    return sock;
}

static void esp_ping_task(void *arg)
{
    esp_ping_handle_t handle = (esp_ping_handle_t)arg;
    uint8_t request[sizeof(struct icmp_echo_hdr) + ESP_PING_DATA_SIZE];
    struct icmp_echo_hdr *echo = (struct icmp_echo_hdr *)request;
    struct sockaddr_in target;

    uint32_t count = handle->config.count ? handle->config.count : 1;
    uint32_t interval_ms = handle->config.interval_ms;
    uint32_t timeout_ms =
        handle->config.timeout_ms ? handle->config.timeout_ms : ESP_PING_FALLBACK_TIMEOUT;
    uint16_t seqno = 0;

    memset(&target, 0, sizeof(target));
    target.sin_len = sizeof(target);
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&handle->config.target_addr));

    /* Let the caller finish esp_ping_start() -> status=RUNNING before the
     * first result can be reported; otherwise a fast session could complete
     * first and the caller would wait forever for its end callback. */
    vTaskDelay(pdMS_TO_TICKS(5));

    int sock = esp_ping_open_socket(timeout_ms,
                                    handle->config.ttl ? handle->config.ttl : 64);
    if (sock < 0) {
        if (handle->callbacks.on_ping_timeout) {
            handle->callbacks.on_ping_timeout(handle, handle->callbacks.cb_args);
        }
    } else {
        for (uint32_t i = 0; i < count && !handle->stop && !handle->deleted; i++) {
            if (i > 0 && interval_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(interval_ms));
                if (handle->stop || handle->deleted) {
                    break;
                }
            }

            memset(request, 0, sizeof(request));
            ICMPH_TYPE_SET(echo, ICMP_ECHO);
            ICMPH_CODE_SET(echo, 0);
            echo->chksum = 0;
            echo->id = lwip_htons(ESP_PING_ICMP_ID);
            echo->seqno = lwip_htons(++seqno);
            for (size_t d = 0; d < ESP_PING_DATA_SIZE; d++) {
                request[sizeof(struct icmp_echo_hdr) + d] = (uint8_t)d;
            }
            echo->chksum = inet_chksum(request, sizeof(request));

            unsigned long start = millis();
            int sent = (int)lwip_sendto(sock, request, sizeof(request), 0,
                                        (struct sockaddr *)&target, sizeof(target));
            bool success = false;

            if (sent == (int)sizeof(request)) {
                uint8_t reply[64];
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                int len = (int)lwip_recvfrom(sock, reply, sizeof(reply), 0,
                                             (struct sockaddr *)&from, &from_len);
                if (len >= (int)(sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr)) &&
                    from.sin_addr.s_addr == target.sin_addr.s_addr) {
                    struct ip_hdr *iphdr = (struct ip_hdr *)reply;
                    struct icmp_echo_hdr *iecho =
                        (struct icmp_echo_hdr *)(reply + (IPH_HL(iphdr) * 4));
                    if (ICMPH_TYPE(iecho) == ICMP_ER &&
                        iecho->id == lwip_htons(ESP_PING_ICMP_ID) &&
                        iecho->seqno == lwip_htons(seqno)) {
                        success = true;
                    }
                }
            }

            if (success) {
                handle->last_timegap_ms = (uint32_t)(millis() - start);
                if (handle->callbacks.on_ping_success) {
                    handle->callbacks.on_ping_success(handle, handle->callbacks.cb_args);
                }
            } else if (handle->callbacks.on_ping_timeout) {
                handle->callbacks.on_ping_timeout(handle, handle->callbacks.cb_args);
            }
        }
        lwip_close(sock);
    }

    if (handle->callbacks.on_ping_end) {
        handle->callbacks.on_ping_end(handle, handle->callbacks.cb_args);
    }

    /* esp_ping_delete_session() may be called from on_ping_end (the usual
     * bridge flow) or from another task; the task owns the memory once the
     * session is marked deleted. */
    if (handle->deleted) {
        free(handle);
    }
    vTaskDelete(NULL);
}

extern "C" int esp_ping_new_session(const esp_ping_config_t *config,
                                    const esp_ping_callbacks_t *callbacks,
                                    esp_ping_handle_t *out_handle)
{
    if (config == nullptr || out_handle == nullptr) {
        return ESP_FAIL;
    }

#if LWIP_IPV6
    /* The session socket is AF_INET; reject IPv6 targets instead of
     * misreading the address. */
    if (IP_IS_V6(&config->target_addr)) {
        return ESP_FAIL;
    }
#endif

    esp_ping_handle_t handle =
        (esp_ping_handle_t)calloc(1, sizeof(struct esp_ping_handle_t_));
    if (handle == nullptr) {
        return ESP_FAIL;
    }

    handle->config = *config;
    if (callbacks != nullptr) {
        handle->callbacks = *callbacks;
    }
    *out_handle = handle;
    return ESP_OK;
}

extern "C" int esp_ping_start(esp_ping_handle_t handle)
{
    if (handle == nullptr) {
        return ESP_FAIL;
    }
    BaseType_t created = xTaskCreate(esp_ping_task, "esp_ping",
                                     ESP_PING_TASK_STACK_WORDS, handle,
                                     ESP_PING_TASK_PRIORITY, &handle->task);
    return created == pdPASS ? ESP_OK : ESP_FAIL;
}

extern "C" int esp_ping_stop(esp_ping_handle_t handle)
{
    if (handle == nullptr) {
        return ESP_FAIL;
    }
    handle->stop = true;
    return ESP_OK;
}

extern "C" void esp_ping_delete_session(esp_ping_handle_t handle)
{
    if (handle == nullptr) {
        return;
    }
    handle->deleted = true;
    handle->stop = true;
    /* When called from the ping task itself (on_ping_end), the task frees
     * the handle after the callback returns. */
}

extern "C" int esp_ping_get_profile(esp_ping_handle_t handle, int profile,
                                    void *data, uint32_t size)
{
    if (handle == nullptr || data == nullptr || profile != ESP_PING_PROF_TIMEGAP ||
        size < sizeof(uint32_t)) {
        return ESP_FAIL;
    }
    *(uint32_t *)data = handle->last_timegap_ms;
    return ESP_OK;
}
