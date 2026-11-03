/* od_cfg_index -- see od_cfg_index.h. NVM3 is the single source of truth; this
 * keeps only a map of where each packet body lives.
 */
#include "od_cfg_index.h"

#include <stdio.h>
#include <string.h>

#include "nvm3_default.h"
#include "nvm3_default_config.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_constants.h"
#include "sl_status.h"

#define OD_NVM3_CONFIG_KEY ((nvm3_ObjectKey_t)0x0F4401u)
#define CONFIG_STORAGE_MAGIC 0xDEADBEEFu
#define OD_CFG_HDR_SZ 16u    /* magic+version+crc+data_len, asserted in storage.c */
#define OD_CFG_PREAMBLE_SZ 3u /* blob starts [0x00 0x00][version] before any packet */

static od_cfg_entry_t s_index[OD_CFG_INDEX_MAX];
static uint8_t        s_count;
static uint32_t       s_blob_len;

/* Packet body sizes. The walk needs these to step over a packet, which is why
 * an unknown id has to abandon the rest of the blob -- there is no length on
 * the wire to skip by. Keeping the table here (rather than re-deriving it in
 * the walker) is what would let a size-table skip model be switched on later. */
/* EVERY packet type that can appear before the drive packets must be here, not
 * just the ones this file cares about. An unknown id abandons the rest of the
 * blob, and the drive packets sit at the END of the id space -- so one missing
 * entry means the walk stops short and the panel has no drive data at all.
 *
 * SECURITY (0x27) was missing from the first version of this table. It sorts
 * before 0x2E, so the walk stopped at it on every encrypted device: the index
 * came back with the low-numbered packets only and od_epd_desc_from_config
 * always failed. Nothing in the build or the host-side round-trip could catch
 * that, because both of those exercise the blob WITHOUT a security packet. */
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
        case CONFIG_PKT_SECURITY:      return 64u;
        case CONFIG_PKT_TOUCH:         return CONFIG_PKT_TOUCH_SIZE;
        case CONFIG_PKT_BUZZER:        return CONFIG_PKT_BUZZER_SIZE;
        case CONFIG_PKT_NFC:           return 32u;
        case CONFIG_PKT_FLASH:         return 32u;
        case CONFIG_PKT_DATA_EXTENDED: return CONFIG_PKT_DATA_EXTENDED_SIZE;
        case CONFIG_PKT_FINDMY:        return CONFIG_PKT_FINDMY_SIZE;
        /* Burned Rev 1 drive-packet ids. The driver no longer consumes them,
         * but a stale (pre-renumbering) config must walk PAST them rather than
         * abandon its tail -- 0x2D..0x31 sort after everything else legacy. */
        case CONFIG_PKT_BURNED_2E:     return CONFIG_PKT_BURNED_2E_SIZE;
        case CONFIG_PKT_BURNED_2F:     return CONFIG_PKT_BURNED_2F_SIZE;
        case CONFIG_PKT_BURNED_30:     return CONFIG_PKT_BURNED_30_SIZE;
        case CONFIG_PKT_BURNED_31:     return CONFIG_PKT_BURNED_31_SIZE;
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

    /* THE PACKETS DO NOT START AT OFFSET 0.
     *
     * The blob is [0x00 0x00][version][packets...][crc16], per
     * serialize_config() in py-opendisplay and the skip in
     * opendisplay_config_parser.c. Walking from 0 reads the two padding bytes
     * as a header, gets type 0x00, finds no size for it and stops immediately
     * -- producing an empty index that looks like "this device has no drive
     * data" rather than like a bug. */
    uint32_t off = OD_CFG_PREAMBLE_SZ;
    while (off + 2u + 2u <= data_len && s_count < OD_CFG_INDEX_MAX) {
        uint8_t  ph[4];
        uint8_t  b2[2] = { 0u, 0u };
        uint16_t bsz;
        uint32_t hdr_sz;
        bool     record = true;

        if (!read_blob(off, ph, 2u)) {
            break;
        }
        if (ph[1] >= 0x80u) {
            /* LENGTH-PREFIXED range (structs 2.2): [number][id][length:2 LE].
             * The explicit length is what lets an UNKNOWN id be skipped here
             * instead of abandoning the tail -- the exact failure the frozen
             * legacy range kept producing (0x2D, then the burned 0x2E-0x31). */
            if (off + 4u > data_len - 2u || !read_blob(off, ph, 4u)) {
                break;
            }
            hdr_sz = 4u;
            bsz = (uint16_t)(ph[2] | ((uint16_t)ph[3] << 8));
            if (bsz == 0u) {
                printf("[OD] cfg index: 0x%02X with zero body at %u, skipping\r\n",
                       (unsigned)ph[1], (unsigned)off);
            }
            switch (ph[1]) {
                case CONFIG_PKT_DISPLAY_MODE:
                case CONFIG_PKT_DISPLAY_LUT:
                case CONFIG_PKT_DISPLAY_CALIB:
                case CONFIG_PKT_DISPLAY_PANEL:
                    break;
                default:
                    /* Unknown but skippable -- that is the whole point. */
                    printf("[OD] cfg index: skipping unknown 0x%02X (%u B) at %u\r\n",
                           (unsigned)ph[1], (unsigned)bsz, (unsigned)off);
                    record = false;
                    break;
            }
        } else {
            hdr_sz = 2u;
            bsz = body_size(ph[1]);
            if (bsz == 0u) {
                /* Legacy range has no length on the wire: a type missing from
                 * body_size() abandons the tail, and saying so matters -- the
                 * silent version of this cost a whole debugging session. */
                printf("[OD] cfg index: unknown legacy packet 0x%02X at %u, "
                       "abandoning %u B of tail\r\n",
                       (unsigned)ph[1], (unsigned)off,
                       (unsigned)(data_len - off));
                break;
            }
        }
        if (off + hdr_sz + bsz > data_len - 2u) {
            break;      /* body would run past the CRC tail */
        }
        if (record && bsz > 0u) {
            /* Identity comes from the body: body[0] instance_number, body[1]
             * the per-type discriminator. See the note in od_cfg_index.h. */
            if (bsz >= 2u) {
                (void)read_blob(off + hdr_sz, b2, 2u);
            }
            s_index[s_count].type     = ph[1];
            s_index[s_count].instance = b2[0];
            s_index[s_count].sub      = b2[1];
            s_index[s_count].offset   = (uint16_t)(off + hdr_sz);
            s_index[s_count].len      = bsz;
            s_count++;
        }
        off += hdr_sz + bsz;
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
