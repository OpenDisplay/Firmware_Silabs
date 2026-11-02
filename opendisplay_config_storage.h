#ifndef OPENDISPLAY_CONFIG_STORAGE_H
#define OPENDISPLAY_CONFIG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* 2096 = NVM3_DEFAULT_MAX_OBJECT_SIZE (2112) minus the 16-byte record header.
 *
 * I PREVIOUSLY SET THIS TO 4080 AND IT BROKE A DEVICE. The reasoning was that
 * NVM3's hard per-object cap is 4096 (NVM3_MAX_OBJECT_SIZE_HIGH_LIMIT), so 4080
 * must be the ceiling. That cap is a bound on the SETTING, not a promise the
 * configured NVM3 region can store such an object: the limit that actually
 * binds is the 24576-byte region, and nvm3_default_config.h says so in its own
 * comment -- "3 pages (24576) is enough for max-object 2112".
 *
 * Measured on ODBB1B76 after OTAing a build with max-object 4096: the stored
 * config came back as 0 bytes and every config write NACKed at the final chunk,
 * i.e. at saveConfig's nvm3_writeData. Raising max-object changes the NVM3
 * instance parameters, which invalidates the existing contents.
 *
 * To go beyond 2096 the NVM3 region has to grow, and that is not a free change:
 * nvm3_default_config.h warns that changing size or base invalidates NVM3 and
 * needs a chip erase, which needs a wired programmer -- not an OTA.
 *
 * The staging buffer below is sized by this for the config WRITE path, which is
 * transient; see od_cfg_index.h for why nothing holds it afterwards. */
#define MAX_CONFIG_SIZE 2096

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
