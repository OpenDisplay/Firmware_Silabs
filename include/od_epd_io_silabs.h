/* od_epd_io_silabs -- EFR32BG22 half of od_epd_io_t. See the .c for what was
 * dropped from bb_epaper's silabs_efr32_io.inl and why. */
#ifndef OD_EPD_IO_SILABS_H
#define OD_EPD_IO_SILABS_H

#include <stdbool.h>
#include <stdint.h>

#include "od_epd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Claim the pins. `speed` 0 means bit-bang; non-zero configures USART1 at that
 * bit rate with a bit-bang fallback. `busy_idle_high` sets the input pull so an
 * unconnected BUSY line reads idle rather than hanging a poll forever. */
void od_epd_io_silabs_setup(uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                            uint8_t mosi, uint8_t sck, uint32_t speed,
                            bool busy_idle_high);

/* The GPIO/SPI half of the vtable. seq_read/lut_info/lut_read are NULL here and
 * are supplied by whoever knows where the config lives. */
const od_epd_io_t *od_epd_io_silabs(void);

/* Drive one RAW (port<<4|pin encoded) GPIO for the SET_PIN sequence escape:
 * level 0/1 = push-pull low/high, 2 = release to input/Hi-Z (a bidirectional
 * data line during a 3-wire read-back). Returns false on an invalid encoding.
 * The pin_ref -> raw mapping lives in the adapter (it owns the config). */
bool od_epd_io_silabs_pin(uint8_t raw_pin, uint8_t level);

#ifdef __cplusplus
}
#endif
#endif /* OD_EPD_IO_SILABS_H */
