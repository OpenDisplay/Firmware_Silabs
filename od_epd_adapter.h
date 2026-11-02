/* od_epd_adapter -- bb_epaper's call surface, backed by config instead.
 *
 * opendisplay_display.cpp used exactly 13 bb_epaper methods. Rather than
 * rewrite 1000 lines of display logic in the same change that replaces the
 * driver, this presents those 13 methods and implements them on od_epd, whose
 * panel knowledge comes entirely from the device config.
 *
 * That keeps the risky part isolated: if a panel misbehaves after this change,
 * the cause is in od_epd or in the config, not in refactored transfer logic.
 *
 * bb_epaper's enum values are re-declared here because the call sites use them
 * as arguments. They are now just this adapter's own constants -- no vendored
 * header, no 90-entry panel enum, no mapping switch.
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
#define BBEP_3COLOR 2
#define BBEP_4COLOR 3
#define PLANE_0 0
#define PLANE_1 1
#define DEEP_SLEEP 1
#define REFRESH_FULL 0
#define REFRESH_FAST 1
#define REFRESH_PARTIAL 2

class OdEpdAdapter {
 public:
    /* Was: look up a bb_epaper panel enum. Now: load the panel descriptor out
     * of the stored config. Returns BBEP_ERROR_BAD_PARAMETER when the config
     * carries no drive data for this display, which is the honest answer --
     * there is no built-in table to fall back to. */
    int  setPanelType(int panel_ic_type);
    void setRotation(int degrees);
    void initIO(uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                uint8_t mosi, uint8_t sck, uint32_t speed);
    void wake(void);
    void sendPanelInitFull(void);
    int  startWrite(int plane);
    void writeCmd(uint8_t c);
    void writeData(uint8_t *data, int len);
    void setAddrWindow(int x, int y, int w, int h);
    void fillScreen(uint8_t colour);
    int  refresh(int mode, bool wait);
    void sleep(int mode);
    bool isBusy(void);

    /* New: select one of the config-described drive modes by name. The
     * 16-colour mode is just a 3-pass mode with LUTs, not a special case. */
    int  selectModeByName(const char *name);
    uint8_t passCount(void) const;
    int  beginPass(uint8_t pass);

 private:
    od_epd_t        _epd;
    od_panel_desc_t _desc;
    bool            _ready = false;
    int             _rotation = 0;
    uint8_t         _plane = 0;
};

#endif /* OD_EPD_ADAPTER_H */
