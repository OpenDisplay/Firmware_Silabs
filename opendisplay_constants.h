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
#define CONFIG_PKT_FINDMY         0x2D
/* Panel drive data, structs 2.2. These live in the LENGTH-PREFIXED id range:
 * on the wire each is [number][id][length:2 LE][body] (SinglePacketHeaderEx),
 * so a reader skips an unknown 0x80+ id by length instead of abandoning the
 * tail. The legacy range 0x00-0x7F is FROZEN; 0x2D findmy_config is its last
 * id. The panel packet carries the per-panel drive properties that 0x20 never
 * had: whether the BUSY line can be trusted, which level means idle, the
 * logical-to-panel colour code map, and the sleep sequence. Without it a
 * driver still needs a built-in per-panel table, which this change deletes. */
#define CONFIG_PKT_DISPLAY_MODE   0xAE
#define CONFIG_PKT_DISPLAY_LUT    0xAF
#define CONFIG_PKT_DISPLAY_CALIB  0xB0
#define CONFIG_PKT_DISPLAY_PANEL  0xB1
/* Ids 0x2E-0x31 were burned by pre-release lab builds carrying FIXED-SIZE
 * drafts of the same packets. RESERVED, never reuse; the index walker keeps
 * their sizes so a stale config walks PAST them instead of abandoning its
 * tail (the driver no longer consumes them). */
#define CONFIG_PKT_BURNED_2E      0x2E
#define CONFIG_PKT_BURNED_2F      0x2F
#define CONFIG_PKT_BURNED_30      0x30
#define CONFIG_PKT_BURNED_31      0x31

/* Wire sizes of config packets Silabs does not consume itself (touch/buzzer are
 * handled by the main MCU, data_extended is host-only). They must still be
 * skipped by their exact size so later packets (NFC 0x2A, flash 0x2B) parse. */
#define CONFIG_PKT_TOUCH_SIZE          32
#define CONFIG_PKT_BUZZER_SIZE         32
#define CONFIG_PKT_DATA_EXTENDED_SIZE  288
#define CONFIG_PKT_FINDMY_SIZE         128
#define CONFIG_PKT_BURNED_2E_SIZE      157
#define CONFIG_PKT_BURNED_2F_SIZE      541
#define CONFIG_PKT_BURNED_30_SIZE      116
#define CONFIG_PKT_BURNED_31_SIZE      36
/* Fixed HEAD sizes of the variable-body drive packets (the tails are seq[],
 * lut data[], calibration entries[] and sleep_seq[]; total body length comes
 * from the wire header). Asserted against the structs in od_epd_adapter. */
#define CONFIG_PKT_DISPLAY_MODE_HEAD   37
#define CONFIG_PKT_DISPLAY_LUT_HEAD    4
#define CONFIG_PKT_DISPLAY_CALIB_HEAD  4
#define CONFIG_PKT_DISPLAY_PANEL_HEAD  13

#define GPIO_PIN_UNUSED 0xFF

#define TRANSMISSION_MODE_ZIPXL          (1u << 0)
#define TRANSMISSION_MODE_ZIP            (1u << 1)
#define TRANSMISSION_MODE_DIRECT_WRITE   (1u << 3)

#endif
