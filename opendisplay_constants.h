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
#define CONFIG_PKT_NFC            0x2A
#define CONFIG_PKT_FLASH          0x2B

#define CONFIG_CHUNK_SIZE               200
#define CONFIG_CHUNK_SIZE_WITH_PREFIX   202
#define MAX_CONFIG_CHUNKS               20
#define MAX_RESPONSE_DATA_SIZE          100

#define GPIO_PIN_UNUSED 0xFF

/* OpenDisplay config struct DataBus.bus_type (matches toolbox presets, e.g. 0x01 = I2C). */
#define OD_BUS_TYPE_I2C 1u

#define OD_NFC_IC_AUTO      0u
#define OD_NFC_IC_TNB132M   1u

#define OD_NFC_REC_TEXT             0u
#define OD_NFC_REC_URI              1u
#define OD_NFC_REC_WELL_KNOWN_RAW   2u
/* BLE payload: [mime_type_len][mime_type ascii][mime body UTF-8] -> NDEF MIME (SR). */
#define OD_NFC_REC_MIME             3u
#define OD_NFC_REC_RAW_NDEF         4u

#define TRANSMISSION_MODE_ZIPXL          (1u << 0)
#define TRANSMISSION_MODE_ZIP            (1u << 1)
#define TRANSMISSION_MODE_DIRECT_WRITE   (1u << 3)

#endif
