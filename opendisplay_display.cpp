#include "opendisplay_display.h"
#include "opendisplay_display_color.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_epd_map.h"
#include "opendisplay_structs.h"
#include "bb_epaper.h"
#include "em_gpio.h"
#include "em_system.h"
#include "qr/qrcode.h"
#include "sl_sleeptimer.h"
#include <stdio.h>
#include <string.h>

static BBEPAPER s_epd;
static bool s_active;
static uint32_t s_total_bytes;
static uint32_t s_written_bytes;
static uint32_t s_dw_chunk_n;
static uint8_t s_dw_log_pct;
static uint8_t s_dw_trailing_ignores;
static uint32_t s_dw_init_t0;
static uint8_t s_color_scheme;
static uint32_t s_plane_size;
static bool s_plane2_started;
static bool s_boot_applied;

#ifndef OD_FALLBACK_DISPLAY_PWR_PIN
#define OD_FALLBACK_DISPLAY_PWR_PIN 0x00u
#endif

static void dw_init_mark(const char *tag)
{
  uint32_t now = sl_sleeptimer_get_tick_count();
  printf("[OD] dw init %-26s %lu ms\r\n", tag, (unsigned long)sl_sleeptimer_tick_to_ms(now - s_dw_init_t0));
}

static const struct DisplayConfig *display_cfg(void)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  if (cfg == nullptr || cfg->display_count == 0u) {
    return nullptr;
  }
  return &cfg->displays[0];
}

static bool decode_pin(uint8_t v, GPIO_Port_TypeDef *port_out, uint8_t *pin_out)
{
  unsigned pr;
  unsigned pn;
  if (v == 0xFFu) {
    return false;
  }
  pr = (unsigned)(v >> 4) & 0x0Fu;
  pn = (unsigned)(v & 0x0Fu);
  if (pr > (unsigned)GPIO_PORT_MAX || pn > 15u) {
    return false;
  }
  *port_out = (GPIO_Port_TypeDef)(gpioPortA + pr);
  *pin_out = (uint8_t)pn;
  return true;
}

static void display_power_set(bool on)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  uint8_t p;
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (cfg == nullptr) {
    return;
  }
  p = cfg->system_config.pwr_pin;
  if (p == 0xFFu) {
    p = OD_FALLBACK_DISPLAY_PWR_PIN;
  }
  if (!decode_pin(p, &port, &pin)) {
    return;
  }
  GPIO_PinModeSet(port, pin, gpioModePushPull, on ? 1u : 0u);
}

typedef struct {
  char c;
  uint8_t col[5];
} Glyph5x7;

