/* od_epd_io_silabs -- EFR32BG22 hardware access for od_epd.
 *
 * Rebased off bb_epaper's silabs_efr32_io.inl. That file reached into a
 * BBEPDISP struct in 61 places for pin numbers, flags and chip type, which
 * meant the HAL could not be used without the whole vendored library and could
 * not be reasoned about without knowing what bb_epaper would put in those
 * fields. Here the pins are this file's own state and everything else is a
 * parameter.
 *
 * Three behaviours did NOT survive the move, deliberately:
 *
 *   - IT8951 command framing (bbepWriteIT8951Cmd/Data/CmdArgs). Those panels
 *     are a different transport (16-bit preamble words, MISO reads) and none is
 *     in this branch. Leaving dead framing here would imply support that is not
 *     tested.
 *   - BBEP_CS_EVERY_BYTE. No panel in this branch sets it. If one needs it,
 *     it belongs in the panel config packet, not in a compiled-in flag.
 *   - The BUSY pull direction was chosen from bb_epaper's chip_type enum
 *     (UC81xx -> pull-up, else pull-down). It is now derived from the config's
 *     declared idle level, which is the fact that actually mattered.
 *
 * SPI is USART1 in synchronous master mode with a bit-bang fallback. The
 * fallback is not belt-and-braces: a stalled TXBL latches s_hw_spi off for the
 * rest of the session, because clocking half a frame and then hanging is worse
 * than clocking all of it slowly.
 */
#include "od_epd_io_silabs.h"

#include "em_cmu.h"
#include "em_gpio.h"
#include "sl_sleeptimer.h"
#include "sl_udelay.h"

#define PIN_UNUSED 0xFFu
#define SPI_GUARD  200000u

static struct {
    uint8_t  dc, rst, busy, cs, mosi, sck;
    uint32_t speed;
    bool     hw_spi;
    bool     busy_idle_high;
} s_io;

/* Packed port/pin, same encoding as the OpenDisplay LED and display configs:
 * high nibble = port index from A, low nibble = pin. */
static bool decode(uint8_t v, GPIO_Port_TypeDef *port, uint8_t *pin)
{
    unsigned pr, pn;
    if (v == PIN_UNUSED) {
        return false;
    }
    pr = (unsigned)(v >> 4) & 0x0Fu;
    pn = (unsigned)(v & 0x0Fu);
    if (pr > (unsigned)GPIO_PORT_MAX || pn > 15u) {
        return false;
    }
    *port = (GPIO_Port_TypeDef)(gpioPortA + pr);
    *pin = (uint8_t)pn;
    return true;
}

static void pin_out(uint8_t v, bool level)
{
    GPIO_Port_TypeDef port;
    uint8_t pin;
    if (!decode(v, &port, &pin)) {
        return;
    }
    if (level) {
        GPIO_PinOutSet(port, pin);
    } else {
        GPIO_PinOutClear(port, pin);
    }
}

static void pin_mode_out(uint8_t v, bool level)
{
    GPIO_Port_TypeDef port;
    uint8_t pin;
    if (decode(v, &port, &pin)) {
        GPIO_PinModeSet(port, pin, gpioModePushPull, level ? 1u : 0u);
    }
}

static int pin_in(uint8_t v)
{
    GPIO_Port_TypeDef port;
    uint8_t pin;
    if (!decode(v, &port, &pin)) {
        return 0;
    }
    return (int)GPIO_PinInGet(port, pin);
}

