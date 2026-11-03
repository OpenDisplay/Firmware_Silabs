/* od_epd_adapter -- bb_epaper's call surface, backed by config instead.
 *
 * opendisplay_display.cpp used 16 bb_epaper entry points. Rather than rewrite
 * 1000 lines of display logic in the same change that replaces the driver, this
 * presents those entry points and implements them on od_epd, whose panel
 * knowledge comes entirely from the device config.
 *
 * That keeps the risky part isolated: if a panel misbehaves after this change,
 * the cause is in od_epd or in the config, not in refactored transfer logic.
 *
 * bb_epaper's enum values are re-declared here because the call sites use them
 * as arguments. They are now just this adapter's own constants -- no vendored
 * header, no 90-entry panel enum, no mapping switch.
 *
 * WHERE THE BYTES COME FROM. Nothing here holds the config. The io callbacks
 * this class installs read through od_cfg_index, which reads NVM3 directly;
 * sequences are copied per pass (128 B bounded) and waveforms are streamed.
 */
#ifndef OD_EPD_ADAPTER_H
#define OD_EPD_ADAPTER_H

#include <stdint.h>

#include "od_epd.h"
#include "opendisplay_runtime.h"

/* Kept at bb_epaper's values so the existing call sites read unchanged. */
#define BBEP_SUCCESS 0
#define BBEP_ERROR_BAD_PARAMETER 1
#define BBEP_WHITE 1
#define PLANE_0 0
#define PLANE_1 1
#define DEEP_SLEEP 1
#define REFRESH_FULL 0
#define REFRESH_FAST 1

class OdEpdAdapter {
 public:
    /* Was: map panel_ic_type to a bb_epaper panel enum, then look up compiled
     * tables. Now: load the panel descriptor out of the stored config. Returns
     * BBEP_ERROR_BAD_PARAMETER when the config carries no drive data for this
     * display, which is the honest answer -- there is no built-in table to fall
     * back to, so a panel with no 0x2E/0x31 packets genuinely cannot be driven.
     */
    int  setPanelType(uint8_t display_instance);
    void setRotation(int degrees);
    void initIO(uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                uint8_t mosi, uint8_t sck, uint32_t speed);

    /* Select a drive mode BY INDEX. There is no select-by-name: the firmware
     * has no opinion about what modes are called, and mode 0 is by convention
     * the plain full refresh the boot screen uses. */
    int  selectModeIndex(uint8_t index);
    uint8_t modeCount(void) const { return _desc.mode_count; }
    uint8_t passCount(void) const;

    /* Run the pass up to the image marker. Replaces bb_epaper's
     * wake + sendPanelInitFull + setAddrWindow + startWrite, which were four
     * calls because the library split the init across them; here the sequence
     * says where the frame goes. */
    int  beginPass(uint8_t pass);
    void writeData(const uint8_t *data, int len);
    /* Two-plane panels only (3-colour). Advances to the sequence's next image
     * marker; fails if the sequence declares no second plane. */
    int  nextPlane(void);
    /* Run the sequence tail: power-on, refresh opcode, BUSY wait. */
    int  finishPass(void);
    void sleep(int mode);
    bool isBusy(void);

    /* Raw SPI debug passthrough (CMD_SPI_RAW). writeRaw bypasses the pass state
     * machine on purpose: a raw session has no pass in flight, and routing it
     * through writeData would be rejected for not being inside one. */
    void writeCmd(uint8_t c);
    void writeRaw(const uint8_t *data, int len);
    int  reset(uint8_t hold_ms);

    uint16_t width(void) const  { return _desc.width; }
    uint16_t height(void) const { return _desc.height; }
    uint8_t  colourScheme(void) const { return _desc.colour_scheme; }
    bool     ready(void) const  { return _ready; }

    /* The ACTIVE mode's header (temperature window, passes, scheme) -- what
     * the device-side temp_policy enforcement reads. Valid after a successful
     * selectModeIndex. */
    const od_epd_mode_t &activeMode(void) const { return _epd.active; }
    /* Raw od_epd rc of the last beginPass, so the 0x0085 handler can turn
     * OD_EPD_RC_UNSUP into OD_ERR_DRIVE_SEQ_UNSUPPORTED instead of the
     * generic NACK. */
    int lastRc(void) const { return _last_rc; }

 private:
    od_epd_t        _epd;
    od_panel_desc_t _desc;
    bool            _ready = false;
    int             _rotation = 0;
    int             _last_rc = 0;
};

#endif /* OD_EPD_ADAPTER_H */