static const Glyph5x7 s_font5x7[] = {
  { ' ', { 0, 0, 0, 0, 0 } },       { '.', { 0, 0, 0x40, 0, 0 } }, { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
  { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } }, { '2', { 0x62, 0x51, 0x49, 0x49, 0x46 } },
  { '3', { 0x22, 0x49, 0x49, 0x49, 0x36 } }, { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
  { '5', { 0x2F, 0x49, 0x49, 0x49, 0x31 } }, { '6', { 0x3E, 0x49, 0x49, 0x49, 0x32 } },
  { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } }, { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
  { '9', { 0x26, 0x49, 0x49, 0x49, 0x3E } }, { 'A', { 0x7E, 0x11, 0x11, 0x11, 0x7E } },
  { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } }, { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
  { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } }, { 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
  { 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } }, { 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } },
  { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } }, { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
  { 'N', { 0x7F, 0x02, 0x0C, 0x10, 0x7F } }, { 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } },
  { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } }, { 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
  { 'S', { 0x26, 0x49, 0x49, 0x49, 0x32 } }, { 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
  { 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
};

static const uint8_t *glyph5x7(char c)
{
  unsigned i;
  for (i = 0; i < (sizeof(s_font5x7) / sizeof(s_font5x7[0])); i++) {
    if (s_font5x7[i].c == c) {
      return s_font5x7[i].col;
    }
  }
  return s_font5x7[0].col;
}

static void set_pixel_row(uint8_t *row, int x, bool is4clr)
{
  if (is4clr) {
    row[x >> 2] &= (uint8_t)~(0xC0u >> ((unsigned)(x & 3) * 2u));
  } else {
    row[x >> 3] &= (uint8_t)~(0x80u >> ((unsigned)x & 7u));
  }
}

static void draw_text_row(uint8_t *row, int y, int x0, int y0, const char *s, int scale, bool is4clr)
{
  int cursor = x0;
  const char *p;
  if (s == nullptr || scale <= 0) {
    return;
  }
  for (p = s; *p != '\0'; p++) {
    const uint8_t *g = glyph5x7(*p);
    int col, gy, sx;
    for (col = 0; col < 5; col++) {
      for (gy = 0; gy < 7; gy++) {
        int py, px;
        if (((g[col] >> gy) & 1u) == 0u) {
          continue;
        }
        py = y0 + gy * scale;
        if (y < py || y >= py + scale) {
          continue;
        }
        px = cursor + col * scale;
        for (sx = 0; sx < scale; sx++) {
          int rx = px + sx;
          if (rx >= 0) {
            set_pixel_row(row, rx, is4clr);
          }
        }
      }
    }
    cursor += 6 * scale;
  }
}

static uint16_t text_width_px(const char *s, int scale)
{
  if (s == nullptr || scale <= 0) {
    return 0u;
  }
  return (uint16_t)(strlen(s) * (size_t)(6 * scale));
}

static void bytes_to_hex(const uint8_t *in, uint16_t len, char *out, uint16_t out_size)
{
  static const char *H = "0123456789ABCDEF";
  uint16_t i;
  if (out == nullptr || out_size < (uint16_t)(len * 2u + 1u)) {
    return;
  }
  for (i = 0; i < len; i++) {
    out[i * 2u + 0u] = H[(in[i] >> 4) & 0x0Fu];
    out[i * 2u + 1u] = H[in[i] & 0x0Fu];
  }
  out[len * 2u] = '\0';
}

static uint16_t base64url_encode(const uint8_t *data, uint16_t len, char *out, uint16_t out_size)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  uint16_t out_len = 0;
  uint16_t i = 0;
  while (i + 3u <= len) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1u] << 8) | data[i + 2u];
    i += 3u;
    if (out_len + 4u >= out_size) {
      return 0u;
    }
    out[out_len++] = tbl[(v >> 18) & 63u];
    out[out_len++] = tbl[(v >> 12) & 63u];
    out[out_len++] = tbl[(v >> 6) & 63u];
    out[out_len++] = tbl[v & 63u];
  }
  if ((len - i) == 1u) {
    uint32_t v = ((uint32_t)data[i] << 16);
    if (out_len + 2u >= out_size) {
      return 0u;
    }
    out[out_len++] = tbl[(v >> 18) & 63u];
    out[out_len++] = tbl[(v >> 12) & 63u];
  } else if ((len - i) == 2u) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1u] << 8);
    if (out_len + 3u >= out_size) {
      return 0u;
    }
    out[out_len++] = tbl[(v >> 18) & 63u];
    out[out_len++] = tbl[(v >> 12) & 63u];
    out[out_len++] = tbl[(v >> 6) & 63u];
  }
  if (out_len >= out_size) {
    return 0u;
  }
  out[out_len] = '\0';
  return out_len;
}

