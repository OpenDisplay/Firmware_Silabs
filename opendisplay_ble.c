#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_display.h"
#include "opendisplay_led.h"
#include "opendisplay_pipe.h"
#include "opendisplay_constants.h"
#include "app.h"
#include "app_assert.h"
#include "gatt_db.h"
#include "em_cmu.h"
#include "em_core.h"
#include "em_emu.h"
#include "em_gpio.h"
#include "em_iadc.h"
#include "em_system.h"
#include "sl_gpio.h"
#include "sl_sleeptimer.h"
#include "sl_udelay.h"
#include <stdio.h>
#include <string.h>

#define OPENDISPLAY_COMPANY_ID 0x2446u
#define MSD_PAYLOAD_LEN        16u
#define OD_NAME_PREFIX         "OD"
#ifndef OD_APP_VERSION
#define OD_APP_VERSION         0x0100u
#endif

#ifndef OPENDISPLAY_MAX_PIPE_LEN
#define OPENDISPLAY_MAX_PIPE_LEN 244u
#endif

/* BLE adv interval in units of 0.625 ms (used when not connected / undirected adv). */
#define OD_ADV_INTERVAL_IDLE_SLOTS 1600u
#define OD_ADV_INTERVAL_BOOST_MIN    32u
#define OD_ADV_INTERVAL_BOOST_MAX    48u
#define OD_ADV_BOOST_MS              3000u
#define OD_MSD_UPDATE_INTERVAL_MS  30000u
#define BUTTON_ID_MASK             0x07u
#define PRESS_COUNT_MASK           0x0Fu
#define PRESS_COUNT_SHIFT          3u
#define BUTTON_STATE_SHIFT         7u
#define OD_MAX_CONNECTION_MS       300000u

/* When system_config.pwr_pin is 0xFF, drive this pin HIGH as display rail enable (PA0). Set to 0xFF to disable. */
#ifndef OD_FALLBACK_DISPLAY_PWR_PIN
#define OD_FALLBACK_DISPLAY_PWR_PIN 0x00u
#endif

/* OD_TNB132M_BOOT_PROBE=1 (e.g. -DOD_TNB132M_BOOT_PROBE=1): read the phone-visible
 * NDEF via TNB132M at boot and log it over RTT. Type-3 AI lives at I2C 0x48 sub=0;
 * NDEF data blocks are byte-offset reads at I2C 0x40 sub=0x10/0x20/... The tag
 * caches only AI+Nbr blocks per anchor (Nbr=2 here), so each data block read is
 * preceded by a fresh 0x48 sub=0 anchor.
 *
 * OD_TNB132M_WRITE_NDEF=1 (requires BOOT_PROBE): before the read, write a Well-Known
 * Text record containing OD_TNB132M_NDEF_TEXT (UTF-8, lang=en). Max 25 chars so the
 * record fits in AI + 2 data blocks (Nbr=2 cache). Leaves WriteFlag=0x00 and RWFlag
 * untouched so the NFC Forum "read/write" bit keeps its factory value.
 *
 * Power is still removed at end of od_nfc_init_sequence(). */
#ifndef OD_TNB132M_BOOT_PROBE
#define OD_TNB132M_BOOT_PROBE 0
#endif
#ifndef OD_TNB132M_WRITE_NDEF
#define OD_TNB132M_WRITE_NDEF 0
#endif
#ifndef OD_TNB132M_NDEF_TEXT
#define OD_TNB132M_NDEF_TEXT "OpenDisplay"
#endif

static uint8_t msd_payload[MSD_PAYLOAD_LEN];
static uint8_t msd_loop_counter;
static uint8_t dynamic_return[11];
static uint8_t reboot_flag = 1u;
static uint8_t connection_requested = 0u;

static uint16_t g_od_pipe_char;
static uint8_t g_connection = 0xFFu;
static uint8_t s_adv_handle = 0xFFu;
static char s_dev_name[16];
static struct GlobalConfig s_od_global_config;
static uint32_t s_last_msd_refresh_ms;
static uint32_t s_adv_boost_until_ms;
static bool s_pending_dfu;
static bool s_pending_deep_sleep;
static uint32_t s_connection_open_ms;
static bool s_connection_timeout_close_requested;
static uint32_t s_last_batt_measure_ms;
static uint16_t s_batt_voltage_mv_cache;

static GPIO_Port_TypeDef s_od_flash_mosi_port = gpioPortA;
static uint8_t s_od_flash_mosi_pin = 0u;
static GPIO_Port_TypeDef s_od_flash_sck_port = gpioPortA;
static uint8_t s_od_flash_sck_pin = 0u;
static GPIO_Port_TypeDef s_od_flash_cs_port = gpioPortA;
static uint8_t s_od_flash_cs_pin = 0u;
static bool s_od_flash_enabled;

/* NFC / bit-bang I2C lines: initialized only from explicit NFC config packet. */
static GPIO_Port_TypeDef s_od_nfc_scl_port = gpioPortA;
static uint8_t s_od_nfc_scl_pin = 0u;
static GPIO_Port_TypeDef s_od_nfc_sda_port = gpioPortA;
static uint8_t s_od_nfc_sda_pin = 0u;
static GPIO_Port_TypeDef s_od_nfc_pwr_port = gpioPortA;
static uint8_t s_od_nfc_pwr_pin = 0u;
static bool s_od_nfc_has_pwr_pin;
static bool s_od_nfc_enabled;
static uint8_t s_od_nfc_ic_type;
static uint8_t s_od_nfc_power_on_delay_ms = 40u;

typedef struct {
  bool enabled;
  uint8_t pin_cfg;
  uint8_t mode;
  bool active_high;
  uint8_t debounce_ms;
  uint8_t adv_button_byte_index;
  uint8_t adv_button_id;
  uint8_t state;
  uint32_t last_change_ms;
  int32_t int_no;
  int32_t em4_int_no;
  volatile bool irq_pending;
} od_nfc_field_detect_t;
static od_nfc_field_detect_t s_od_nfc_field_detect;
static void od_nfc_field_detect_irq_callback(uint8_t int_no, void *context);
static uint32_t s_od_nfc_field_scan_counter;
static uint8_t s_od_nfc_write_blocks[512];
static uint8_t s_od_nfc_read_data[128];
static uint8_t s_od_nfc_read_throwaway[16];

typedef struct {
  bool active;
  uint8_t pin_cfg;
  int32_t int_no;
  int32_t em4_int_no;
  uint8_t button_id;
  uint8_t byte_index;
  bool inverted;
  uint8_t press_count;
  uint8_t current_state;
} od_button_state_t;
static od_button_state_t s_buttons[32];
static uint8_t s_button_count;
static volatile bool s_button_msd_dirty;

static void build_and_apply_adv(uint8_t adv_set, const char *name, bool quick);
static void od_boost_advertising(uint32_t now_ms);
static void od_apply_advertising_timing(uint8_t adv_handle, uint32_t now_ms);

#if defined(__GNUC__)
extern uint32_t __ResetReasonStart__;
#endif

typedef struct {
  uint16_t reason;
  uint16_t signature;
} od_bootloader_reset_cause_t;

#define OD_BTL_RESET_REASON_BOOTLOAD    0x0202u
#define OD_BTL_RESET_SIGNATURE_VALID    0xF00Fu

static void od_enter_gecko_bootloader(void)
{
#if defined(__GNUC__)
  uintptr_t base = (uintptr_t)&__ResetReasonStart__;
#else
  uintptr_t base = 0x20000000u;
#endif
  od_bootloader_reset_cause_t *cause = (od_bootloader_reset_cause_t *)base;
  cause->reason = OD_BTL_RESET_REASON_BOOTLOAD;
  cause->signature = OD_BTL_RESET_SIGNATURE_VALID;
  NVIC_SystemReset();
}

static bool od_pin_decode(uint8_t v, GPIO_Port_TypeDef *port_out, uint8_t *pin_out)
{
  if (v == 0xFFu) {
    return false;
  }
  unsigned pr = (unsigned)(v >> 4) & 0x0Fu;
  unsigned pn = (unsigned)(v & 0x0Fu);
  if (pr > (unsigned)GPIO_PORT_MAX || pn > 15u) {
    return false;
  }
  *port_out = (GPIO_Port_TypeDef)(gpioPortA + pr);
  *pin_out = (uint8_t)pn;
  return true;
}

static uint8_t od_read_button_pin(uint8_t pin_cfg)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (!od_pin_decode(pin_cfg, &port, &pin)) {
    return 0u;
  }
  return (uint8_t)(GPIO_PinInGet(port, pin) != 0);
}

static void od_setup_button_pin(uint8_t pin_cfg, bool pullup, bool pulldown)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (!od_pin_decode(pin_cfg, &port, &pin)) {
    return;
  }
  if (pullup) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 1);
  } else if (pulldown) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 0);
  } else {
    GPIO_PinModeSet(port, pin, gpioModeInput, 0);
  }
}

static void od_button_irq_callback(uint8_t int_no, void *context)
{
  od_button_state_t *st = (od_button_state_t *)context;
  uint8_t pin_state;
  uint8_t logical_state;
  uint8_t encoded;

  (void)int_no;
  if (st == NULL || !st->active) {
    return;
  }
  pin_state = od_read_button_pin(st->pin_cfg);
  logical_state = (st->inverted ? (pin_state == 0u) : (pin_state != 0u)) ? 1u : 0u;
  if (logical_state == st->current_state) {
    return;
  }
  st->current_state = logical_state;
  if (logical_state != 0u) {
    st->press_count = (uint8_t)((st->press_count + 1u) & PRESS_COUNT_MASK);
  }
  encoded = (uint8_t)((st->button_id & BUTTON_ID_MASK)
            | ((st->press_count & PRESS_COUNT_MASK) << PRESS_COUNT_SHIFT)
            | ((st->current_state & 0x01u) << BUTTON_STATE_SHIFT));
  dynamic_return[st->byte_index] = encoded;
  s_button_msd_dirty = true;
  app_proceed();
}

static void od_buttons_deinit_interrupts(void)
{
  uint8_t i;
  for (i = 0u; i < s_button_count; i++) {
    od_button_state_t *st = &s_buttons[i];
    if (st->active && st->int_no >= 0) {
      (void)sl_gpio_deconfigure_external_interrupt(st->int_no);
      st->int_no = -1;
    }
    if (st->active && st->em4_int_no >= 0) {
      (void)sl_gpio_deconfigure_wakeup_em4_interrupt(st->em4_int_no);
      st->em4_int_no = -1;
    }
  }
}

