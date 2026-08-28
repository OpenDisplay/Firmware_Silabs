#include "opendisplay_apple_findmy.h"

#include "sl_bt_api.h"
#include "sl_sleeptimer.h"
#include <stdio.h>
#include <string.h>

static bool s_apple_adv_started;
static uint32_t s_apple_last_fail_log_ms;

static bool od_apple_bytes_nonzero(const uint8_t *p, size_t n)
{
  size_t i;

  for (i = 0u; i < n; i++) {
    if (p[i] != 0u) {
      return true;
    }
  }
  return false;
}

bool od_apple_config_active(const struct FindMyConfig *cfg)
{
  if (cfg == NULL) {
    return false;
  }
  if ((cfg->flags & OD_FINDMY_FLAG_APPLE_ENABLE) == 0u) {
    return false;
  }
  return od_apple_bytes_nonzero(cfg->apple_master_secret, OD_APPLE_ADV_KEY_LEN);
}

static bool od_apple_apply_address(uint8_t adv_set, const uint8_t key[OD_APPLE_ADV_KEY_LEN])
{
  bd_addr addr = { { 0 } };
  bd_addr applied = { { 0 } };
  sl_status_t sc;

  addr.addr[5] = (uint8_t)(key[0] | 0xC0u);
  addr.addr[4] = key[1];
  addr.addr[3] = key[2];
  addr.addr[2] = key[3];
  addr.addr[1] = key[4];
  addr.addr[0] = key[5];

  sc = sl_bt_advertiser_set_random_address(adv_set,
                                           sl_bt_gap_static_address,
                                           addr,
                                           &applied);
  if (sc != SL_STATUS_OK) {
    printf("[OD][Apple] set_random_address sc=0x%04lX\r\n", (unsigned long)sc);
    return false;
  }
  printf("[OD][Apple] addr %02X:%02X:%02X:%02X:%02X:%02X (OpenHaystack key)\r\n",
         applied.addr[5], applied.addr[4], applied.addr[3],
         applied.addr[2], applied.addr[1], applied.addr[0]);
  return true;
}

static size_t od_apple_build_legacy_adv(const uint8_t key[OD_APPLE_ADV_KEY_LEN],
                                        uint8_t *adv,
                                        size_t adv_cap)
{
  if (adv_cap < OD_APPLE_LEGACY_ADV_LEN) {
    return 0u;
  }

  adv[0] = 0x1Eu;
  adv[1] = 0xFFu;
  adv[2] = 0x4Cu;
  adv[3] = 0x00u;
  adv[4] = 0x12u;
  adv[5] = 0x19u;
  adv[6] = 0x00u;
  memcpy(&adv[7], &key[6], 22u);
  adv[29] = (uint8_t)(key[0] >> 6);
  adv[30] = 0x00u;
  return OD_APPLE_LEGACY_ADV_LEN;
}

void od_apple_stop(uint8_t adv_set)
{
  if (adv_set == 0xFFu) {
    return;
  }
  if (s_apple_adv_started) {
    (void)sl_bt_advertiser_stop(adv_set);
    s_apple_adv_started = false;
  }
}

static void od_apple_log_fail(const char *step, sl_status_t sc)
{
  uint32_t now_ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());

  if ((now_ms - s_apple_last_fail_log_ms) < 5000u) {
    return;
  }
  s_apple_last_fail_log_ms = now_ms;
  printf("[OD][Apple] %s sc=0x%04lX (retrying)\r\n", step, (unsigned long)sc);
}

void od_apple_sync(uint8_t adv_set, const struct FindMyConfig *cfg)
{
  uint8_t adv[OD_APPLE_LEGACY_ADV_LEN];
  size_t adv_len;
  sl_status_t sc;

  if (adv_set == 0xFFu) {
    return;
  }
  if (!od_apple_config_active(cfg)) {
    od_apple_stop(adv_set);
    return;
  }
  if (s_apple_adv_started) {
    return;
  }

  od_apple_stop(adv_set);

  if (!od_apple_apply_address(adv_set, cfg->apple_master_secret)) {
    return;
  }

  adv_len = od_apple_build_legacy_adv(cfg->apple_master_secret, adv, sizeof(adv));
  if (adv_len == 0u) {
    return;
  }

  sc = sl_bt_advertiser_set_timing(adv_set,
                                   OD_APPLE_ADV_INTERVAL_SLOTS,
                                   OD_APPLE_ADV_INTERVAL_SLOTS,
                                   0,
                                   0);
  if (sc != SL_STATUS_OK) {
    od_apple_log_fail("set_timing", sc);
    return;
  }

  sc = sl_bt_legacy_advertiser_set_data(adv_set,
                                        sl_bt_advertiser_advertising_data_packet,
                                        (uint8_t)adv_len,
                                        adv);
  if (sc != SL_STATUS_OK) {
    od_apple_log_fail("set_data", sc);
    return;
  }

  sc = sl_bt_legacy_advertiser_start(adv_set, sl_bt_legacy_advertiser_non_connectable);
  if (sc != SL_STATUS_OK) {
    od_apple_log_fail("advertiser_start", sc);
    s_apple_adv_started = false;
    return;
  }

  s_apple_adv_started = true;
  s_apple_last_fail_log_ms = 0u;
  printf("[OD][Apple] advertising (OpenHaystack static, %u byte payload)\r\n",
         (unsigned)adv_len);
}
