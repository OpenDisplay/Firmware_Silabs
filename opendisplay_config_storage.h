#ifndef OPENDISPLAY_CONFIG_STORAGE_H
#define OPENDISPLAY_CONFIG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* 4096 since the blob is no longer held in RAM -- od_cfg_index keeps a 192-byte
 * map and bodies are read from NVM3 on demand. The bound is now NVM3: the
 * object must fit NVM3_DEFAULT_MAX_OBJECT_SIZE (4112 = 4096 + 16-byte header)
 * and leave repack headroom inside the 24576-byte NVM region (~17%).
 *
 * The staging buffer below is still sized by this for the config WRITE path,
 * which is transient; see od_cfg_index.h for why nothing holds it afterwards. */
/* 4080 = NVM3's hard per-object cap (NVM3_MAX_OBJECT_SIZE_HIGH_LIMIT = 4096)
 * minus the 16-byte record header. This is the ceiling: raising it further
 * needs the config split across multiple NVM3 objects, not a bigger number. */
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