static bool render_boot_screen(BBEPAPER &epd, const struct GlobalConfig *cfg)
{
  static const char *LANDING_URL_PREFIX = "https://opendisplay.org/l/?";
  const struct SecurityConfig *sec = od_get_parsed_security();
  const struct DisplayConfig *dc;
  uint16_t w, h, tag_type, mfg;
  uint64_t uid;
  uint32_t last3;
  uint8_t payload[23];
  uint8_t key[16];
  char payload_b64[64];
  char url[128];
  QRCode qr;
  static uint8_t qr_buf[256];
  uint16_t qr_buf_size;
  uint8_t qr_size;
  uint16_t qr_modules;
  int scale_text;
  int pad;
  int module_px;
  int qr_px;
  bool qr_right;
  int qr_x, qr_y, avail_w, text_y;
  char name_line[16];
  const char *domain_line = "OPENDISPLAY.ORG";
  char key_hex[33];
  char k1[17], k2[17];
  uint16_t dW, nW, k1W, k2W;

  if (cfg == nullptr || cfg->display_count == 0u) {
    return false;
  }
  dc = &cfg->displays[0];
  w = (uint16_t)epd.width();
  h = (uint16_t)epd.height();

  memset(payload, 0, sizeof(payload));
  tag_type = dc->tag_type;
  payload[0] = (uint8_t)((tag_type >> 8) & 0xFFu);
  payload[1] = (uint8_t)(tag_type & 0xFFu);
  uid = SYSTEM_GetUnique();
  last3 = (uint32_t)(uid & 0xFFFFFFu);
  payload[2] = (uint8_t)((last3 >> 16) & 0xFFu);
  payload[3] = (uint8_t)((last3 >> 8) & 0xFFu);
  payload[4] = (uint8_t)(last3 & 0xFFu);
  memset(key, 0, sizeof(key));
  if (sec != nullptr && (sec->flags & SECURITY_FLAG_SHOW_KEY_ON_SCREEN) != 0u) {
    memcpy(key, sec->encryption_key, sizeof(key));
  }
  memcpy(&payload[5], key, sizeof(key));
  mfg = cfg->manufacturer_data.manufacturer_id;
  payload[21] = (uint8_t)((mfg >> 8) & 0xFFu);
  payload[22] = (uint8_t)(mfg & 0xFFu);

  if (base64url_encode(payload, sizeof(payload), payload_b64, sizeof(payload_b64)) == 0u) {
    return false;
  }
  (void)snprintf(url, sizeof(url), "%s%s", LANDING_URL_PREFIX, payload_b64);

  qr_buf_size = qrcode_getBufferSize(6u);
  if (qr_buf_size == 0u || qr_buf_size > sizeof(qr_buf)) {
    return false;
  }
  if (qrcode_initText(&qr, qr_buf, 6u, ECC_MEDIUM, url) != 0) {
    return false;
  }
  qr_size = qr.size;
  qr_modules = (uint16_t)(qr_size + 8u);

  scale_text = (w >= 400u && h >= 300u) ? 2 : 1;
  pad = 6 * scale_text;
  module_px = ((int)((w < h) ? w : h) - pad * 2) / (int)qr_modules;
  if (module_px < 1) {
    module_px = 1;
  }
  if (module_px > 6) {
    module_px = 6;
  }
  qr_px = module_px * (int)qr_modules;
  qr_right = (w >= (uint16_t)(qr_px + 160));
  qr_x = qr_right ? ((int)w - pad - qr_px) : (((int)w - qr_px) / 2);
  qr_y = qr_right ? pad : ((int)h - pad - qr_px);
  avail_w = qr_right ? qr_x : (int)w;
  text_y = pad;
  if (qr_right) {
    int block_h = 4 * (scale_text * 10) + 7 * scale_text;
    if (qr_px > block_h) {
      text_y = qr_y + (qr_px - block_h) / 2;
    } else {
      text_y = qr_y;
    }
  }

  (void)snprintf(name_line, sizeof(name_line), "OD%06lX", (unsigned long)last3);
  bytes_to_hex(key, sizeof(key), key_hex, sizeof(key_hex));
  memcpy(k1, key_hex, 16);
  k1[16] = '\0';
  memcpy(k2, key_hex + 16, 16);
  k2[16] = '\0';

  {
    static uint8_t s_boot_row[256];
    bool is4clr = (epd._bbep.iFlags & BBEP_4COLOR) != 0;
    bool is3clr = (epd._bbep.iFlags & BBEP_3COLOR) != 0;
    int pitch = is4clr ? ((int)w + 3) / 4 : ((int)w + 7) / 8;
    uint8_t white_byte = is4clr ? 0x55u : 0xFFu;
    int domX, nameX, k1X, k2X, y;

    dW  = text_width_px(domain_line, scale_text);
    nW  = text_width_px(name_line, scale_text);
    k1W = text_width_px(k1, scale_text);
    k2W = text_width_px(k2, scale_text);
    domX  = (dW  < (uint16_t)avail_w) ? ((avail_w - (int)dW)  / 2) : pad;
    nameX = (nW  < (uint16_t)avail_w) ? ((avail_w - (int)nW)  / 2) : pad;
    k1X   = (k1W < (uint16_t)avail_w) ? ((avail_w - (int)k1W) / 2) : pad;
    k2X   = (k2W < (uint16_t)avail_w) ? ((avail_w - (int)k2W) / 2) : pad;

    epd.setAddrWindow(0, 0, (int)w, (int)h);
    epd.startWrite(is4clr ? PLANE_1 : PLANE_0);

    for (y = 0; y < (int)h; y++) {
      memset(s_boot_row, white_byte, (size_t)pitch);

      draw_text_row(s_boot_row, y, domX,  text_y,                domain_line, scale_text, is4clr);
      draw_text_row(s_boot_row, y, nameX, text_y + scale_text * 10, name_line,  scale_text, is4clr);
      draw_text_row(s_boot_row, y, k1X,   text_y + scale_text * 30, k1, scale_text, is4clr);
      draw_text_row(s_boot_row, y, k2X,   text_y + scale_text * 40, k2, scale_text, is4clr);

      if (y >= qr_y && y < qr_y + qr_px) {
        int local_y = y - qr_y;
        int my = local_y / module_px;
        if (my < (int)qr_modules) {
          int qy_qr = my - 4;
          int mx;
          for (mx = 0; mx < (int)qr_modules; mx++) {
            int qx_qr = mx - 4;
            bool on = false;
            if (qx_qr >= 0 && qy_qr >= 0 && qx_qr < (int)qr_size && qy_qr < (int)qr_size) {
              on = qrcode_getModule(&qr, (uint8_t)qx_qr, (uint8_t)qy_qr);
            }
            if (!on) {
              continue;
            }
            {
              int px0 = qr_x + mx * module_px;
              int sx;
              for (sx = 0; sx < module_px; sx++) {
                int px = px0 + sx;
                if (px >= 0 && px < (int)w) {
                  set_pixel_row(s_boot_row, px, is4clr);
                }
              }
            }
          }
        }
      }

      epd.writeData(s_boot_row, pitch);
    }
    if (is3clr) {
      epd.startWrite(PLANE_1);
      for (y = 0; y < (int)h; y++) {
        memset(s_boot_row, 0x00u, (size_t)pitch);
        epd.writeData(s_boot_row, pitch);
      }
    }
  }
  return true;
}

