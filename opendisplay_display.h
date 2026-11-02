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
void opendisplay_display_park_pins(void);
void opendisplay_display_power_off(void);

/* Raw SPI debug session (CMD_SPI_RAW 0x0084).
 *
 * The session deliberately PERSISTS between calls: a raw interface that
 * re-initialises per command cannot be used to stage registers, because the
 * reset discards whatever the previous call wrote.
 *
 * All return 0 on success, or a negative OD_ERR_SPI_RAW_* value negated.
 * `open` with want_panel_init=false brings up power + SPI/GPIO only and sends
 * nothing to the panel. */
int  opendisplay_display_spi_raw_open(bool want_panel_init);
int  opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len);
int  opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len);
int  opendisplay_display_spi_raw_busy(uint32_t timeout_ms,
                                      uint16_t *elapsed_ms_out,
                                      bool *still_busy_out);
int  opendisplay_display_spi_raw_reset(uint8_t hold_ms);
void opendisplay_display_spi_raw_close(void);
bool opendisplay_display_spi_raw_is_open(void);

#ifdef __cplusplus
}
#endif

#endif
