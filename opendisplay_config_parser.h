#ifndef OPENDISPLAY_CONFIG_PARSER_H
#define OPENDISPLAY_CONFIG_PARSER_H

#include <stdbool.h>
#include <stdint.h>
#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

bool parseConfigBytes(uint8_t *configData, uint32_t configLen, struct GlobalConfig *globalConfig);

bool loadGlobalConfig(struct GlobalConfig *globalConfig);

const struct SecurityConfig *od_get_parsed_security(void);

#ifdef __cplusplus
}
#endif

#endif
