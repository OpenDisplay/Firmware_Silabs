#include "opendisplay_config_storage.h"
#include "nvm3_default.h"
#include "nvm3_default_config.h"
#include "sl_status.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OD_NVM3_CONFIG_KEY ((nvm3_ObjectKey_t)0x0F4401u)
#define CONFIG_STORAGE_MAGIC 0xDEADBEEFu
#define CONFIG_STORAGE_VERSION 1u

#if (MAX_CONFIG_SIZE + 16u) > NVM3_DEFAULT_MAX_OBJECT_SIZE
#error "MAX_CONFIG_SIZE + header exceeds NVM3_DEFAULT_MAX_OBJECT_SIZE"
#endif

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t crc;
  uint32_t data_len;
} od_config_header_t;

_Static_assert(sizeof(od_config_header_t) ==
                   offsetof(opendisplay_config_storage_t, data),
               "config header layout mismatch");

/* Staging record, HEAP-ALLOCATED AND TRANSIENT.
 *
 * This used to be a static `opendisplay_config_storage_t`, which made it the
 * largest application symbol on this 32 KB part (2064 B, more than the parsed
 * GlobalConfig at 1115 B) -- a permanently resident second copy of bytes that
 * already live in NVM3. Raising MAX_CONFIG_SIZE to 4096 would have made that
 * 4112 B of permanent RAM for a buffer used only while a config is being
 * written or parsed.
 *
 * Now it is allocated on first use and released afterwards, so the resident
 * cost is one pointer. NVM3 is the single source of truth and od_cfg_index
 * keeps a 192-byte map so runtime lookups need no buffer at all.
 *
 * Not re-entrant, which is unchanged: the BLE event loop is single-threaded. */
static opendisplay_config_storage_t *s_cfg_rec_p;

static opendisplay_config_storage_t *cfg_rec(void)
{
  if (s_cfg_rec_p == NULL) {
    s_cfg_rec_p = (opendisplay_config_storage_t *)malloc(
        sizeof(opendisplay_config_storage_t));
    if (s_cfg_rec_p != NULL) {
      memset(s_cfg_rec_p, 0, sizeof(opendisplay_config_storage_t));
    }
  }
  return s_cfg_rec_p;
}

void opendisplay_config_buf_release(void)
{
  if (s_cfg_rec_p != NULL) {
    free(s_cfg_rec_p);
    s_cfg_rec_p = NULL;
  }
}

uint8_t *opendisplay_config_buf(void)
{
  opendisplay_config_storage_t *r = cfg_rec();
  return (r != NULL) ? r->data : NULL;
}

bool initConfigStorage(void)
{
  return nvm3_defaultHandle != NULL;
}

uint32_t calculateConfigCRC(uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFu;

  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1u) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc >>= 1;
      }
    }
  }
  return ~crc;
}

bool saveConfig(uint8_t *config_data, uint32_t len)
{
  size_t header_sz = offsetof(opendisplay_config_storage_t, data);
  size_t total;
  sl_status_t sc;
  uint32_t crc;

  if (config_data == NULL || len > MAX_CONFIG_SIZE || nvm3_defaultHandle == NULL) {
    return false;
  }

  /* CRC before any header writes; config_data may alias cfg_rec()->data. */
  crc = calculateConfigCRC(config_data, len);
  if (config_data != cfg_rec()->data) {
    memcpy(cfg_rec()->data, config_data, len);
  }

  cfg_rec()->magic = CONFIG_STORAGE_MAGIC;
  cfg_rec()->version = CONFIG_STORAGE_VERSION;
  cfg_rec()->data_len = len;
  cfg_rec()->crc = crc;

  total = header_sz + len;
  sc = nvm3_writeData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, cfg_rec(), total);
  if (sc != SL_STATUS_OK) {
    printf("[OD] nvm3_writeData config key=0x%06lX sc=0x%04lX len=%u\r\n",
           (unsigned long)OD_NVM3_CONFIG_KEY, (unsigned long)sc, (unsigned)total);
  }
  return sc == SL_STATUS_OK;
}

bool loadConfig(uint8_t *config_data, uint32_t *len)
{
  od_config_header_t hdr;
  size_t header_sz = offsetof(opendisplay_config_storage_t, data);
  uint32_t obj_type = 0;
  size_t obj_len = 0;
  sl_status_t sc;

  if (config_data == NULL || len == NULL || nvm3_defaultHandle == NULL) {
    return false;
  }

  sc = nvm3_getObjectInfo(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, &obj_type, &obj_len);
  if (sc != SL_STATUS_OK) {
    return false;
  }
  if (obj_len < header_sz || obj_len > sizeof(opendisplay_config_storage_t)) {
    return false;
  }

  sc = nvm3_readPartialData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, &hdr, 0, header_sz);
  if (sc != SL_STATUS_OK) {
    return false;
  }

  if (hdr.magic != CONFIG_STORAGE_MAGIC) {
    return false;
  }
  if (hdr.data_len > MAX_CONFIG_SIZE || hdr.data_len > obj_len - header_sz) {
    return false;
  }
  if (hdr.data_len > *len) {
    return false;
  }

  if (hdr.data_len > 0u) {
    sc = nvm3_readPartialData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, config_data,
                              header_sz, hdr.data_len);
    if (sc != SL_STATUS_OK) {
      return false;
    }
  }

  if (calculateConfigCRC(config_data, hdr.data_len) != hdr.crc) {
    return false;
  }

  *len = hdr.data_len;
  return true;
}