static void hw_spi_init(void)
{
    GPIO_Port_TypeDef mosi_port, sck_port;
    uint8_t mosi_pin, sck_pin;

    s_io.hw_spi = false;
    if (s_io.speed == 0u) {
        return;                  /* caller asked for bit-bang */
    }
    if (!decode(s_io.mosi, &mosi_port, &mosi_pin) ||
        !decode(s_io.sck, &sck_port, &sck_pin)) {
        return;
    }

    CMU_ClockEnable(cmuClock_GPIO, true);
    CMU_ClockEnable(cmuClock_USART1, true);

    GPIO_PinModeSet(mosi_port, mosi_pin, gpioModePushPull, 0);
    GPIO_PinModeSet(sck_port, sck_pin, gpioModePushPull, 0);

    USART1->EN = 0;
    USART1->CMD = USART_CMD_RXDIS | USART_CMD_TXDIS | USART_CMD_MASTERDIS;
    USART1->CTRL = USART_CTRL_SYNC | USART_CTRL_MSBF;
    USART1->FRAME = USART_FRAME_DATABITS_EIGHT;

    {
        uint32_t clk = CMU_ClockFreqGet(cmuClock_USART1);
        if (clk > (2u * s_io.speed)) {
            uint32_t div = (uint32_t)(((256ULL * (uint64_t)clk) /
                                       (2ULL * (uint64_t)s_io.speed)) - 256ULL);
            USART1->CLKDIV = (div << _USART_CLKDIV_DIV_SHIFT) & _USART_CLKDIV_DIV_MASK;
        } else {
            USART1->CLKDIV = 0u;
        }
    }

    GPIO->USARTROUTE[1].TXROUTE =
        ((uint32_t)mosi_port << _GPIO_USART_TXROUTE_PORT_SHIFT) |
        ((uint32_t)mosi_pin << _GPIO_USART_TXROUTE_PIN_SHIFT);
    GPIO->USARTROUTE[1].CLKROUTE =
        ((uint32_t)sck_port << _GPIO_USART_CLKROUTE_PORT_SHIFT) |
        ((uint32_t)sck_pin << _GPIO_USART_CLKROUTE_PIN_SHIFT);
    GPIO->USARTROUTE[1].ROUTEEN = GPIO_USART_ROUTEEN_TXPEN | GPIO_USART_ROUTEEN_CLKPEN;
    USART1->EN = USART_EN_EN;
    USART1->CMD = USART_CMD_MASTEREN | USART_CMD_TXEN | USART_CMD_RXDIS;

    s_io.hw_spi = true;
}

static void spi_write(const uint8_t *p, size_t n)
{
    if (s_io.hw_spi) {
        size_t i;
        for (i = 0; i < n; i++) {
            uint32_t guard = SPI_GUARD;
            while ((USART1->STATUS & USART_STATUS_TXBL) == 0u && guard-- > 0u) {
            }
            if (guard == 0u) {
                s_io.hw_spi = false;
                break;
            }
            USART1->TXDATA = (uint32_t)p[i];
        }
        if (s_io.hw_spi) {
            uint32_t guard = SPI_GUARD;
            while ((USART1->STATUS & USART_STATUS_TXC) == 0u && guard-- > 0u) {
            }
            if (guard == 0u) {
                s_io.hw_spi = false;
            }
        }
        if (s_io.hw_spi) {
            return;
        }
        /* Fell through: the rest of this frame goes out bit-banged. */
    }

    for (size_t i = 0; i < n; i++) {
        uint8_t uc = p[i];
        for (int b = 0; b < 8; b++) {
            pin_out(s_io.sck, false);
            pin_out(s_io.mosi, (uc & 0x80u) != 0u);
            pin_out(s_io.sck, true);
            uc = (uint8_t)(uc << 1);
        }
    }
    pin_out(s_io.sck, false);    /* CLK idles low: SPI mode 0 */
}

/* ----------------------------------------------------------- od_epd_io_t */

static void io_cmd(uint8_t c)
{
    pin_out(s_io.dc, false);
    pin_out(s_io.cs, false);
    spi_write(&c, 1u);
    pin_out(s_io.cs, true);
    pin_out(s_io.dc, true);
}

static void io_data(const uint8_t *d, size_t n)
{
    if (d == NULL || n == 0u) {
        return;
    }
    pin_out(s_io.cs, false);
    spi_write(d, n);
    pin_out(s_io.cs, true);
}

