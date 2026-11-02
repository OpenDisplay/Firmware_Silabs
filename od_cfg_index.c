/* od_cfg_index -- see od_cfg_index.h. NVM3 is the single source of truth; this
 * keeps only a map of where each packet body lives.
 */
#include "od_cfg_index.h"

#include <string.h>

#include "nvm3_default.h"
#include "nvm3_default_config.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_constants.h"
#include "sl_status.h"

#define OD_NVM3_CONFIG_KEY ((nvm3_ObjectKey_t)0x0F4401u)
#define CONFIG_STORAGE_MAGIC 0xDEADBEEFu
#define OD_CFG_HDR_SZ 16u    /* magic+version+crc+data_len, asserted in storage.c */

static od_cfg_entry_t s_index[OD_CFG_INDEX_MAX];
static uint8_t        s_count;
static uint32_t       s_blob_len;

/* Packet body sizes. The walk needs these to step over a packet, which is why
 * an unknown id has to abandon the rest of the blob -- there is no length on
 * the wire to skip by. Keeping the table here (rather than re-deriving it in
 * the walker) is what would let a size-table skip model be switched on later. */
static uint16_t body_size(uint8_t type)
{
    switch (type) {
        case CONFIG_PKT_SYSTEM:        return 22u;
        case CONFIG_PKT_MANUFACTURER:  return 22u;
        case CONFIG_PKT_POWER:         return 30u;
        case CONFIG_PKT_DISPLAY:       return 46u;
        case CONFIG_PKT_LED:           return 22u;
        case CONFIG_PKT_SENSOR:        return 30u;
        case CONFIG_PKT_DATA_BUS:      return 30u;
        case CONFIG_PKT_BINARY_INPUT:  return 30u;
        case CONFIG_PKT_WIFI:          return 160u;
        case CONFIG_PKT_TOUCH:         return CONFIG_PKT_TOUCH_SIZE;
        case CONFIG_PKT_BUZZER:        return CONFIG_PKT_BUZZER_SIZE;
        case CONFIG_PKT_NFC:           return 32u;
        case CONFIG_PKT_FLASH:         return 32u;
        case CONFIG_PKT_DATA_EXTENDED: return CONFIG_PKT_DATA_EXTENDED_SIZE;
        case CONFIG_PKT_FINDMY:        return CONFIG_PKT_FINDMY_SIZE;
        case CONFIG_PKT_DISPLAY_MODE:  return CONFIG_PKT_DISPLAY_MODE_SIZE;
        case CONFIG_PKT_DISPLAY_LUT:   return CONFIG_PKT_DISPLAY_LUT_SIZE;
        case CONFIG_PKT_DISPLAY_CALIB: return CONFIG_PKT_DISPLAY_CALIB_SIZE;
        default:                       return 0u;   /* unknown -> stop */
    }
}

static bool read_blob(uint32_t off, void *dst, uint32_t len)
{
    if (nvm3_defaultHandle == NULL) {
        return false;
    }
    return nvm3_readPartialData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, dst,
                                OD_CFG_HDR_SZ + off, len) == SL_STATUS_OK;
}

int od_cfg_index_build(void)
{
    uint32_t obj_type = 0;
    size_t   obj_len = 0;
    uint8_t  hdr[OD_CFG_HDR_SZ];

    s_count = 0;
    s_blob_len = 0;

    if (nvm3_defaultHandle == NULL) {
        return -1;
    }
    if (nvm3_getObjectInfo(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY,
                           &obj_type, &obj_len) != SL_STATUS_OK) {
        return -1;
    }
    if (obj_len <= OD_CFG_HDR_SZ) {
        return -1;
    }
    if (nvm3_readPartialData(nvm3_defaultHandle, OD_NVM3_CONFIG_KEY, hdr, 0,
                             OD_CFG_HDR_SZ) != SL_STATUS_OK) {
        return -1;
    }
    uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (magic != CONFIG_STORAGE_MAGIC) {
        return -1;
    }
    uint32_t data_len = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
                        ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
    if (data_len > (obj_len - OD_CFG_HDR_SZ) || data_len > MAX_CONFIG_SIZE) {
        return -1;
    }
    s_blob_len = data_len;

    /* Walk: [instance][type][body...] repeated, then a 2-byte CRC tail.
     * Two bytes at a time out of flash -- no buffer, no second copy. */
    uint32_t off = 0;
    while (off + 2u + 2u <= data_len && s_count < OD_CFG_INDEX_MAX) {
        uint8_t ph[2];
        if (!read_blob(off, ph, 2u)) {
            break;
        }
        uint16_t bsz = body_size(ph[1]);
        if (bsz == 0u) {
            break;      /* unknown id: documented behaviour is to stop here */
        }
        if (off + 2u + bsz > data_len - 2u) {
            break;      /* body would run past the CRC tail */
        }
        s_index[s_count].type     = ph[1];
        s_index[s_count].instance = ph[0];
        s_index[s_count].offset   = (uint16_t)(off + 2u);
        s_index[s_count].len      = bsz;
        s_count++;
        off += 2u + bsz;
    }
    return (int)s_count;
}

int od_cfg_index_count(void) { return (int)s_count; }

const od_cfg_entry_t *od_cfg_index_entry(int i)
{
    return (i >= 0 && i < (int)s_count) ? &s_index[i] : NULL;
}

const od_cfg_entry_t *od_cfg_find(uint8_t type, uint8_t nth)
{
    uint8_t seen = 0;
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_index[i].type == type) {
            if (seen == nth) {
                return &s_index[i];
            }
            seen++;
        }
    }
    return NULL;
}

uint32_t od_cfg_read(const od_cfg_entry_t *e, uint16_t off,
                     uint8_t *dst, uint16_t len)
{
    if (e == NULL || dst == NULL || off >= e->len) {
        return 0u;
    }
    if ((uint32_t)off + len > e->len) {
        len = (uint16_t)(e->len - off);
    }
    return read_blob((uint32_t)e->offset + off, dst, len) ? len : 0u;
}

uint32_t od_cfg_blob_len(void) { return s_blob_len; }
