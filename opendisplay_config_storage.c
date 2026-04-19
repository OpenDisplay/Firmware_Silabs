#include "opendisplay_config_storage.h"
#include "nvm3_default.h"
#include "sl_status.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define OD_NVM3_CONFIG_KEY ((nvm3_ObjectKey_t)0x0F4401u)
#define CONFIG_STORAGE_MAGIC 0xDEADBEEFu
#define CONFIG_STORAGE_VERSION 1u

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
  opendisplay_config_storage_t rec;
  size_t header_sz = offsetof(opendisplay_config_storage_t, data);
  size_t total;

  if (len > MAX_CONFIG_SIZE || nvm3_defaultHandle == NULL) {
    return false;
  }

  memset(&rec, 0, sizeof(rec));
  rec.magic = CONFIG_STORAGE_MAGIC;
  rec.version = CONFIG_STORAGE_VERSION;
  rec.data_len = len;
  rec.crc = calculateConfigCRC(config_data, len);
  memcpy(rec.data, config_data, len);

  total = header_sz + len;
  {
    sl_status_t sc = nvm3_writeData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, &rec, total);
    if (sc != SL_STATUS_OK) {
      printf("[OD] nvm3_writeData config key=0x%06lX sc=0x%04lX len=%u\r\n",
             (unsigned long)OD_NVM3_CONFIG_KEY, (unsigned long)sc, (unsigned)total);
    }
    return sc == SL_STATUS_OK;
  }
}

bool loadConfig(uint8_t *config_data, uint32_t *len)
{
  opendisplay_config_storage_t rec;
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
  if (obj_len < header_sz || obj_len > sizeof(rec)) {
    return false;
  }

  sc = nvm3_readData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, &rec, obj_len);
  if (sc != SL_STATUS_OK) {
    return false;
  }

  if (rec.magic != CONFIG_STORAGE_MAGIC) {
    return false;
  }
  if (rec.data_len > MAX_CONFIG_SIZE || rec.data_len > obj_len - header_sz) {
    return false;
  }
  if (rec.data_len > *len) {
    return false;
  }
  if (calculateConfigCRC(rec.data, rec.data_len) != rec.crc) {
    return false;
  }

  memcpy(config_data, rec.data, rec.data_len);
  *len = rec.data_len;
  return true;
}