static void od_buttons_init_from_config(void)
{
  const struct GlobalConfig *cfg = &s_od_global_config;
  uint8_t i;
  uint8_t bi;

  od_buttons_deinit_interrupts();
  s_button_count = 0u;
  s_button_msd_dirty = false;
  memset(s_buttons, 0, sizeof(s_buttons));
  printf("[OD][BTN] init: binary_input_count=%u\r\n", (unsigned)cfg->binary_input_count);
  for (i = 0; i < cfg->binary_input_count; i++) {
    const struct BinaryInputs *input = &cfg->binary_inputs[i];
    const uint8_t local_pins[8] = {
      input->reserved_pin_1, input->reserved_pin_2, input->reserved_pin_3, input->reserved_pin_4,
      input->reserved_pin_5, input->reserved_pin_6, input->reserved_pin_7, input->reserved_pin_8
    };
    if (input->input_type != 1u || input->button_data_byte_index >= sizeof(dynamic_return)) {
      printf("[OD][BTN] skip instance=%u input_type=%u byte_index=%u\r\n",
             (unsigned)input->instance_number,
             (unsigned)input->input_type,
             (unsigned)input->button_data_byte_index);
      continue;
    }
    printf("[OD][BTN] instance=%u byte_index=%u invert=0x%02X pullup=0x%02X pulldown=0x%02X\r\n",
           (unsigned)input->instance_number,
           (unsigned)input->button_data_byte_index,
           (unsigned)input->invert,
           (unsigned)input->pullups,
           (unsigned)input->pulldowns);
    for (bi = 0u; bi < 8u && s_button_count < (uint8_t)(sizeof(s_buttons) / sizeof(s_buttons[0])); bi++) {
      od_button_state_t *st;
      bool pullup;
      bool pulldown;
      bool pressed;
      uint8_t pin_state_raw;
      uint8_t pin_cfg = local_pins[bi];
      bool pin_used = (input->input_flags & (uint8_t)(1u << bi)) != 0u;
      if (!pin_used) {
        continue;
      }
      if (pin_cfg == GPIO_PIN_UNUSED) {
        printf("[OD][BTN] skip used slot=%u pin=0xFF (not configured)\r\n", (unsigned)bi);
        continue;
      }
      st = &s_buttons[s_button_count];
      pullup = (input->pullups & (uint8_t)(1u << bi)) != 0u;
      pulldown = (input->pulldowns & (uint8_t)(1u << bi)) != 0u;
      od_setup_button_pin(pin_cfg, pullup, pulldown);
      st->active = true;
      st->pin_cfg = pin_cfg;
      st->button_id = (uint8_t)(((input->instance_number * 8u) + bi) & BUTTON_ID_MASK);
      st->byte_index = input->button_data_byte_index;
      st->inverted = (input->invert & (uint8_t)(1u << bi)) != 0u;
      st->int_no = -1;
      st->em4_int_no = -1;
      pin_state_raw = od_read_button_pin(pin_cfg);
      pressed = st->inverted ? (pin_state_raw == 0u) : (pin_state_raw != 0u);
      st->current_state = pressed ? 1u : 0u;
      dynamic_return[st->byte_index] =
        (uint8_t)((st->button_id & BUTTON_ID_MASK) | ((st->current_state & 0x01u) << BUTTON_STATE_SHIFT));
      printf("[OD][BTN] arm idx=%u slot=%u pin=0x%02X button_id=%u pin_state=%s init_state=%u\r\n",
             (unsigned)s_button_count,
             (unsigned)bi,
             (unsigned)pin_cfg,
             (unsigned)st->button_id,
             pin_state_raw ? "HIGH" : "LOW",
             (unsigned)st->current_state);
      {
        sl_gpio_t gpio = {
          .port = (sl_gpio_port_t)(pin_cfg >> 4),
          .pin = (pin_cfg & 0x0Fu)
        };
        int32_t int_no = -1;
        if (sl_gpio_configure_external_interrupt(&gpio,
                                                 &int_no,
                                                 SL_GPIO_INTERRUPT_RISING_FALLING_EDGE,
                                                 od_button_irq_callback,
                                                 st) == SL_STATUS_OK) {
          st->int_no = int_no;
          printf("[OD][BTN] irq ok idx=%u int_no=%ld\r\n",
                 (unsigned)s_button_count, (long)st->int_no);
        } else {
          printf("[OD][BTN] irq fail idx=%u pin=0x%02X\r\n",
                 (unsigned)s_button_count, (unsigned)pin_cfg);
        }
      }
      s_button_count++;
    }
  }
  printf("[OD][BTN] init done active_buttons=%u\r\n", (unsigned)s_button_count);
}

static void od_publish_button_msd(uint8_t adv_handle, uint32_t now_ms)
{
  if (!s_button_msd_dirty || adv_handle == 0xFFu || g_connection != 0xFFu) {
    return;
  }
  s_button_msd_dirty = false;
  od_boost_advertising(now_ms);
  (void)sl_bt_advertiser_stop(adv_handle);
  build_and_apply_adv(adv_handle, s_dev_name, true);
  od_apply_advertising_timing(adv_handle, now_ms);
  app_assert_status(sl_bt_legacy_advertiser_start(adv_handle, sl_bt_legacy_advertiser_connectable));
}

static void od_buttons_arm_em4_wakeup(void)
{
  uint8_t i;
  for (i = 0u; i < s_button_count; i++) {
    od_button_state_t *st = &s_buttons[i];
    sl_gpio_t gpio;
    int32_t em4_int_no = SL_GPIO_INTERRUPT_UNAVAILABLE;
    sl_status_t sc;
    bool wake_polarity_high;

    if (!st->active) {
      continue;
    }
    gpio.port = (sl_gpio_port_t)(st->pin_cfg >> 4);
    gpio.pin = (st->pin_cfg & 0x0Fu);
    wake_polarity_high = !st->inverted;
    sc = sl_gpio_configure_wakeup_em4_interrupt(&gpio,
                                                &em4_int_no,
                                                wake_polarity_high,
                                                od_button_irq_callback,
                                                st);
    if (sc == SL_STATUS_OK) {
      st->em4_int_no = em4_int_no;
      printf("[OD][BTN] EM4 wake armed pin=0x%02X em4_int=%ld polarity=%s\r\n",
             (unsigned)st->pin_cfg,
             (long)em4_int_no,
             wake_polarity_high ? "high" : "low");
    } else {
      printf("[OD][BTN] EM4 wake arm failed pin=0x%02X sc=0x%04lX\r\n",
             (unsigned)st->pin_cfg, (unsigned long)sc);
    }
  }
}

static void od_nfc_i2c_start(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                             GPIO_Port_TypeDef sda_port, uint8_t sda_pin)
{
  GPIO_PinOutSet(sda_port, sda_pin);
  GPIO_PinOutSet(scl_port, scl_pin);
  sl_udelay_wait(2);
  GPIO_PinOutClear(sda_port, sda_pin);
  sl_udelay_wait(2);
  GPIO_PinOutClear(scl_port, scl_pin);
}

static void od_nfc_i2c_stop(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                            GPIO_Port_TypeDef sda_port, uint8_t sda_pin)
{
  GPIO_PinOutClear(sda_port, sda_pin);
  sl_udelay_wait(2);
  GPIO_PinOutSet(scl_port, scl_pin);
  sl_udelay_wait(2);
  GPIO_PinOutSet(sda_port, sda_pin);
  sl_udelay_wait(2);
}

static bool od_nfc_i2c_write_byte(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                                  GPIO_Port_TypeDef sda_port, uint8_t sda_pin,
                                  uint8_t byte)
{
  for (uint8_t i = 0; i < 8u; i++) {
    if ((byte & 0x80u) != 0u) {
      GPIO_PinOutSet(sda_port, sda_pin);
    } else {
      GPIO_PinOutClear(sda_port, sda_pin);
    }
    byte <<= 1;
    sl_udelay_wait(1);
    GPIO_PinOutSet(scl_port, scl_pin);
    sl_udelay_wait(2);
    GPIO_PinOutClear(scl_port, scl_pin);
  }

  GPIO_PinModeSet(sda_port, sda_pin, gpioModeInputPull, 1);
  sl_udelay_wait(1);
  GPIO_PinOutSet(scl_port, scl_pin);
  sl_udelay_wait(2);
  bool ack = (GPIO_PinInGet(sda_port, sda_pin) == 0);
  GPIO_PinOutClear(scl_port, scl_pin);
  GPIO_PinModeSet(sda_port, sda_pin, gpioModeWiredAndFilter, 0);
  return ack;
}

static uint8_t od_nfc_i2c_read_byte(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                                    GPIO_Port_TypeDef sda_port, uint8_t sda_pin, bool ack)
{
  uint8_t value = 0;
  GPIO_PinModeSet(sda_port, sda_pin, gpioModeInputPull, 1);
  for (uint8_t i = 0; i < 8u; i++) {
    value <<= 1;
    GPIO_PinOutSet(scl_port, scl_pin);
    sl_udelay_wait(2);
    if (GPIO_PinInGet(sda_port, sda_pin) != 0) {
      value |= 1u;
    }
    GPIO_PinOutClear(scl_port, scl_pin);
    sl_udelay_wait(1);
  }

  GPIO_PinModeSet(sda_port, sda_pin, gpioModeWiredAndFilter, 0);
  if (ack) {
    GPIO_PinOutClear(sda_port, sda_pin);
  } else {
    GPIO_PinOutSet(sda_port, sda_pin);
  }
  sl_udelay_wait(1);
  GPIO_PinOutSet(scl_port, scl_pin);
  sl_udelay_wait(2);
  GPIO_PinOutClear(scl_port, scl_pin);
  GPIO_PinOutSet(sda_port, sda_pin);
  return value;
}

static bool od_nfc_type3_paged_block_read16(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                                            GPIO_Port_TypeDef sda_port, uint8_t sda_pin,
                                            uint8_t dev7, uint8_t sub, uint8_t *out16)
{
  bool a;
  if (out16 == NULL) {
    return false;
  }
  od_nfc_i2c_start(scl_port, scl_pin, sda_port, sda_pin);
  a = od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, (uint8_t)(dev7 << 1));
  if (!a) {
    od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
    return false;
  }
  a = od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, sub);
  if (!a) {
    od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
    return false;
  }
  od_nfc_i2c_start(scl_port, scl_pin, sda_port, sda_pin);
  a = od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, (uint8_t)((dev7 << 1u) | 1u));
  if (!a) {
    od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
    return false;
  }
  for (uint8_t i = 0; i < 16u; i++) {
    out16[i] = od_nfc_i2c_read_byte(scl_port, scl_pin, sda_port, sda_pin, i < 15u);
  }
  od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
  return true;
}

#if OD_TNB132M_BOOT_PROBE