static void io_reset(uint32_t hold_ms)
{
    if (s_io.rst == PIN_UNUSED) {
        return;
    }
    if (hold_ms == 0u) {
        hold_ms = 1u;
    }
    pin_out(s_io.rst, true);
    sl_sleeptimer_delay_millisecond((uint16_t)hold_ms);
    pin_out(s_io.rst, false);
    sl_sleeptimer_delay_millisecond((uint16_t)hold_ms);
    pin_out(s_io.rst, true);
    sl_sleeptimer_delay_millisecond((uint16_t)hold_ms);
}

static bool io_busy_level(void)
{
    if (s_io.busy == PIN_UNUSED) {
        /* No pin: report the idle level so the driver's compare says "not
         * busy" regardless of which sense the config declared. */
        return s_io.busy_idle_high;
    }
    return pin_in(s_io.busy) != 0;
}

static void io_delay_ms(uint32_t ms)
{
    while (ms > 60000u) {         /* sleeptimer takes a uint16_t */
        sl_sleeptimer_delay_millisecond(60000u);
        ms -= 60000u;
    }
    if (ms) {
        sl_sleeptimer_delay_millisecond((uint16_t)ms);
    }
}

void od_epd_io_silabs_setup(uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                            uint8_t mosi, uint8_t sck, uint32_t speed,
                            bool busy_idle_high)
{
    s_io.dc = dc;
    s_io.rst = rst;
    s_io.busy = busy;
    s_io.cs = cs;
    s_io.mosi = mosi;
    s_io.sck = sck;
    s_io.speed = speed;
    s_io.busy_idle_high = busy_idle_high;
    s_io.hw_spi = false;

    CMU_ClockEnable(cmuClock_GPIO, true);
    pin_mode_out(dc, true);
    if (rst != PIN_UNUSED) {
        pin_mode_out(rst, true);
    }
    if (busy != PIN_UNUSED) {
        GPIO_Port_TypeDef port;
        uint8_t pin;
        if (decode(busy, &port, &pin)) {
            /* Hold the line at the declared idle level so a floating or
             * unconnected BUSY reads "not busy" instead of hanging the poll. */
            GPIO_PinModeSet(port, pin, gpioModeInputPull, busy_idle_high ? 1u : 0u);
        }
    }
    pin_mode_out(cs, true);
    if (speed == 0u) {
        pin_mode_out(mosi, false);
        pin_mode_out(sck, false);
    }
    hw_spi_init();
}

bool od_epd_io_silabs_pin(uint8_t raw_pin, uint8_t level)
{
    GPIO_Port_TypeDef port;
    uint8_t pin;

    if (!decode(raw_pin, &port, &pin) || level > 2u) {
        return false;
    }
    if (level == 2u) {
        /* Release to input/Hi-Z -- a bidirectional data line being handed to
         * the panel for a read-back must actually stop driving, not idle high. */
        GPIO_PinModeSet(port, pin, gpioModeInput, 0u);
    } else {
        GPIO_PinModeSet(port, pin, gpioModePushPull, level);
    }
    return true;
}

const od_epd_io_t *od_epd_io_silabs(void)
{
    static const od_epd_io_t io = {
        .cmd = io_cmd,
        .data = io_data,
        .reset = io_reset,
        .busy_level = io_busy_level,
        .delay_ms = io_delay_ms,
        /* The config-backed reads are filled in by the adapter: they are not a
         * property of this chip, they are a property of where the config is
         * stored. Same for set_pin -- the pin_ref SYMBOL table lives in the
         * config, so the adapter resolves it and calls od_epd_io_silabs_pin. */
        .seq_read = NULL,
        .lut_info = NULL,
        .lut_read = NULL,
        .set_pin = NULL,
        .cs_select = NULL,
    };
    return &io;
}
