#ifndef OPENDISPLAY_LED_H
#define OPENDISPLAY_LED_H

#include <stdbool.h>
#include <stdint.h>

void opendisplay_led_init(void);

/* 0 = success, 2 = instance/config invalid (pipe error code 0x02) */
int opendisplay_led_activate(uint8_t instance, const uint8_t *payload_after_instance, uint16_t rest_len);

/* 0 = stopped or idle, 2 = wrong instance while another sequence is active */
int opendisplay_led_stop(uint8_t instance, bool instance_given);

void opendisplay_led_process(void);

bool opendisplay_led_is_active(void);

#endif