/* Type-3 NDEF read: 0x48 sub=0 returns the NFC Forum Attribute Information
 * (Ver|Nbr|Nbw|Nmaxb|rsv[4]|WriteFlag|RWFlag|Ln[3]|Sum[2]); NDEF data blocks are
 * byte-offset reads at dev=0x40 (sub=0x10 for blk1, 0x20 for blk2, ..). TNB132M
 * caches only AI + Nbr data blocks (Nbr=2 here) per anchor, so re-anchor via
 * 0x48 sub=0 before each data read so block 3+ still lands in the cache window.
 * Decodes the first record's TNF/type/payload so RTT matches phone output. */
static void od_nfc_boot_dump_ndef_t3(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                                     GPIO_Port_TypeDef sda_port, uint8_t sda_pin)
{
  uint8_t ai[16];
  uint8_t data[64];
  uint32_t ln;
  uint16_t sum_calc, sum_read;
  unsigned need_blocks;

  if (!od_nfc_type3_paged_block_read16(scl_port, scl_pin, sda_port, sda_pin, 0x48u, 0x00u, ai)) {
    printf("[OD] NFC ndef AI @0x48 sub=0: NACK\r\n");
    return;
  }
  if (ai[0] != 0x10u) {
    printf("[OD] NFC ndef AI ver=%02x (not 1.x) — skip decode\r\n", ai[0]);
    return;
  }
  ln = ((uint32_t)ai[11] << 16) | ((uint32_t)ai[12] << 8) | (uint32_t)ai[13];
  sum_read = (uint16_t)(((uint16_t)ai[14] << 8) | (uint16_t)ai[15]);
  sum_calc = 0;
  for (unsigned i = 0; i < 14u; i++) {
    sum_calc = (uint16_t)(sum_calc + ai[i]);
  }
  printf("[OD] NFC ndef AI Ver=%u.%u Nbr=%u Nbw=%u Nmaxb=%u RWFlag=%02x Ln=%lu Sum=%04x (calc %04x %s)\r\n",
         (unsigned)(ai[0] >> 4), (unsigned)(ai[0] & 0x0Fu), (unsigned)ai[1], (unsigned)ai[2],
         (unsigned)(((uint16_t)ai[3] << 8) | ai[4]), ai[10], (unsigned long)ln, (unsigned)sum_read,
         (unsigned)sum_calc, sum_calc == sum_read ? "OK" : "BAD");
  if (ln == 0u || ln > sizeof(data)) {
    printf("[OD] NFC ndef Ln=%lu out of range (max %u)\r\n",
           (unsigned long)ln, (unsigned)sizeof(data));
    return;
  }
  need_blocks = (unsigned)((ln + 15u) / 16u);
  for (unsigned b = 0; b < need_blocks; b++) {
    uint8_t byte_off = (uint8_t)(0x10u + b * 0x10u);
    uint8_t throwaway[16];
    (void)od_nfc_type3_paged_block_read16(scl_port, scl_pin, sda_port, sda_pin, 0x48u, 0x00u,
                                          throwaway);
    sl_udelay_wait(500);
    if (!od_nfc_type3_paged_block_read16(scl_port, scl_pin, sda_port, sda_pin, 0x40u,
                                         byte_off, &data[b * 16u])) {
      printf("[OD] NFC ndef blk%u @0x40 sub=%02x: NACK\r\n", b + 1u, (unsigned)byte_off);
      return;
    }
    printf("[OD] NFC ndef blk%u %04x:", b + 1u, b * 16u);
    for (unsigned j = 0; j < 16u; j++) {
      printf(" %02x", data[b * 16u + j]);
    }
    printf("\r\n");
    sl_udelay_wait(200);
  }
  {
    uint8_t tnf0 = data[0] & 0x07u;
    if (ln < 4u || tnf0 == 0u || tnf0 >= 6u) {
      printf("[OD] NFC ndef record too short or bad TNF (ln=%lu hdr=%02x tnf=%u)\r\n",
             (unsigned long)ln, data[0], tnf0);
      return;
    }
  }
  {
    uint8_t hdr = data[0];
    uint8_t tnf = hdr & 0x07u;
    bool sr = (hdr & 0x10u) != 0;
    bool il = (hdr & 0x08u) != 0;
    unsigned tlen = data[1];
    unsigned plen;
    unsigned off;
    unsigned ilen = 0;
    if (sr) {
      plen = data[2];
      off = 3u;
    } else {
      if (ln < 6u) {
        printf("[OD] NFC ndef long-format truncated\r\n");
        return;
      }
      plen = ((unsigned)data[2] << 24) | ((unsigned)data[3] << 16) | ((unsigned)data[4] << 8) | data[5];
      off = 6u;
    }
    if (il) {
      if (ln < off + 1u) {
        printf("[OD] NFC ndef IL truncated\r\n");
        return;
      }
      ilen = data[off];
      off += 1u;
    }
    if (off + tlen + ilen + plen > ln) {
      printf("[OD] NFC ndef fields exceed Ln (off=%u tlen=%u il=%u plen=%u ln=%lu)\r\n",
             off, tlen, ilen, plen, (unsigned long)ln);
      return;
    }
    printf("[OD] NFC ndef rec TNF=%u SR=%u IL=%u tlen=%u plen=%u type=\"%.*s\"\r\n",
           tnf, (unsigned)sr, (unsigned)il, tlen, plen, tlen, (const char *)&data[off]);
    off += tlen + ilen;
    if (tnf == 0x01u && tlen == 1u && data[off - tlen - ilen] == 'T' && plen >= 1u) {
      unsigned stat = data[off];
      unsigned lang_len = stat & 0x3Fu;
      if (1u + lang_len <= plen) {
        printf("[OD] NFC ndef Text lang=\"%.*s\" utf%u text=\"%.*s\"\r\n",
               lang_len, (const char *)&data[off + 1u], (stat & 0x80u) ? 16u : 8u,
               (unsigned)(plen - 1u - lang_len), (const char *)&data[off + 1u + lang_len]);
      }
    }
  }
}

#endif /* OD_TNB132M_BOOT_PROBE */

/* Type-3 16-byte block write: START, dev+W, sub, 16 data bytes, STOP. Mirrors the
 * paged read on the write side — AI goes to dev=0x48 sub=0, NDEF data blocks go to
 * dev=0x40 sub=0x10/0x20/... (same byte-offset mapping as the read path). */
static bool od_nfc_type3_paged_block_write16(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                                             GPIO_Port_TypeDef sda_port, uint8_t sda_pin,
                                             uint8_t dev7, uint8_t sub, const uint8_t *in16)
{
  bool a;
  if (in16 == NULL) {
    return false;
  }
  od_nfc_i2c_start(scl_port, scl_pin, sda_port, sda_pin);
  a = od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, (uint8_t)(dev7 << 1));
  if (!a) {
    od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
    return false;
  }
  a = od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, sub);
  if (!a) {
    od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
    return false;
  }
  for (uint8_t i = 0; i < 16u; i++) {
    a = od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, in16[i]);
    if (!a) {
      od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
      return false;
    }
  }
  od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
  return true;
}

#if OD_TNB132M_BOOT_PROBE
#if OD_TNB132M_WRITE_NDEF
/* Replace the NDEF with a UTF-8 Text record (lang="en"). Max 25 chars so the whole
 * record fits in AI + 2 data blocks (TNB132M Nbr=2 cache limit). Reads the current
 * AI first to preserve RWFlag so the NFC Forum read/write bit keeps its factory
 * value — we only modify Ln and checksum. */
static bool od_nfc_write_ndef_text_en(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                                      GPIO_Port_TypeDef sda_port, uint8_t sda_pin,
                                      const char *text)
{
  uint8_t ai[16] = { 0 };
  uint8_t blocks[32] = { 0 };
  uint8_t cur_ai[16];
  size_t tlen = (text != NULL) ? strlen(text) : 0u;
  unsigned plen;
  unsigned recln;
  uint16_t sum;
  unsigned i;
  unsigned need_blocks;

  if (tlen == 0u || tlen > 25u) {
    printf("[OD] NFC write: text len %u out of range (1..25)\r\n", (unsigned)tlen);
    return false;
  }
  plen = 1u + 2u + (unsigned)tlen;
  recln = 4u + plen;

  blocks[0] = 0xD1u;
  blocks[1] = 0x01u;
  blocks[2] = (uint8_t)plen;
  blocks[3] = 0x54u;
  blocks[4] = 0x02u;
  blocks[5] = (uint8_t)'e';
  blocks[6] = (uint8_t)'n';
  for (i = 0; i < tlen; i++) {
    blocks[7u + i] = (uint8_t)text[i];
  }

  ai[0] = 0x10u;
  ai[1] = 0x02u;
  ai[2] = 0x01u;
  ai[3] = 0x00u;
  ai[4] = 0x3Cu;
  ai[11] = (uint8_t)((recln >> 16) & 0xFFu);
  ai[12] = (uint8_t)((recln >> 8) & 0xFFu);
  ai[13] = (uint8_t)(recln & 0xFFu);
  if (od_nfc_type3_paged_block_read16(scl_port, scl_pin, sda_port, sda_pin, 0x48u, 0x00u, cur_ai)
      && cur_ai[0] == 0x10u) {
    ai[10] = cur_ai[10];
  }
  sum = 0;
  for (i = 0; i < 14u; i++) {
    sum = (uint16_t)(sum + ai[i]);
  }
  ai[14] = (uint8_t)(sum >> 8);
  ai[15] = (uint8_t)(sum & 0xFFu);

  need_blocks = (recln + 15u) / 16u;
  printf("[OD] NFC write: Ln=%u RWFlag=%02x text=\"%s\" (%u blk)\r\n",
         recln, ai[10], text, need_blocks);
  if (!od_nfc_type3_paged_block_write16(scl_port, scl_pin, sda_port, sda_pin, 0x48u, 0x00u, ai)) {
    printf("[OD] NFC write: AI @0x48 sub=0 NACK\r\n");
    return false;
  }
  sl_udelay_wait(10000);
  for (i = 0; i < need_blocks; i++) {
    uint8_t byte_off = (uint8_t)(0x10u + i * 0x10u);
    if (!od_nfc_type3_paged_block_write16(scl_port, scl_pin, sda_port, sda_pin, 0x40u, byte_off,
                                          &blocks[i * 16u])) {
      printf("[OD] NFC write: blk%u @0x40 sub=%02x NACK\r\n", i + 1u, (unsigned)byte_off);
      return false;
    }
    sl_udelay_wait(10000);
  }
  printf("[OD] NFC write: OK\r\n");
  return true;
}
#endif /* OD_TNB132M_WRITE_NDEF */
#endif /* OD_TNB132M_BOOT_PROBE */

/* Wake TNB132M host I2C + open the byte-offset Type-3 data window at 0x40.
 * After EEPROM/block writes via 0x48/0x40, callers must run this again or
 * 0x48 sub=0 still returns updated AI while 0x40 sub=0x10 reads all 0xFF (RF /
 * tag RAM cache repopulates; MCU window must be re-opened). */
