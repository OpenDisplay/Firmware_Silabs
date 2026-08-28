#ifndef OPENDISPLAY_NET_A_ADV_H
#define OPENDISPLAY_NET_A_ADV_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#define OD_NET_A_ADV_KEY_LEN         28u
#define OD_NET_A_LEGACY_ADV_LEN      31u
#define OD_NET_A_ADV_INTERVAL_SLOTS  1600u

bool od_net_a_config_active(const struct FindMyConfig *cfg);

void od_net_a_sync(uint8_t adv_set, const struct FindMyConfig *cfg);

void od_net_a_stop(uint8_t adv_set);

#endif
