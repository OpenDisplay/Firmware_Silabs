#ifndef OPENDISPLAY_BLE_H
#define OPENDISPLAY_BLE_H

#include "sl_bt_api.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GlobalConfig;

void opendisplay_ble_on_boot(uint8_t advertising_set_handle);

const struct GlobalConfig *opendisplay_get_global_config(void);

void opendisplay_ble_reload_config_from_nvm(void);

void opendisplay_ble_restart_advertising(uint8_t advertising_set_handle);

void opendisplay_ble_on_event(sl_bt_msg_t *evt);
void opendisplay_ble_process(void);
void opendisplay_ble_schedule_dfu(void);
void opendisplay_ble_schedule_deep_sleep(void);

uint16_t opendisplay_ble_get_app_version(void);

void opendisplay_ble_copy_msd_bytes(uint8_t out[16]);

bool opendisplay_ble_nfc_read(uint8_t *type_out, uint8_t *data_out, uint16_t *data_len_io, uint16_t max_len);
bool opendisplay_ble_nfc_write(uint8_t type, const uint8_t *data, uint16_t data_len);

#ifdef __cplusplus
}
#endif

#endif