static void od_nfc_tnb132m_prime_type3(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                                      GPIO_Port_TypeDef sda_port, uint8_t sda_pin)
{
  uint8_t sink = 0;

  od_nfc_i2c_start(scl_port, scl_pin, sda_port, sda_pin);
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, (uint8_t)(0x30u << 1));
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, 0x21u);
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, 0x04u);
  od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);

  od_nfc_i2c_start(scl_port, scl_pin, sda_port, sda_pin);
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, (uint8_t)(0x30u << 1));
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, 0x25u);
  od_nfc_i2c_start(scl_port, scl_pin, sda_port, sda_pin);
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, (uint8_t)((0x30u << 1) | 1u));
  sink = od_nfc_i2c_read_byte(scl_port, scl_pin, sda_port, sda_pin, false);
  (void)sink;
  od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);

  sl_udelay_wait(20000);

  od_nfc_i2c_start(scl_port, scl_pin, sda_port, sda_pin);
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, (uint8_t)(0x43u << 1));
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, 0x30u);
  od_nfc_i2c_start(scl_port, scl_pin, sda_port, sda_pin);
  (void)od_nfc_i2c_write_byte(scl_port, scl_pin, sda_port, sda_pin, (uint8_t)((0x43u << 1) | 1u));
  for (uint8_t i = 0; i < 16u; i++) {
    sink = od_nfc_i2c_read_byte(scl_port, scl_pin, sda_port, sda_pin, i < 15u);
  }
  (void)sink;
  od_nfc_i2c_stop(scl_port, scl_pin, sda_port, sda_pin);
}

static void od_nfc_field_detect_init_from_config(const struct NfcConfig *nfc_cfg)
{
  memset(&s_od_nfc_field_detect, 0, sizeof(s_od_nfc_field_detect));
  s_od_nfc_field_detect.int_no = -1;
  s_od_nfc_field_detect.em4_int_no = -1;
  s_od_nfc_field_scan_counter = 0u;
  if (nfc_cfg == NULL) {
    return;
  }
  if (nfc_cfg->field_detect_mode == 0u || nfc_cfg->field_detect_pin == GPIO_PIN_UNUSED) {
    return;
  }
  s_od_nfc_field_detect.enabled = true;
  s_od_nfc_field_detect.pin_cfg = nfc_cfg->field_detect_pin;
  s_od_nfc_field_detect.mode = nfc_cfg->field_detect_mode;
  s_od_nfc_field_detect.active_high = (nfc_cfg->field_detect_active != 0u);
  s_od_nfc_field_detect.debounce_ms = nfc_cfg->field_detect_debounce_ms;
  s_od_nfc_field_detect.adv_button_byte_index = nfc_cfg->adv_button_byte_index;
  s_od_nfc_field_detect.adv_button_id = (uint8_t)(nfc_cfg->adv_button_button_id & BUTTON_ID_MASK);
  od_setup_button_pin(s_od_nfc_field_detect.pin_cfg, false, false);
  if (s_od_nfc_field_detect.mode == 2u) {
    sl_gpio_t gpio = {
      .port = (sl_gpio_port_t)(s_od_nfc_field_detect.pin_cfg >> 4),
      .pin = (s_od_nfc_field_detect.pin_cfg & 0x0Fu)
    };
    if (sl_gpio_configure_external_interrupt(&gpio,
                                             &s_od_nfc_field_detect.int_no,
                                             SL_GPIO_INTERRUPT_RISING_FALLING_EDGE,
                                             od_nfc_field_detect_irq_callback,
                                             &s_od_nfc_field_detect) == SL_STATUS_OK) {
      printf("[OD][NFC] field-detect IRQ enabled int_no=%ld\r\n", (long)s_od_nfc_field_detect.int_no);
    } else {
      s_od_nfc_field_detect.int_no = -1;
      printf("[OD][NFC] field-detect IRQ setup failed; falling back to polling\r\n");
    }
  }
  {
    uint8_t pin_state = od_read_button_pin(s_od_nfc_field_detect.pin_cfg);
    uint8_t logical = (s_od_nfc_field_detect.active_high ? (pin_state != 0u) : (pin_state == 0u)) ? 1u : 0u;
    /* Start from 0 so the first post-init sample can create a 0->1 scan edge. */
    s_od_nfc_field_detect.state = 0u;
    if (s_od_nfc_field_detect.adv_button_byte_index < sizeof(dynamic_return)) {
      dynamic_return[s_od_nfc_field_detect.adv_button_byte_index] =
        (uint8_t)((s_od_nfc_field_detect.state & 0x01u)
                  | (((s_od_nfc_field_scan_counter & 0x7Fu) << 1) & 0xFEu));
    }
    (void)logical;
  }
  printf("[OD][NFC] field-detect pin=0x%02X state=%u byte_index=%u button_id=%u\r\n",
         (unsigned)s_od_nfc_field_detect.pin_cfg,
         (unsigned)s_od_nfc_field_detect.state,
         (unsigned)s_od_nfc_field_detect.adv_button_byte_index,
         (unsigned)s_od_nfc_field_detect.adv_button_id);
}

static bool od_nfc_field_detect_refresh(uint32_t now_ms, bool force_log)
{
  uint8_t pin_state;
  uint8_t logical;
  uint8_t prev_state;
  bool scan_event = false;

  if (!s_od_nfc_field_detect.enabled) {
    return false;
  }
  if (s_od_nfc_field_detect.adv_button_byte_index >= sizeof(dynamic_return)) {
    return false;
  }

  pin_state = od_read_button_pin(s_od_nfc_field_detect.pin_cfg);
  logical = (s_od_nfc_field_detect.active_high ? (pin_state != 0u) : (pin_state == 0u)) ? 1u : 0u;
  prev_state = s_od_nfc_field_detect.state;

  if (!force_log && logical == prev_state) {
    return false;
  }
  if (!force_log
      && s_od_nfc_field_detect.debounce_ms != 0u
      && (now_ms - s_od_nfc_field_detect.last_change_ms) < s_od_nfc_field_detect.debounce_ms) {
    return false;
  }

  s_od_nfc_field_detect.last_change_ms = now_ms;
  s_od_nfc_field_detect.state = logical;
  if (logical != 0u && prev_state == 0u) {
    s_od_nfc_field_scan_counter = (uint32_t)((s_od_nfc_field_scan_counter + 1u) & 0x7Fu);
    scan_event = true;
  }
  dynamic_return[s_od_nfc_field_detect.adv_button_byte_index] =
    (uint8_t)((logical & 0x01u)
              | (((s_od_nfc_field_scan_counter & 0x7Fu) << 1) & 0xFEu));

  if (force_log) {
    printf("[OD][NFC] field-detect sample pin=0x%02X raw=%u state=%u ctr7=%lu adv=0x%02X (active_%s)\r\n",
           (unsigned)s_od_nfc_field_detect.pin_cfg,
           (unsigned)pin_state,
           (unsigned)logical,
           (unsigned long)s_od_nfc_field_scan_counter,
           (unsigned)dynamic_return[s_od_nfc_field_detect.adv_button_byte_index],
           s_od_nfc_field_detect.active_high ? "high" : "low");
  } else {
    printf("[OD][NFC] field-detect pin=0x%02X raw=%u state %u->%u ctr7=%lu adv=0x%02X%s (active_%s)\r\n",
           (unsigned)s_od_nfc_field_detect.pin_cfg,
           (unsigned)pin_state,
           (unsigned)prev_state,
           (unsigned)logical,
           (unsigned long)s_od_nfc_field_scan_counter,
           (unsigned)dynamic_return[s_od_nfc_field_detect.adv_button_byte_index],
           scan_event ? " scan++" : "",
           s_od_nfc_field_detect.active_high ? "high" : "low");
  }
  return true;
}

static void od_nfc_field_detect_irq_callback(uint8_t int_no, void *context)
{
  od_nfc_field_detect_t *st = (od_nfc_field_detect_t *)context;
  (void)int_no;
  if (st == NULL || !st->enabled) {
    return;
  }
  st->irq_pending = true;
}

static void od_nfc_field_detect_arm_em4_wakeup(void)
{
  sl_gpio_t gpio;
  sl_status_t sc;
  int32_t em4_int_no = SL_GPIO_INTERRUPT_UNAVAILABLE;

  if (!s_od_nfc_field_detect.enabled) {
    return;
  }

  gpio.port = (sl_gpio_port_t)(s_od_nfc_field_detect.pin_cfg >> 4);
  gpio.pin = (s_od_nfc_field_detect.pin_cfg & 0x0Fu);
  sc = sl_gpio_configure_wakeup_em4_interrupt(&gpio,
                                              &em4_int_no,
                                              s_od_nfc_field_detect.active_high,
                                              od_nfc_field_detect_irq_callback,
                                              &s_od_nfc_field_detect);
  if (sc == SL_STATUS_OK) {
    s_od_nfc_field_detect.em4_int_no = em4_int_no;
    printf("[OD][NFC] EM4 wake armed on field-detect pin=0x%02X em4_int=%ld polarity=%s\r\n",
           (unsigned)s_od_nfc_field_detect.pin_cfg,
           (long)em4_int_no,
           s_od_nfc_field_detect.active_high ? "high" : "low");
  } else {
    printf("[OD][NFC] EM4 wake arm failed pin=0x%02X sc=0x%04lX\r\n",
           (unsigned)s_od_nfc_field_detect.pin_cfg, (unsigned long)sc);
  }
}

static bool od_nfc_field_detect_process(uint32_t now_ms)
{
  if (!s_od_nfc_field_detect.enabled) {
    return false;
  }
  if (s_od_nfc_field_detect.mode == 2u) {
    if (!s_od_nfc_field_detect.irq_pending) {
      return false;
    }
    s_od_nfc_field_detect.irq_pending = false;
    return od_nfc_field_detect_refresh(now_ms, false);
  }
  return od_nfc_field_detect_refresh(now_ms, false);
}

