#ifndef OPENDISPLAY_CONSTANTS_H
#define OPENDISPLAY_CONSTANTS_H

/* Wire structs pull in the canonical opendisplay_protocol.h transitively, which
 * now owns the config-chunk sizes, NFC IC/record-type constants, and the
 * OD_BUS_TYPE_* enum. This header keeps only the Silabs-local constants the
 * canonical protocol does not define. */
#include "opendisplay_structs.h"

#define CONFIG_PKT_SYSTEM         0x01
#define CONFIG_PKT_MANUFACTURER   0x02
#define CONFIG_PKT_POWER          0x04
#define CONFIG_PKT_DISPLAY        0x20
#define CONFIG_PKT_LED            0x21
#define CONFIG_PKT_SENSOR         0x23
#define CONFIG_PKT_DATA_BUS       0x24
#define CONFIG_PKT_BINARY_INPUT   0x25
#define CONFIG_PKT_WIFI           0x26
#define CONFIG_PKT_SECURITY       0x27
#define CONFIG_PKT_TOUCH          0x28
#define CONFIG_PKT_BUZZER         0x29
#define CONFIG_PKT_NFC            0x2A
#define CONFIG_PKT_FLASH          0x2B
#define CONFIG_PKT_DATA_EXTENDED  0x2C

/* Wire sizes of config packets Silabs does not consume itself (touch/buzzer are
 * handled by the main MCU, data_extended is host-only). They must still be
 * skipped by their exact size so later packets (NFC 0x2A, flash 0x2B) parse. */
#define CONFIG_PKT_TOUCH_SIZE          32
#define CONFIG_PKT_BUZZER_SIZE         32
#define CONFIG_PKT_DATA_EXTENDED_SIZE  288

#define GPIO_PIN_UNUSED 0xFF

#define TRANSMISSION_MODE_ZIPXL          (1u << 0)
#define TRANSMISSION_MODE_ZIP            (1u << 1)
#define TRANSMISSION_MODE_DIRECT_WRITE   (1u << 3)

#endif
