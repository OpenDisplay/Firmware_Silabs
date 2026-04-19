#include "opendisplay_display_color.h"

/*
 * Byte counts follow Firmware handleDirectWriteStart (display_service.cpp) and
 * web encodeCanvasToByteData (ble-common.js). color_scheme 6 (16 gray) uses 4bpp
 * nibbles in the web stream; Firmware getBitsPerPixel() returns 1 for scheme 6 but
 * direct-write length must match the packed nibble stream.
 */

uint32_t opendisplay_color_bitplane_plane_bytes(uint32_t w, uint32_t h)
{
  uint64_t pixels = (uint64_t)w * (uint64_t)h;
  return (uint32_t)((pixels + 7u) / 8u);
}

int opendisplay_color_bits_per_pixel(uint8_t color_scheme)
{
  if (color_scheme == 4u) {
    return 4;
  }
  if (color_scheme == 3u || color_scheme == 5u) {
    return 2;
  }
  if (color_scheme == 6u) {
    return 4;
  }
  return 1;
}

bool opendisplay_color_is_bitplanes(uint8_t color_scheme)
{
  return (color_scheme == 1u || color_scheme == 2u);
}

int opendisplay_color_start_plane(uint8_t color_scheme)
{
  if (color_scheme == 0u || color_scheme == 6u) {
    return 0;
  }
  if (color_scheme == 1u || color_scheme == 2u) {
    return 0;
  }
  if (color_scheme == 5u) {
    return 1;
  }
  return 1;
}

uint32_t opendisplay_color_direct_write_total_bytes(uint32_t w, uint32_t h, uint8_t color_scheme)
{
  uint64_t pixels = (uint64_t)w * (uint64_t)h;
  if (opendisplay_color_is_bitplanes(color_scheme)) {
    uint32_t one = (uint32_t)((pixels + 7u) / 8u);
    return one * 2u;
  }
  int bpp = opendisplay_color_bits_per_pixel(color_scheme);
  if (bpp == 4) {
    return (uint32_t)((pixels + 1u) / 2u);
  }
  if (bpp == 2) {
    return (uint32_t)((pixels + 3u) / 4u);
  }
  return (uint32_t)((pixels + 7u) / 8u);
}