static void od_nfc_autoinit_from_config(const struct GlobalConfig *cfg)
{
  const struct NfcConfig *nfc_cfg = NULL;
  const struct DataBus *bus = NULL;
  uint8_t i;
  GPIO_Port_TypeDef pwr_port;
  uint8_t pwr_pin;

  s_od_nfc_enabled = false;
  s_od_nfc_has_pwr_pin = false;
  s_od_nfc_ic_type = OD_NFC_IC_AUTO;
  s_od_nfc_power_on_delay_ms = 40u;
  memset(&s_od_nfc_field_detect, 0, sizeof(s_od_nfc_field_detect));
  s_od_nfc_field_detect.int_no = -1;

  if (cfg == NULL || !cfg->loaded || cfg->nfc_config_count == 0u) {
    printf("[OD][NFC] no NFC config packet (0x2A); NFC init disabled\r\n");
    return;
  }

  for (i = 0u; i < cfg->nfc_config_count; i++) {
    if ((cfg->nfc_configs[i].flags & 0x01u) != 0u) {
      nfc_cfg = &cfg->nfc_configs[i];
      break;
    }
  }
  if (nfc_cfg == NULL) {
    printf("[OD][NFC] NFC configs present, but none enabled; NFC init disabled\r\n");
    return;
  }

  for (i = 0u; i < cfg->data_bus_count; i++) {
    if (cfg->data_buses[i].instance_number == nfc_cfg->bus_instance) {
      bus = &cfg->data_buses[i];
      break;
    }
  }
  if (bus == NULL || bus->bus_type != OD_BUS_TYPE_I2C) {
    printf("[OD][NFC] bus instance=%u missing/non-I2C; NFC init disabled\r\n", (unsigned)nfc_cfg->bus_instance);
    return;
  }
  if (!od_pin_decode(bus->pin_1, &s_od_nfc_scl_port, &s_od_nfc_scl_pin)
      || !od_pin_decode(bus->pin_2, &s_od_nfc_sda_port, &s_od_nfc_sda_pin)) {
    printf("[OD][NFC] bad bus pins for instance=%u; NFC init disabled\r\n", (unsigned)nfc_cfg->bus_instance);
    return;
  }

  s_od_nfc_ic_type = nfc_cfg->nfc_ic_type;
  s_od_nfc_power_on_delay_ms = (nfc_cfg->power_on_delay_ms == 0u) ? 40u : nfc_cfg->power_on_delay_ms;

  if (nfc_cfg->power_pin != GPIO_PIN_UNUSED && od_pin_decode(nfc_cfg->power_pin, &pwr_port, &pwr_pin)) {
    s_od_nfc_has_pwr_pin = true;
    s_od_nfc_pwr_port = pwr_port;
    s_od_nfc_pwr_pin = pwr_pin;
  } else if (od_pin_decode(bus->pin_3, &pwr_port, &pwr_pin)) {
    s_od_nfc_has_pwr_pin = true;
    s_od_nfc_pwr_port = pwr_port;
    s_od_nfc_pwr_pin = pwr_pin;
  }

  if (s_od_nfc_ic_type != OD_NFC_IC_AUTO && s_od_nfc_ic_type != OD_NFC_IC_TNB132M) {
    printf("[OD][NFC] ic_type=%u unsupported yet; NFC init disabled\r\n", (unsigned)s_od_nfc_ic_type);
    return;
  }
  s_od_nfc_enabled = true;
  od_nfc_field_detect_init_from_config(nfc_cfg);
  printf("[OD][NFC] enabled: bus=%u ic_type=%u scl=0x%02X sda=0x%02X pwr=%s\r\n",
         (unsigned)nfc_cfg->bus_instance,
         (unsigned)s_od_nfc_ic_type,
         (unsigned)bus->pin_1,
         (unsigned)bus->pin_2,
         s_od_nfc_has_pwr_pin ? "yes" : "no");
}

static void od_nfc_init_sequence(void)
{
  GPIO_Port_TypeDef sp = s_od_nfc_scl_port;
  uint8_t sn = s_od_nfc_scl_pin;
  GPIO_Port_TypeDef dp = s_od_nfc_sda_port;
  uint8_t dn = s_od_nfc_sda_pin;

  if (!s_od_nfc_enabled) {
    return;
  }

  GPIO_PinModeSet(sp, sn, gpioModeWiredAndFilter, 0);
  GPIO_PinModeSet(dp, dn, gpioModeWiredAndFilter, 0);
  if (s_od_nfc_has_pwr_pin) {
    GPIO_PinModeSet(s_od_nfc_pwr_port, s_od_nfc_pwr_pin, gpioModePushPull, 1);
    sl_udelay_wait((uint32_t)s_od_nfc_power_on_delay_ms * 1000u);
  } else {
    sl_udelay_wait(10000);
  }

  od_nfc_tnb132m_prime_type3(sp, sn, dp, dn);

#if OD_TNB132M_BOOT_PROBE
  sl_udelay_wait(2000);
#if OD_TNB132M_WRITE_NDEF
  (void)od_nfc_write_ndef_text_en(sp, sn, dp, dn, OD_TNB132M_NDEF_TEXT);
  sl_udelay_wait(50000);
  od_nfc_tnb132m_prime_type3(sp, sn, dp, dn);
  sl_udelay_wait(2000);
#endif
  od_nfc_boot_dump_ndef_t3(sp, sn, dp, dn);
  sl_udelay_wait(1000);
#endif

  sl_udelay_wait(20000);

  od_nfc_i2c_start(sp, sn, dp, dn);
  (void)od_nfc_i2c_write_byte(sp, sn, dp, dn, (uint8_t)(0x30u << 1));
  (void)od_nfc_i2c_write_byte(sp, sn, dp, dn, 0x21u);
  (void)od_nfc_i2c_write_byte(sp, sn, dp, dn, 0x01u);
  od_nfc_i2c_stop(sp, sn, dp, dn);

  sl_udelay_wait(14000);
  (void)od_nfc_field_detect_refresh(sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count()), true);

  if (s_od_nfc_has_pwr_pin) {
    GPIO_PinOutClear(s_od_nfc_pwr_port, s_od_nfc_pwr_pin);
    GPIO_PinModeSet(s_od_nfc_pwr_port, s_od_nfc_pwr_pin, gpioModeInput, 1);
  }
  GPIO_PinModeSet(sp, sn, gpioModeInput, 1);
  GPIO_PinModeSet(dp, dn, gpioModeInput, 1);
}

static bool od_nfc_read_record_raw(uint8_t *type_out, uint8_t *out, uint16_t *io_len, uint16_t out_max)
{
  uint8_t ai[16];
  uint8_t *data = s_od_nfc_read_data;
  uint32_t ln;
  uint16_t off;
  uint16_t tlen;
  uint16_t plen;
  uint16_t copy_len;
  bool sr;
  CORE_DECLARE_IRQ_STATE;

  if (type_out == NULL || io_len == NULL || out == NULL) {
    return false;
  }

  CORE_ENTER_CRITICAL();

  GPIO_PinModeSet(s_od_nfc_scl_port, s_od_nfc_scl_pin, gpioModeWiredAndFilter, 0);
  GPIO_PinModeSet(s_od_nfc_sda_port, s_od_nfc_sda_pin, gpioModeWiredAndFilter, 0);
  if (s_od_nfc_has_pwr_pin) {
    GPIO_PinModeSet(s_od_nfc_pwr_port, s_od_nfc_pwr_pin, gpioModePushPull, 1);
    sl_udelay_wait(40000);
  } else {
    sl_udelay_wait(10000);
  }

  od_nfc_tnb132m_prime_type3(s_od_nfc_scl_port, s_od_nfc_scl_pin, s_od_nfc_sda_port, s_od_nfc_sda_pin);
  sl_udelay_wait(2000);
  if (!od_nfc_type3_paged_block_read16(s_od_nfc_scl_port, s_od_nfc_scl_pin,
                                        s_od_nfc_sda_port, s_od_nfc_sda_pin,
                                        0x48u, 0x00u, ai)) {
    CORE_EXIT_CRITICAL();
    return false;
  }
  if ((ai[0] & 0xF0u) != 0x10u) {
    CORE_EXIT_CRITICAL();
    return false;
  }

  ln = ((uint32_t)ai[11] << 16) | ((uint32_t)ai[12] << 8) | (uint32_t)ai[13];
  if (ln == 0u || ln > sizeof(s_od_nfc_read_data)) {
    CORE_EXIT_CRITICAL();
    return false;
  }
  for (uint16_t b = 0; b < (uint16_t)((ln + 15u) / 16u); b++) {
    uint8_t byte_off = (uint8_t)(0x10u + b * 0x10u);
    (void)od_nfc_type3_paged_block_read16(s_od_nfc_scl_port, s_od_nfc_scl_pin,
                                          s_od_nfc_sda_port, s_od_nfc_sda_pin,
                                          0x48u, 0x00u, s_od_nfc_read_throwaway);
    sl_udelay_wait(500);
    if (!od_nfc_type3_paged_block_read16(s_od_nfc_scl_port, s_od_nfc_scl_pin,
                                         s_od_nfc_sda_port, s_od_nfc_sda_pin,
                                         0x40u, byte_off, &data[b * 16u])) {
      CORE_EXIT_CRITICAL();
      return false;
    }
  }

  CORE_EXIT_CRITICAL();

  if (ln < 3u) {
    return false;
  }
  sr = (data[0] & 0x10u) != 0u;
  if (!sr) {
    return false;
  }
  /* IL (payload ID length field present): unsupported — return verbatim NDEF. */
  if ((data[0] & 0x08u) != 0u) {
    if (ln > out_max) {
      *io_len = out_max;
    } else {
      *io_len = (uint16_t)ln;
    }
    *type_out = OD_NFC_REC_RAW_NDEF;
    memcpy(out, data, *io_len);
    return true;
  }

  tlen = data[1];
  plen = data[2];
  off = 3u;
  if (((uint32_t)off + (uint32_t)tlen + (uint32_t)plen) > ln) {
    return false;
  }

  switch (data[0] & 7u) {
  case 2u: /* NDEF MIME (Media-type): host payload = type string + MIME body byte-for-byte */
  {
    uint16_t out_pack;
    *type_out = OD_NFC_REC_MIME;
    if (plen > 255u || tlen == 0u) {
      return false;
    }
    out_pack = (uint16_t)(1u + tlen + plen);
    if (out_pack > out_max) {
      return false;
    }
    out[0] = tlen;
    memcpy(&out[1], &data[off], tlen);
    memcpy(&out[1u + tlen], &data[(uint16_t)(off + tlen)], plen);
    *io_len = out_pack;
    return true;
  }

  case 1u:
    break;

  default: /* TNF ≠ Well-known/MIME SR: verbatim NDEF (future-proof passthrough). */
    if (ln > out_max) {
      *io_len = out_max;
    } else {
      *io_len = (uint16_t)ln;
    }
    *type_out = OD_NFC_REC_RAW_NDEF;
    memcpy(out, data, *io_len);
    return true;
  }

  /* Well-known SR (already bounds-checked above). */

  if (tlen == 1u && data[off] == 'T') {
    uint8_t status;
    uint8_t lang_len;
    *type_out = OD_NFC_REC_TEXT;
    off = (uint16_t)(off + 1u);
    if (plen < 1u) {
      return false;
    }
    status = data[off];
    lang_len = status & 0x3Fu;
    if ((uint16_t)(1u + lang_len) > plen) {
      return false;
    }
    off = (uint16_t)(off + 1u + lang_len);
    copy_len = (uint16_t)(plen - 1u - lang_len);
  } else if (tlen == 1u && data[off] == 'U') {
    uint8_t uri_prefix;
    *type_out = OD_NFC_REC_URI;
    off = (uint16_t)(off + 1u);
    if (plen < 1u) {
      return false;
    }
    uri_prefix = data[off];
    if (uri_prefix != 0x00u) {
      return false;
    }
    off = (uint16_t)(off + 1u);
    copy_len = (uint16_t)(plen - 1u);
  } else {
    uint16_t raw_len;
    *type_out = OD_NFC_REC_WELL_KNOWN_RAW;
    raw_len = (uint16_t)(1u + tlen + plen);
    if (raw_len > out_max) {
      raw_len = out_max;
    }
    if (raw_len == 0u) {
      *io_len = 0u;
      return true;
    }
    out[0] = tlen;
    copy_len = (uint16_t)(raw_len - 1u);
    memcpy(&out[1], &data[off], copy_len);
    *io_len = raw_len;
    return true;
  }

  if (copy_len > out_max) {
    copy_len = out_max;
  }
  memcpy(out, &data[off], copy_len);
  *io_len = copy_len;
  return true;
}

