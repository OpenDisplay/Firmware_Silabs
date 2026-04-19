#ifndef OPENDISPLAY_DISPLAY_COLOR_H
#define OPENDISPLAY_DISPLAY_COLOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors Firmware/src/display_service.cpp: getBitsPerPixel, getplane, handleDirectWriteStart
 * and web/js/ble-common.js encodeCanvasToByteData buffer sizes. */

int opendisplay_color_bits_per_pixel(uint8_t color_scheme);
bool opendisplay_color_is_bitplanes(uint8_t color_scheme);
int opendisplay_color_start_plane(uint8_t color_scheme);
uint32_t opendisplay_color_bitplane_plane_bytes(uint32_t pixel_width, uint32_t pixel_height);
uint32_t opendisplay_color_direct_write_total_bytes(uint32_t pixel_width, uint32_t pixel_height,
                                                    uint8_t color_scheme);

#ifdef __cplusplus
}
#endif

#endif
