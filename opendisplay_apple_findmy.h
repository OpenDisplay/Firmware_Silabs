#ifndef OPENDISPLAY_APPLE_FINDMY_H
#define OPENDISPLAY_APPLE_FINDMY_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#define OD_APPLE_ADV_KEY_LEN         28u
#define OD_APPLE_LEGACY_ADV_LEN      31u
#define OD_APPLE_ADV_INTERVAL_SLOTS  1600u

bool od_apple_config_active(const struct FindMyConfig *cfg);

void od_apple_sync(uint8_t adv_set, const struct FindMyConfig *cfg);

void od_apple_stop(uint8_t adv_set);

#endif
