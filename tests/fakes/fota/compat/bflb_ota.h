#include <stdint.h>
int bflb_ota_start(uint32_t);
int bflb_ota_update(uint32_t, uint32_t, uint8_t *, uint32_t);
int bflb_ota_finish(uint8_t);
void bflb_ota_abort(void);