static bool wait_for_refresh(uint32_t timeout_ms)
{
  uint32_t elapsed = 0;
  bool saw_busy = false;
  while (elapsed < timeout_ms) {
    bool busy = s_epd.isBusy();
    if (busy) {
      saw_busy = true;
    } else if (saw_busy) {
      return true;
    }
    sl_sleeptimer_delay_millisecond(50);
    elapsed += 50;
  }
  return saw_busy && !s_epd.isBusy();
}

extern "C" void opendisplay_display_abort(void)
{
  if (s_active) {
    s_epd.sleep(DEEP_SLEEP);
  }
  display_power_set(false);
  s_active = false;
  s_total_bytes = 0;
  s_written_bytes = 0;
  s_dw_chunk_n = 0;
  s_dw_log_pct = 0;
  s_dw_trailing_ignores = 0;
  s_plane_size = 0;
  s_plane2_started = false;
}

extern "C" void opendisplay_display_boot_apply(void)
{
  const struct DisplayConfig *d = display_cfg();
  int panel;
  if (s_boot_applied || d == nullptr) {
    return;
  }
  s_boot_applied = true;
  panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    return;
  }
  s_epd = BBEPAPER();
  display_power_set(true);
  if (s_epd.setPanelType(panel) != BBEP_SUCCESS) {
    display_power_set(false);
    return;
  }
  s_epd.setRotation((int)d->rotation * 90);
  s_epd.initIO(d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin, 0);
  s_epd.wake();
  s_epd.sendPanelInitFull();
  if (!render_boot_screen(s_epd, opendisplay_get_global_config())) {
    s_epd.fillScreen(BBEP_WHITE);
  }
  (void)s_epd.refresh(REFRESH_FULL, true);
  s_epd.sleep(DEEP_SLEEP);
  display_power_set(false);
}

