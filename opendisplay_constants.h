#ifndef OPENDISPLAY_CONSTANTS_H
#define OPENDISPLAY_CONSTANTS_H

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

#define CONFIG_CHUNK_SIZE               200
#define CONFIG_CHUNK_SIZE_WITH_PREFIX   202
#define MAX_CONFIG_CHUNKS               20
#define MAX_RESPONSE_DATA_SIZE          100

#define GPIO_PIN_UNUSED 0xFF

#endif