static bool od_nfc_write_record_raw(uint8_t rec_type, const uint8_t *data, uint16_t data_len)
{
  uint8_t ai[16] = { 0 };
  uint8_t cur_ai[16];
  uint8_t *blocks = s_od_nfc_write_blocks;
  uint16_t sum;
  uint16_t record_len;
  uint16_t payload_len;
  uint8_t need_blocks;
  uint8_t i;
  CORE_DECLARE_IRQ_STATE;

  if (data == NULL || data_len == 0u) {
    return false;
  }
  /* Defense-in-depth: bound data_len so the uint16_t payload_len math below
   * (1 + hdr + data_len) cannot wrap and overrun s_od_nfc_write_blocks. Callers
   * validate too, but this keeps the record builder memory-safe on its own. */
  if (data_len > sizeof(s_od_nfc_write_blocks)) {
    return false;
  }
  memset(blocks, 0, sizeof(s_od_nfc_write_blocks));

  if (rec_type == OD_NFC_REC_TEXT) {
    payload_len = (uint16_t)(1u + 2u + data_len);
    if (payload_len > 255u || payload_len > (uint16_t)(sizeof(s_od_nfc_write_blocks) - 4u)) {
      return false;
    }
    record_len = (uint16_t)(4u + payload_len);
    blocks[0] = 0xD1u;
    blocks[1] = 0x01u;
    blocks[2] = (uint8_t)payload_len;
    blocks[3] = 0x54u;
    blocks[4] = 0x02u;
    blocks[5] = (uint8_t)'e';
    blocks[6] = (uint8_t)'n';
    memcpy(&blocks[7], data, data_len);
  } else if (rec_type == OD_NFC_REC_URI) {
    payload_len = (uint16_t)(1u + data_len);
    if (payload_len > 255u || payload_len > (uint16_t)(sizeof(s_od_nfc_write_blocks) - 4u)) {
      return false;
    }
    record_len = (uint16_t)(4u + payload_len);
    blocks[0] = 0xD1u;
    blocks[1] = 0x01u;
    blocks[2] = (uint8_t)payload_len;
    blocks[3] = 0x55u;
    blocks[4] = 0x00u;
    memcpy(&blocks[5], data, data_len);
  } else if (rec_type == OD_NFC_REC_WELL_KNOWN_RAW) {
    uint8_t type_len;
    uint16_t raw_payload_len;
    if (data_len < 2u) {
      return false;
    }
    type_len = data[0];
    if (type_len == 0u || (uint16_t)(1u + type_len) > data_len) {
      return false;
    }
    raw_payload_len = (uint16_t)(data_len - 1u - type_len);
    if (raw_payload_len > 255u) {
      return false;
    }
    record_len = (uint16_t)(3u + type_len + raw_payload_len);
    if (record_len > sizeof(s_od_nfc_write_blocks)) {
      return false;
    }
    blocks[0] = 0xD1u;
    blocks[1] = type_len;
    blocks[2] = (uint8_t)raw_payload_len;
    memcpy(&blocks[3], &data[1], type_len);
    if (raw_payload_len > 0u) {
      memcpy(&blocks[3u + type_len], &data[1u + type_len], raw_payload_len);
    }
  } else if (rec_type == OD_NFC_REC_MIME) {
    uint8_t mime_tl;
    uint16_t body_len;

    if (data_len < 3u) {
      return false;
    }
    mime_tl = data[0];
    if (mime_tl == 0u || (uint16_t)(1u + mime_tl) > data_len) {
      return false;
    }
    body_len = (uint16_t)(data_len - 1u - mime_tl);
    if (body_len > 255u) {
      return false;
    }
    record_len = (uint16_t)(3u + mime_tl + body_len);
    if (record_len > sizeof(s_od_nfc_write_blocks)) {
      return false;
    }
    blocks[0] = 0xD2u; /* MB | ME | SR ; TNF = MIME */
    blocks[1] = mime_tl;
    blocks[2] = (uint8_t)body_len;
    memcpy(&blocks[3], &data[1], mime_tl);
    if (body_len > 0u) {
      memcpy(&blocks[3u + mime_tl], &data[1u + mime_tl], body_len);
    }
  } else if (rec_type == OD_NFC_REC_RAW_NDEF) {
    record_len = data_len;
    if (record_len == 0u || record_len > sizeof(s_od_nfc_write_blocks)) {
      return false;
    }
    memcpy(blocks, data, record_len);
  } else {
    return false;
  }

  CORE_ENTER_CRITICAL();

  GPIO_PinModeSet(s_od_nfc_scl_port, s_od_nfc_scl_pin, gpioModeWiredAndFilter, 0);
  GPIO_PinModeSet(s_od_nfc_sda_port, s_od_nfc_sda_pin, gpioModeWiredAndFilter, 0);
  if (s_od_nfc_has_pwr_pin) {
    GPIO_PinModeSet(s_od_nfc_pwr_port, s_od_nfc_pwr_pin, gpioModePushPull, 1);
    sl_udelay_wait(40000);
  } else {
    sl_udelay_wait(10000);
  }

  od_nfc_tnb132m_prime_type3(s_od_nfc_scl_port, s_od_nfc_scl_pin, s_od_nfc_sda_port, s_od_nfc_sda_pin);
  sl_udelay_wait(2000);

  ai[0] = 0x10u;
  ai[1] = 0x02u;
  ai[2] = 0x01u;
  ai[3] = 0x00u;
  ai[4] = 0x3Cu;
  ai[11] = (uint8_t)((record_len >> 16) & 0xFFu);
  ai[12] = (uint8_t)((record_len >> 8) & 0xFFu);
  ai[13] = (uint8_t)(record_len & 0xFFu);

  if (od_nfc_type3_paged_block_read16(s_od_nfc_scl_port, s_od_nfc_scl_pin, s_od_nfc_sda_port,
                                      s_od_nfc_sda_pin, 0x48u, 0x00u, cur_ai)
      && (cur_ai[0] & 0xF0u) == 0x10u) {
    ai[10] = cur_ai[10];
  }
  sum = 0u;
  for (i = 0u; i < 14u; i++) {
    sum = (uint16_t)(sum + ai[i]);
  }
  ai[14] = (uint8_t)(sum >> 8);
  ai[15] = (uint8_t)(sum & 0xFFu);

  if (!od_nfc_type3_paged_block_write16(s_od_nfc_scl_port, s_od_nfc_scl_pin, s_od_nfc_sda_port,
                                        s_od_nfc_sda_pin, 0x48u, 0x00u, ai)) {
    CORE_EXIT_CRITICAL();
    return false;
  }
  sl_udelay_wait(10000);
  need_blocks = (uint8_t)((record_len + 15u) / 16u);
  for (i = 0u; i < need_blocks; i++) {
    uint8_t byte_off = (uint8_t)(0x10u + i * 0x10u);
    if (!od_nfc_type3_paged_block_write16(s_od_nfc_scl_port, s_od_nfc_scl_pin, s_od_nfc_sda_port,
                                          s_od_nfc_sda_pin, 0x40u, byte_off, &blocks[i * 16u])) {
      CORE_EXIT_CRITICAL();
      return false;
    }
    sl_udelay_wait(10000);
  }

  CORE_EXIT_CRITICAL();
  return true;
}

static void od_flash_autoinit_from_config(const struct GlobalConfig *cfg)
{
  const struct FlashConfig *flash_cfg = NULL;
  uint8_t i;

  s_od_flash_enabled = false;
  if (cfg == NULL || !cfg->loaded || cfg->flash_config_count == 0u) {
    printf("[OD][FLASH] no flash config packet (0x2B); flash deep-sleep config disabled\r\n");
    return;
  }
  for (i = 0u; i < cfg->flash_config_count; i++) {
    if ((cfg->flash_configs[i].flags & 0x01u) != 0u) {
      flash_cfg = &cfg->flash_configs[i];
      break;
    }
  }
  if (flash_cfg == NULL) {
    printf("[OD][FLASH] flash configs present, but none enabled\r\n");
    return;
  }
  if (!od_pin_decode(flash_cfg->mosi_pin, &s_od_flash_mosi_port, &s_od_flash_mosi_pin)
      || !od_pin_decode(flash_cfg->sck_pin, &s_od_flash_sck_port, &s_od_flash_sck_pin)
      || !od_pin_decode(flash_cfg->cs_pin, &s_od_flash_cs_port, &s_od_flash_cs_pin)) {
    printf("[OD][FLASH] bad MOSI/SCK/CS pins in flash config; flash deep-sleep config disabled\r\n");
    return;
  }
  s_od_flash_enabled = true;
  printf("[OD][FLASH] enabled mosi=0x%02X sck=0x%02X cs=0x%02X\r\n",
         (unsigned)flash_cfg->mosi_pin, (unsigned)flash_cfg->sck_pin, (unsigned)flash_cfg->cs_pin);
}

