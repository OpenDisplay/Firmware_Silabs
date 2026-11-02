#ifndef OPENDISPLAY_CONFIG_STORAGE_H
#define OPENDISPLAY_CONFIG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* 4080 = NVM3_DEFAULT_MAX_OBJECT_SIZE (4096) minus the 16-byte record header.
 *
 * WHAT BINDS THIS IS THE NVM3 REGION SIZE, NOT NVM3'S PER-OBJECT CAP. An earlier
 * version of this file set 4080 while the region was still 24576 bytes, reading
 * NVM3_MAX_OBJECT_SIZE_HIGH_LIMIT (4096) as the ceiling. That constant bounds
 * the SETTING; it is not a promise the configured region can hold such an
 * object. nvm3_open() rejects the instance when nvmSize < thrSize, and thrSize
 * scales with max-object -- at 24576 the margin was twenty bytes, and in
 * practice NVM3 would not open at all: stored config read back as 0 bytes and
 * every write NACKed (measured on ODBB1B76, 2026-11-02).
 *
 * 4080 is valid now only because NVM3_DEFAULT_NVM_SIZE was raised to 32768 in
 * the same change. The two move together -- see nvm3_default_config.h, which
 * also records why a resize does NOT need a chip erase.
 *
 * The staging buffer below is sized by this for the config WRITE path, which is
 * transient; see od_cfg_index.h for why nothing holds it afterwards. */
#define MAX_CONFIG_SIZE 4080

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t crc;
  uint32_t data_len;
  uint8_t data[MAX_CONFIG_SIZE];
} opendisplay_config_storage_t;

bool initConfigStorage(void);

bool saveConfig(uint8_t *config_data, uint32_t len);

bool loadConfig(uint8_t *config_data, uint32_t *len);

uint32_t calculateConfigCRC(uint8_t *data, uint32_t len);

/* Shared MAX_CONFIG_SIZE work buffer (pipe chunk / config read / parse). */
uint8_t *opendisplay_config_buf(void);

/* Release the transient staging buffer. Call after a config write or parse --
 * the resident cost should be a pointer, not 4 KB. */
void opendisplay_config_buf_release(void);

#endif
