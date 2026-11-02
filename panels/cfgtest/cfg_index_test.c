/* cfg_index_test -- run the REAL od_cfg_index.c over a REAL config blob.
 *
 * WHY THIS EXISTS. Two bugs in od_cfg_index.c were invisible to everything
 * else in the tree, and both produced the same symptom: an empty index, which
 * reads as "this device has no drive data" rather than as a defect.
 *
 *   1. The walk started at offset 0. A config blob begins
 *      [0x00 0x00][version] before the first packet, so it parsed the padding
 *      as a packet header, got type 0x00, had no size for it and stopped.
 *   2. body_size() had no entry for SECURITY (0x27). That packet sorts before
 *      the drive packets (0x2E/0x2F/0x31), so on any encrypted device the walk
 *      abandoned the tail before ever reaching them.
 *
 * The firmware build could not catch either: it only proves the code compiles.
 * The host round-trip could not catch either: it inspects the drive packets on
 * their own, never inside a whole blob with a preamble and a security packet.
 *
 * So this links the actual od_cfg_index.c against a stub NVM3 holding a blob
 * laid out exactly as py-opendisplay's serialize_config() and the web toolbox
 * emit it, and asserts the drive packets are found with the right offsets.
 *
 * Build: see cfg_index_test.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvm3_default.h"
#include "sl_status.h"
#include "od_cfg_index.h"
#include "opendisplay_constants.h"

/* ---- stub NVM3 backed by a file ---------------------------------------- */
static uint8_t  g_obj[8192];
static size_t   g_obj_len;
static nvm3_Handle_t g_handle;
nvm3_Handle_t  *nvm3_defaultHandle = &g_handle;

uint32_t nvm3_readPartialData(nvm3_Handle_t *h, nvm3_ObjectKey_t key, void *dst,
                              size_t off, size_t len)
{
    (void)h; (void)key;
    if (off + len > g_obj_len) {
        return SL_STATUS_FAIL;
    }
    memcpy(dst, &g_obj[off], len);
    return SL_STATUS_OK;
}

uint32_t nvm3_getObjectInfo(nvm3_Handle_t *h, nvm3_ObjectKey_t key,
                            uint32_t *type, size_t *len)
{
    (void)h; (void)key;
    *type = 0;
    *len = g_obj_len;
    return SL_STATUS_OK;
}

/* ---- test -------------------------------------------------------------- */
static const char *type_name(uint8_t t)
{
    switch (t) {
        case CONFIG_PKT_SYSTEM:        return "system";
        case CONFIG_PKT_MANUFACTURER:  return "manufacturer";
        case CONFIG_PKT_POWER:         return "power";
        case CONFIG_PKT_DISPLAY:       return "display";
        case CONFIG_PKT_LED:           return "led";
        case CONFIG_PKT_SECURITY:      return "security";
        case CONFIG_PKT_DISPLAY_MODE:  return "display_mode";
        case CONFIG_PKT_DISPLAY_LUT:   return "display_lut";
        case CONFIG_PKT_DISPLAY_PANEL: return "display_panel";
        case CONFIG_PKT_DISPLAY_CALIB: return "display_calibration";
        default:                       return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s nvm3_object.bin\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    g_obj_len = fread(g_obj, 1, sizeof(g_obj), f);
    fclose(f);

    int n = od_cfg_index_build();
    printf("entries %d\n", n);
    if (n < 0) {
        return 1;
    }
    printf("blob_len %u\n", (unsigned)od_cfg_blob_len());
    for (int i = 0; i < n; i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);
        printf("entry %d type 0x%02X %s instance %u sub %u offset %u len %u\n",
               i, e->type, type_name(e->type), e->instance, e->sub,
               e->offset, e->len);
    }

    /* Read one byte back through od_cfg_read for every drive packet, which is
     * the path the driver actually uses. An offset that is wrong by the 3-byte
     * preamble still yields plausible bytes, so print them for the differ. */
    for (int i = 0; i < n; i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);
        if (e->type != CONFIG_PKT_DISPLAY_MODE &&
            e->type != CONFIG_PKT_DISPLAY_LUT &&
            e->type != CONFIG_PKT_DISPLAY_PANEL) {
            continue;
        }
        uint8_t head[8];
        uint32_t got = od_cfg_read(e, 0u, head, sizeof(head));
        printf("read 0x%02X got %u:", e->type, got);
        for (uint32_t k = 0; k < got; k++) {
            printf(" %02x", head[k]);
        }
        printf("\n");
    }
    return 0;
}