static void od_flash_enter_deep_sleep(void)
{
  uint8_t cmd = 0xB9u;

  if (!s_od_flash_enabled) {
    return;
  }

  GPIO_PinModeSet(s_od_flash_mosi_port, s_od_flash_mosi_pin, gpioModePushPull, 0);
  GPIO_PinModeSet(s_od_flash_sck_port, s_od_flash_sck_pin, gpioModePushPull, 0);
  GPIO_PinModeSet(s_od_flash_cs_port, s_od_flash_cs_pin, gpioModePushPull, 1);

  GPIO_PinOutClear(s_od_flash_cs_port, s_od_flash_cs_pin);
  for (uint8_t bit = 0; bit < 8u; bit++) {
    bool one = (cmd & 0x80u) != 0u;
    if (one) {
      GPIO_PinOutSet(s_od_flash_mosi_port, s_od_flash_mosi_pin);
    } else {
      GPIO_PinOutClear(s_od_flash_mosi_port, s_od_flash_mosi_pin);
    }
    cmd <<= 1;
    sl_udelay_wait(1);
    GPIO_PinOutSet(s_od_flash_sck_port, s_od_flash_sck_pin);
    sl_udelay_wait(1);
    GPIO_PinOutClear(s_od_flash_sck_port, s_od_flash_sck_pin);
  }
  GPIO_PinOutSet(s_od_flash_cs_port, s_od_flash_cs_pin);
  sl_udelay_wait(30);
}

static void od_init_aux_peripherals(void)
{
  CMU_ClockEnable(cmuClock_GPIO, true);

  od_nfc_autoinit_from_config(&s_od_global_config);
  od_nfc_init_sequence();
  od_flash_autoinit_from_config(&s_od_global_config);

  od_flash_enter_deep_sleep();

  if (s_od_flash_enabled) {
    /* Park external flash SPI lines in passive states. */
    GPIO_PinModeSet(s_od_flash_mosi_port, s_od_flash_mosi_pin, gpioModeInputPull, 1);
    GPIO_PinModeSet(s_od_flash_sck_port, s_od_flash_sck_pin, gpioModeInputPull, 0);
    GPIO_PinModeSet(s_od_flash_cs_port, s_od_flash_cs_pin, gpioModeInputPull, 1);
  }
}

const struct GlobalConfig *opendisplay_get_global_config(void)
{
  return &s_od_global_config;
}

void opendisplay_ble_reload_config_from_nvm(void)
{
  if (!loadGlobalConfig(&s_od_global_config)) {
    printf("[OD] config: reload after save failed\r\n");
  }
  od_buttons_init_from_config();
  opendisplay_display_park_pins();
  opendisplay_led_init();
  opendisplay_display_boot_apply();
}

static void od_apply_advertising_timing(uint8_t adv_handle, uint32_t now_ms)
{
  sl_status_t sc;
  uint16_t min_slots;
  uint16_t max_slots;

  if (adv_handle == 0xFFu) {
    return;
  }
  if (s_adv_boost_until_ms != 0u && now_ms < s_adv_boost_until_ms) {
    min_slots = OD_ADV_INTERVAL_BOOST_MIN;
    max_slots = OD_ADV_INTERVAL_BOOST_MAX;
  } else {
    s_adv_boost_until_ms = 0u;
    min_slots = OD_ADV_INTERVAL_IDLE_SLOTS;
    max_slots = OD_ADV_INTERVAL_IDLE_SLOTS;
  }
  sc = sl_bt_advertiser_set_timing(adv_handle, min_slots, max_slots, 0, 0);
  if (sc != SL_STATUS_OK) {
    printf("[OD] advertiser_set_timing sc=0x%04lX\r\n", (unsigned long)sc);
  }
  app_assert_status(sc);
}

static void od_boost_advertising(uint32_t now_ms)
{
  s_adv_boost_until_ms = now_ms + OD_ADV_BOOST_MS;
}

static void od_advertising_boost_tick(uint8_t adv_handle, uint32_t now_ms)
{
  static bool was_boosted = false;
  bool boosting = (s_adv_boost_until_ms != 0u && now_ms < s_adv_boost_until_ms);

  if (boosting) {
    was_boosted = true;
    return;
  }
  if (!was_boosted) {
    return;
  }
  was_boosted = false;
  s_adv_boost_until_ms = 0u;
  od_apply_advertising_timing(adv_handle, now_ms);
  if (g_connection == 0xFFu) {
    (void)sl_bt_advertiser_stop(adv_handle);
    app_assert_status(sl_bt_legacy_advertiser_start(adv_handle, sl_bt_legacy_advertiser_connectable));
  }
}

static void chip_id_hex6(char out[7])
{
  uint64_t u = SYSTEM_GetUnique();
  uint32_t id = (uint32_t)(u & 0xFFFFFFu);
  snprintf(out, 7, "%06lX", (unsigned long)id);
}

static void update_msd_payload(bool quick)
{
  float chip_temperature = EMU_TemperatureGet();
  int16_t temp_encoded;
  uint16_t battery_voltage_10mv = 0u;
  uint32_t now_ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
  bool measure_batt = !quick
                      && ((s_batt_voltage_mv_cache == 0u)
                          || ((now_ms - s_last_batt_measure_ms) > OD_MSD_UPDATE_INTERVAL_MS));
  uint8_t temperature_byte;
  uint8_t battery_voltage_low_byte;
  uint8_t status_byte;
  uint32_t sample;
  IADC_Init_t init = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t initAllConfigs = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitSingle_t initSingle = IADC_INITSINGLE_DEFAULT;
  IADC_SingleInput_t initSingleInput = IADC_SINGLEINPUT_DEFAULT;

  /* MSD temperature: 0.5 C steps, range -40.0C..+87.5C (OpenDisplay BLE encoding). */
  temp_encoded = (int16_t)((chip_temperature + 40.0f) * 2.0f);
  if (temp_encoded < 0) {
    temp_encoded = 0;
  } else if (temp_encoded > 255) {
    temp_encoded = 255;
  }
  temperature_byte = (uint8_t)temp_encoded;
  if (measure_batt) {
    CMU_ClockEnable(cmuClock_IADC0, true);
    IADC_reset(IADC0);
    CMU_ClockSelectSet(cmuClock_IADCCLK, cmuSelect_FSRCO);
    init.warmup = iadcWarmupNormal;
    init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, 20000000, 0);
    initAllConfigs.configs[0].reference = iadcCfgReferenceInt1V2;
    initAllConfigs.configs[0].vRef = 1210;
    initAllConfigs.configs[0].osrHighSpeed = iadcCfgOsrHighSpeed2x;
    initAllConfigs.configs[0].analogGain = iadcCfgAnalogGain1x;
    initAllConfigs.configs[0].adcClkPrescale = IADC_calcAdcClkPrescale(
      IADC0, 10000000, 0, iadcCfgModeNormal, init.srcClkPrescale);
    initSingleInput.posInput = iadcPosInputAvdd;
    initSingleInput.negInput = iadcNegInputGnd;
    IADC_init(IADC0, &init, &initAllConfigs);
    IADC_initSingle(IADC0, &initSingle, &initSingleInput);
    IADC_command(IADC0, iadcCmdStartSingle);
    while ((IADC0->STATUS & (_IADC_STATUS_CONVERTING_MASK | _IADC_STATUS_SINGLEFIFODV_MASK))
           != IADC_STATUS_SINGLEFIFODV) {
    }
    sample = IADC_pullSingleFifoResult(IADC0).data;
    s_batt_voltage_mv_cache = (uint16_t)((sample * 4u * 1200u) / 4095u);
    s_last_batt_measure_ms = now_ms;
    IADC_command(IADC0, iadcCmdStopSingle);
    IADC_reset(IADC0);
    CMU_ClockEnable(cmuClock_IADC0, false);
  }
  battery_voltage_10mv = (uint16_t)(s_batt_voltage_mv_cache / 10u);
  if (battery_voltage_10mv > 511u) {
    battery_voltage_10mv = 511u;
  }
  battery_voltage_low_byte = (uint8_t)(battery_voltage_10mv & 0xFFu);
  status_byte = (uint8_t)(((battery_voltage_10mv >> 8) & 0x01u)
                           | ((reboot_flag & 0x01u) << 1)
                           | ((connection_requested & 0x01u) << 2)
                           | ((msd_loop_counter & 0x0Fu) << 4));

  memset(msd_payload, 0, sizeof(msd_payload));
  msd_payload[0] = (uint8_t)(OPENDISPLAY_COMPANY_ID & 0xFFu);
  msd_payload[1] = (uint8_t)((OPENDISPLAY_COMPANY_ID >> 8) & 0xFFu);
  memcpy(&msd_payload[2], dynamic_return, sizeof(dynamic_return));
  msd_payload[13] = temperature_byte;
  msd_payload[14] = battery_voltage_low_byte;
  msd_payload[15] = status_byte;
  msd_loop_counter = (uint8_t)((msd_loop_counter + 1u) & 0x0Fu);
}

static sl_status_t set_gap_device_name(const char *name)
{
  size_t len = strlen(name);
  return sl_bt_gatt_server_write_attribute_value(gattdb_device_name, 0, len, (const uint8_t *)name);
}

static sl_status_t install_opendisplay_gatt(void)
{
  uint16_t session;
  uint16_t svc;
  uint16_t ch_pipe;
  sl_bt_uuid_16_t uuid_svc = { .data = { 0x46, 0x24 } };
  sl_bt_uuid_16_t uuid_pipe = { .data = { 0x46, 0x24 } };
  uint8_t pipe_init = 0;
  sl_status_t sc;

  sc = sl_bt_gattdb_new_session(&session);
  if (sc != SL_STATUS_OK) {
    return sc;
  }

  sc = sl_bt_gattdb_add_service(session,
                                sl_bt_gattdb_primary_service,
                                0,
                                sizeof(uuid_svc.data),
                                uuid_svc.data,
                                &svc);
  if (sc != SL_STATUS_OK) {
    (void)sl_bt_gattdb_abort(session);
    return sc;
  }

  sc = sl_bt_gattdb_add_uuid16_characteristic(session,
                                              svc,
                                              SL_BT_GATTDB_CHARACTERISTIC_READ
                                                | SL_BT_GATTDB_CHARACTERISTIC_WRITE
                                                | SL_BT_GATTDB_CHARACTERISTIC_WRITE_NO_RESPONSE
                                                | SL_BT_GATTDB_CHARACTERISTIC_NOTIFY,
                                              0,
                                              0,
                                              uuid_pipe,
                                              sl_bt_gattdb_variable_length_value,
                                              OPENDISPLAY_MAX_PIPE_LEN,
                                              1,
                                              &pipe_init,
                                              &ch_pipe);
  if (sc != SL_STATUS_OK) {
    (void)sl_bt_gattdb_abort(session);
    return sc;
  }

  sc = sl_bt_gattdb_start_service(session, svc);
  if (sc != SL_STATUS_OK) {
    (void)sl_bt_gattdb_abort(session);
    return sc;
  }

  sc = sl_bt_gattdb_commit(session);
  if (sc != SL_STATUS_OK) {
    return sc;
  }

  g_od_pipe_char = ch_pipe;
  return SL_STATUS_OK;
}

