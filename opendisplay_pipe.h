#ifndef OPENDISPLAY_PIPE_H
#define OPENDISPLAY_PIPE_H

#include "sl_bt_api.h"
#include <stdint.h>

void opendisplay_pipe_set_characteristic(uint16_t pipe_value_handle);

void opendisplay_pipe_on_connection_closed(void);

void opendisplay_pipe_handle_gatt_event(sl_bt_msg_t *evt);

#endif