extern "C" int opendisplay_display_direct_write_start(const uint8_t *payload, uint16_t payload_len)
{
  (void)payload;
  s_dw_init_t0 = sl_sleeptimer_get_tick_count();
  printf("[OD] dw init begin\r\n");
  if (payload_len != 0u) {
    printf("[OD] dw start note non-empty payload len=%u (ignored)\r\n", (unsigned)payload_len);
  }

  const struct DisplayConfig *d = display_cfg();
  if (d == nullptr) {
    printf("[OD] dw start err no display cfg\r\n");
    return -1;
  }
  dw_init_mark("after cfg");

  int panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    printf("[OD] dw start err bad panel_ic_type=%u\r\n", (unsigned)d->panel_ic_type);
    return -2;
  }

  opendisplay_display_abort();
  dw_init_mark("after abort");
  display_power_set(true);
  s_epd = BBEPAPER();
  if (s_epd.setPanelType(panel) != BBEP_SUCCESS) {
    printf("[OD] dw start err setPanelType panel=%d\r\n", panel);
    display_power_set(false);
    return -3;
  }
  dw_init_mark("after setPanelType");

  s_epd.setRotation((int)d->rotation * 90);
  s_epd.initIO(d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin, 0);
  dw_init_mark("after initIO");
  s_epd.wake();
  dw_init_mark("after wake (reset + busy)");
  s_epd.sendPanelInitFull();
  dw_init_mark("after pInitFull");
  s_epd.setAddrWindow(0, 0, d->pixel_width, d->pixel_height);
  dw_init_mark("after setAddrWindow");

  s_color_scheme = d->color_scheme;
  s_plane_size = 0;
  s_plane2_started = false;
  if (opendisplay_color_is_bitplanes(s_color_scheme)) {
    s_plane_size = opendisplay_color_bitplane_plane_bytes(d->pixel_width, d->pixel_height);
  }
  s_total_bytes =
    opendisplay_color_direct_write_total_bytes(d->pixel_width, d->pixel_height, s_color_scheme);
  {
    int sp = opendisplay_color_start_plane(s_color_scheme);
    s_epd.startWrite(sp == 0 ? PLANE_0 : PLANE_1);
  }
  dw_init_mark("after startWrite");

  s_written_bytes = 0;
  s_dw_chunk_n = 0;
  s_dw_log_pct = 0;
  s_dw_trailing_ignores = 0;
  s_active = true;
  printf("[OD] dw start total=%lu B bpp=%d cs=%u panel=%u %ux%u bitplanes=%d\r\n",
         (unsigned long)s_total_bytes, opendisplay_color_bits_per_pixel(s_color_scheme),
         (unsigned)s_color_scheme, (unsigned)d->panel_ic_type, (unsigned)d->pixel_width,
         (unsigned)d->pixel_height, (int)opendisplay_color_is_bitplanes(s_color_scheme));
  return 0;
}