static void build_and_apply_adv(uint8_t adv_set, const char *name, bool quick)
{
  uint8_t adv[31];
  uint8_t sr[31];
  size_t ai = 0;
  size_t si = 0;
  size_t nl = strlen(name);
  sl_status_t sc;

  update_msd_payload(quick);

  adv[ai++] = 2u;
  adv[ai++] = 0x01u;
  adv[ai++] = 0x06u;

  adv[ai++] = 17u;
  adv[ai++] = 0xFFu;
  adv[ai++] = (uint8_t)(OPENDISPLAY_COMPANY_ID & 0xFFu);
  adv[ai++] = (uint8_t)((OPENDISPLAY_COMPANY_ID >> 8) & 0xFFu);
  memcpy(&adv[ai], &msd_payload[2], 14);
  ai += 14;

  if (ai + 2 + nl > sizeof(adv)) {
    nl = sizeof(adv) - ai - 2;
  }
  adv[ai++] = (uint8_t)(1 + nl);
  adv[ai++] = 0x09u;
  memcpy(&adv[ai], name, nl);
  ai += nl;

  sr[si++] = 3u;
  sr[si++] = 0x03u;
  sr[si++] = 0x46u;
  sr[si++] = 0x24u;

  sc = sl_bt_legacy_advertiser_set_data(adv_set,
                                        sl_bt_advertiser_advertising_data_packet,
                                        ai,
                                        adv);
  if (sc != SL_STATUS_OK) {
    printf("[OD] legacy_advertiser_set_data(adv) sc=0x%04lX len=%u\r\n",
           (unsigned long)sc, (unsigned)ai);
  }
  app_assert_status(sc);
  sc = sl_bt_legacy_advertiser_set_data(adv_set,
                                        sl_bt_advertiser_scan_response_packet,
                                        si,
                                        sr);
  if (sc != SL_STATUS_OK) {
    printf("[OD] legacy_advertiser_set_data(sr) sc=0x%04lX len=%u\r\n",
           (unsigned long)sc, (unsigned)si);
  }
  app_assert_status(sc);
}

void opendisplay_ble_on_boot(uint8_t advertising_set_handle)
{
  char hex[7];
  sl_status_t sc;

  if (loadGlobalConfig(&s_od_global_config)) {
    printf("[OD] config: loaded displays=%u leds=%u buses=%u nfc=%u flash=%u ver=%u.%u\r\n",
           (unsigned)s_od_global_config.display_count,
           (unsigned)s_od_global_config.led_count,
           (unsigned)s_od_global_config.data_bus_count,
           (unsigned)s_od_global_config.nfc_config_count,
           (unsigned)s_od_global_config.flash_config_count,
           (unsigned)s_od_global_config.version,
           (unsigned)s_od_global_config.minor_version);
  } else {
    printf("[OD] config: none or invalid (defaults)\r\n");
  }

  od_init_aux_peripherals();

  s_adv_handle = advertising_set_handle;
  printf("[OD] BLE boot: adv_set=%u\r\n", (unsigned)advertising_set_handle);

  chip_id_hex6(hex);
  snprintf(s_dev_name, sizeof(s_dev_name), "%s%s", OD_NAME_PREFIX, hex);
  printf("[OD] GAP name: %s\r\n", s_dev_name);

  sc = set_gap_device_name(s_dev_name);
  if (sc != SL_STATUS_OK) {
    printf("[OD] set_gap_device_name sc=0x%04lX\r\n", (unsigned long)sc);
  }
  app_assert_status(sc);

  sc = install_opendisplay_gatt();
  if (sc != SL_STATUS_OK) {
    printf("[OD] install_opendisplay_gatt sc=0x%04lX", (unsigned long)sc);
    if (sc == SL_STATUS_NOT_SUPPORTED) {
      printf(" (SL_STATUS_NOT_SUPPORTED: add bluetooth_feature_dynamic_gattdb)\r\n");
    } else {
      printf("\r\n");
    }
  }
  app_assert_status(sc);
  printf("[OD] GATT 0x2446 ok, pipe_char=%u\r\n", (unsigned)g_od_pipe_char);
  opendisplay_pipe_set_characteristic(g_od_pipe_char);

  od_buttons_init_from_config();
  opendisplay_display_park_pins();
  opendisplay_led_init();
  opendisplay_display_boot_apply();

  build_and_apply_adv(advertising_set_handle, s_dev_name, false);
  printf("[OD] advertising + scan rsp set\r\n");

  od_apply_advertising_timing(advertising_set_handle,
                              sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count()));

  sc = sl_bt_legacy_advertiser_start(advertising_set_handle, sl_bt_legacy_advertiser_connectable);
  if (sc != SL_STATUS_OK) {
    printf("[OD] legacy_advertiser_start sc=0x%04lX\r\n", (unsigned long)sc);
  }
  app_assert_status(sc);
  printf("[OD] advertising started (~1 s interval while idle)\r\n");
}

void opendisplay_ble_restart_advertising(uint8_t advertising_set_handle)
{
  build_and_apply_adv(advertising_set_handle, s_dev_name, false);
  od_apply_advertising_timing(advertising_set_handle,
                              sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count()));
  app_assert_status(sl_bt_legacy_advertiser_start(advertising_set_handle,
                                                  sl_bt_legacy_advertiser_connectable));
}

void opendisplay_ble_on_event(sl_bt_msg_t *evt)
{
  opendisplay_pipe_handle_gatt_event(evt);

  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_connection_opened_id:
      g_connection = evt->data.evt_connection_opened.connection;
      s_connection_open_ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
      s_connection_timeout_close_requested = false;
      reboot_flag = 0u;
      printf("[OD] connection opened handle=%u\r\n", (unsigned)g_connection);
      if (s_adv_handle != 0xFFu) {
        sl_status_t sc = sl_bt_advertiser_stop(s_adv_handle);
        if (sc == SL_STATUS_OK) {
          printf("[OD] advertising stopped while connected (single-client mode)\r\n");
        } else {
          printf("[OD] advertiser_stop sc=0x%04lX\r\n", (unsigned long)sc);
        }
      }
      break;
    case sl_bt_evt_connection_closed_id:
      g_connection = 0xFF;
      s_connection_open_ms = 0u;
      s_connection_timeout_close_requested = false;
      opendisplay_pipe_on_connection_closed();
      printf("[OD] connection closed reason=0x%02X\r\n",
             (unsigned)evt->data.evt_connection_closed.reason);
      if (s_adv_handle != 0xFFu) {
        opendisplay_ble_restart_advertising(s_adv_handle);
        printf("[OD] advertising resumed after disconnect\r\n");
      }
      break;
    default:
      (void)g_od_pipe_char;
      break;
  }
}

void opendisplay_ble_schedule_dfu(void)
{
  s_pending_dfu = true;
}

void opendisplay_ble_schedule_deep_sleep(void)
{
  s_pending_deep_sleep = true;
}

void opendisplay_ble_process(void)
{
  opendisplay_led_process();
  uint32_t now_ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
  od_publish_button_msd(s_adv_handle, now_ms);
  if (s_adv_handle != 0xFFu) {
    od_advertising_boost_tick(s_adv_handle, now_ms);
  }
  if (od_nfc_field_detect_process(now_ms) && s_adv_handle != 0xFFu) {
    build_and_apply_adv(s_adv_handle, s_dev_name, false);
  }
  if ((now_ms - s_last_msd_refresh_ms) >= OD_MSD_UPDATE_INTERVAL_MS) {
    s_last_msd_refresh_ms = now_ms;
    if (s_adv_handle != 0xFFu) {
      build_and_apply_adv(s_adv_handle, s_dev_name, false);
    }
  }
  if (g_connection != 0xFFu
      && !s_connection_timeout_close_requested
      && (now_ms - s_connection_open_ms) >= OD_MAX_CONNECTION_MS) {
    sl_status_t sc = sl_bt_connection_close(g_connection);
    if (sc == SL_STATUS_OK) {
      s_connection_timeout_close_requested = true;
      printf("[OD] connection timeout (%lu ms): closing handle=%u\r\n",
             (unsigned long)OD_MAX_CONNECTION_MS, (unsigned)g_connection);
    } else {
      printf("[OD] connection timeout close failed sc=0x%04lX\r\n", (unsigned long)sc);
    }
  }

  if (s_pending_dfu || s_pending_deep_sleep) {
    if (g_connection != 0xFFu) {
      (void)sl_bt_connection_close(g_connection);
      return;
    }
    if (s_pending_dfu) {
      s_pending_dfu = false;
      printf("[OD] DFU: entering bootloader\r\n");
      od_enter_gecko_bootloader();
      return;
    }
    if (s_pending_deep_sleep) {
      s_pending_deep_sleep = false;
      opendisplay_display_power_off();
      od_buttons_arm_em4_wakeup();
      od_nfc_field_detect_arm_em4_wakeup();
      EMU_EnterEM4();
    }
  }
}

uint16_t opendisplay_ble_get_app_version(void)
{
  return (uint16_t)OD_APP_VERSION;
}

void opendisplay_ble_copy_msd_bytes(uint8_t out[16])
{
  memcpy(out, msd_payload, 16);
}

bool opendisplay_ble_nfc_read(uint8_t *type_out, uint8_t *data_out, uint16_t *data_len_io, uint16_t max_len)
{
  bool ok;

  if (!s_od_nfc_enabled) {
    return false;
  }
  ok = od_nfc_read_record_raw(type_out, data_out, data_len_io, max_len);

  if (s_od_nfc_has_pwr_pin) {
    GPIO_PinOutClear(s_od_nfc_pwr_port, s_od_nfc_pwr_pin);
    GPIO_PinModeSet(s_od_nfc_pwr_port, s_od_nfc_pwr_pin, gpioModeInput, 1);
  }
  GPIO_PinModeSet(s_od_nfc_scl_port, s_od_nfc_scl_pin, gpioModeInput, 1);
  GPIO_PinModeSet(s_od_nfc_sda_port, s_od_nfc_sda_pin, gpioModeInput, 1);
  return ok;
}

bool opendisplay_ble_nfc_write(uint8_t type, const uint8_t *data, uint16_t data_len)
{
  bool ok;

  if (!s_od_nfc_enabled) {
    return false;
  }
  ok = od_nfc_write_record_raw(type, data, data_len);

  if (s_od_nfc_has_pwr_pin) {
    GPIO_PinOutClear(s_od_nfc_pwr_port, s_od_nfc_pwr_pin);
    GPIO_PinModeSet(s_od_nfc_pwr_port, s_od_nfc_pwr_pin, gpioModeInput, 1);
  }
  GPIO_PinModeSet(s_od_nfc_scl_port, s_od_nfc_scl_pin, gpioModeInput, 1);
  GPIO_PinModeSet(s_od_nfc_sda_port, s_od_nfc_sda_pin, gpioModeInput, 1);
  return ok;
}
