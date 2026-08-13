#ifndef BL616CL_ESP32_COMPAT_ESP_PARTITION_H_
#define BL616CL_ESP32_COMPAT_ESP_PARTITION_H_

#include <stdint.h>
#include <stddef.h>

typedef uint32_t spi_flash_mmap_handle_t;

enum {
    ESP_PARTITION_TYPE_DATA = 1,
    ESP_PARTITION_SUBTYPE_ANY = 0xFF,
    SPI_FLASH_MMAP_DATA = 0
};

typedef struct {
    uint32_t type;
    uint32_t subtype;
    uint32_t address;
    uint32_t size;
    char label[17];
} esp_partition_t;

#ifdef __cplusplus
extern "C" {
#endif

const esp_partition_t *esp_partition_find_first(uint32_t type, uint32_t subtype,
                                               const char *label);
int esp_partition_mmap(const esp_partition_t *partition, uint32_t offset,
                       uint32_t size, uint32_t memory,
                       const void **out_ptr, spi_flash_mmap_handle_t *out_handle);

#ifdef __cplusplus
}
#endif

#endif
