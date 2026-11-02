/* od_cfg_index -- keep a MAP of the config in RAM, not the config itself.
 *
 * The stored config lives in NVM3 (key 0x0F4401) and is the single source of
 * truth. Holding a second full copy in RAM cost 2064 bytes in `s_cfg_rec` --
 * the largest application symbol on a 32 KB part, larger even than the parsed
 * GlobalConfig (1115 B).
 *
 * Instead this keeps one 6-byte entry per packet: what it is, which instance,
 * where its body starts in the blob, and how long it is. 32 entries = 192 B.
 * Anything that needs a packet body reads it on demand with
 * nvm3_readPartialData, which the storage layer already uses.
 *
 * WHY AN INDEX AND NOT JUST RE-WALKING THE BLOB. Re-walking means reading from
 * flash and re-parsing every time anything wants a field, and the walk has to
 * stop at the first unknown packet id (that is the documented TLV behaviour).
 * An index is built once at boot, so a later lookup is a bounds check and one
 * partial read -- no parsing, no dependence on walk order.
 *
 * CONSEQUENCE: with the blob no longer resident, MAX_CONFIG_SIZE is bounded by
 * NVM3 (object size and repack headroom), not by RAM.
 */
#ifndef OD_CFG_INDEX_H
#define OD_CFG_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_CFG_INDEX_MAX 32u

typedef struct {
    uint8_t  type;      /* config packet id (0x01, 0x20, 0x2E, ...) */
    uint8_t  instance;  /* instance_number byte that preceded it */
    uint16_t offset;    /* byte offset of the packet BODY within the blob */
    uint16_t len;       /* body length in bytes */
} od_cfg_entry_t;

/* Build the index by walking the stored blob once. Returns entry count, or
 * -1 if the stored config is missing or fails CRC. */
int od_cfg_index_build(void);

int od_cfg_index_count(void);
const od_cfg_entry_t *od_cfg_index_entry(int i);

/* Find the Nth packet of a type. -> entry, or NULL. */
const od_cfg_entry_t *od_cfg_find(uint8_t type, uint8_t nth);

/* Read `len` bytes of a packet body, starting at `off` within that body.
 * Returns bytes read. This is the ONLY way to get config bytes at runtime --
 * there is no resident copy to point at. */
uint32_t od_cfg_read(const od_cfg_entry_t *e, uint16_t off,
                     uint8_t *dst, uint16_t len);

/* Total stored blob length, for diagnostics. */
uint32_t od_cfg_blob_len(void);

#ifdef __cplusplus
}
#endif
#endif /* OD_CFG_INDEX_H */