extern "C" int opendisplay_display_direct_write_data(const uint8_t *payload, uint16_t payload_len)
{
  if (!s_active || payload == nullptr || payload_len == 0u) {
    printf("[OD] dw data bad arg active=%d len=%u\r\n", (int)s_active, (unsigned)payload_len);
    return -1;
  }

  uint32_t remaining = (s_written_bytes < s_total_bytes) ? (s_total_bytes - s_written_bytes) : 0u;
  if (remaining == 0u) {
    if (payload_len > 0u) {
      if (s_dw_trailing_ignores < 4u) {
        printf("[OD] dw data ignore trailing chunk #%u len=%u (have %lu/%lu B)\r\n",
               (unsigned)s_dw_trailing_ignores + 1u, (unsigned)payload_len,
               (unsigned long)s_written_bytes, (unsigned long)s_total_bytes);
        s_dw_trailing_ignores++;
      }
    }
    return 0;
  }

  const bool bitplanes = opendisplay_color_is_bitplanes(s_color_scheme);
  const uint8_t *p = payload;
  uint16_t left = payload_len;
  const uint32_t written_before = s_written_bytes;

  while (left > 0u && remaining > 0u) {
    uint32_t rem = (uint32_t)remaining;
    uint16_t chunk = left;
    if (bitplanes && !s_plane2_started && s_plane_size > 0u) {
      uint32_t to_plane_end = s_plane_size - s_written_bytes;
      if (chunk > to_plane_end) {
        chunk = (uint16_t)to_plane_end;
      }
    }
    if ((uint32_t)chunk > rem) {
      chunk = (uint16_t)rem;
    }
    if (chunk == 0u) {
      break;
    }
    s_epd.writeData((uint8_t *)(void *)p, (int)chunk);
    p += chunk;
    left -= chunk;
    s_written_bytes += chunk;
    remaining -= (uint32_t)chunk;
    if (bitplanes && !s_plane2_started && s_plane_size > 0u && s_written_bytes >= s_plane_size) {
      s_epd.startWrite(PLANE_1);
      s_plane2_started = true;
    }
  }

  if (s_written_bytes > written_before) {
    s_dw_chunk_n++;
    if (s_total_bytes > 0u) {
      uint8_t pct = (uint8_t)((100u * s_written_bytes) / s_total_bytes);
      if (pct >= s_dw_log_pct + 25u) {
        printf("[OD] dw data #%lu %lu/%lu B (%u%%)\r\n", (unsigned long)s_dw_chunk_n,
               (unsigned long)s_written_bytes, (unsigned long)s_total_bytes, (unsigned)pct);
        s_dw_log_pct = (pct / 25u) * 25u;
      }
    }
  }
  return 0;
}

extern "C" int opendisplay_display_direct_write_end(const uint8_t *payload, uint16_t payload_len, bool *refresh_ok)
{
  if (!s_active) {
    printf("[OD] dw end err inactive\r\n");
    return -1;
  }
  if (s_written_bytes < s_total_bytes) {
    printf("[OD] dw end err incomplete wr=%lu need=%lu\r\n", (unsigned long)s_written_bytes,
         (unsigned long)s_total_bytes);
    return -2;
  }
  if (refresh_ok != nullptr) {
    *refresh_ok = false;
  }

  int refresh_mode = REFRESH_FULL;
  if (payload != nullptr && payload_len >= 1u && payload[0] == 1u) {
    refresh_mode = REFRESH_FAST;
  }

  printf("[OD] dw refresh start mode=%d\r\n", refresh_mode);
  (void)s_epd.refresh(refresh_mode, false);
  bool ok = wait_for_refresh(60000u);
  printf("[OD] dw refresh done ok=%d busy=%d\r\n", (int)ok, (int)s_epd.isBusy());
  s_epd.sleep(DEEP_SLEEP);
  s_active = false;
  display_power_set(false);

  if (refresh_ok != nullptr) {
    *refresh_ok = ok;
  }
  return 0;
}
