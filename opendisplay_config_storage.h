#ifndef OPENDISPLAY_CONFIG_STORAGE_H
#define OPENDISPLAY_CONFIG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_CONFIG_SIZE 512

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

#endif
