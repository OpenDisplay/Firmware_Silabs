#ifndef OPENDISPLAY_DISPLAY_H
#define OPENDISPLAY_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int opendisplay_display_direct_write_start(const uint8_t *payload, uint16_t payload_len);
int opendisplay_display_direct_write_data(const uint8_t *payload, uint16_t payload_len);
int opendisplay_display_direct_write_end(const uint8_t *payload, uint16_t payload_len, bool *refresh_ok);
void opendisplay_display_abort(void);
void opendisplay_display_boot_apply(void);

#ifdef __cplusplus
}
#endif

#endif
