#ifndef OPENDISPLAY_LED_H
#define OPENDISPLAY_LED_H

#include <stdint.h>

void opendisplay_led_init(void);

/* 0 = success, 2 = instance/config invalid (pipe error code 0x02) */
int opendisplay_led_activate(uint8_t instance, const uint8_t *payload_after_instance, uint16_t rest_len);

#endif
