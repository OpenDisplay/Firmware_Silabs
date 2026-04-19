#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_display.h"
#include "opendisplay_led.h"
#include "opendisplay_pipe.h"
#include "opendisplay_constants.h"
#include "app_assert.h"
#include "gatt_db.h"
#include "em_cmu.h"
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
#define OD_APP_VERSION         0x0019u
#endif

#ifndef OPENDISPLAY_MAX_PIPE_LEN
#define OPENDISPLAY_MAX_PIPE_LEN 244u
#endif

/* BLE adv interval in units of 0.625 ms (used when not connected / undirected adv). */
#define OD_ADV_INTERVAL_IDLE_SLOTS 1600u
#define OD_MSD_UPDATE_INTERVAL_MS  30000u
#define BUTTON_PRESS_TIMEOUT_MS    5000u
#define BUTTON_MAX_PRESS_COUNT     15u
#define BUTTON_ID_MASK             0x07u
#define PRESS_COUNT_MASK           0x0Fu
#define PRESS_COUNT_SHIFT          3u
#define BUTTON_STATE_SHIFT         7u

/* When system_config.pwr_pin is 0xFF, drive this pin HIGH as display rail enable (PA0). Set to 0xFF to disable. */
#ifndef OD_FALLBACK_DISPLAY_PWR_PIN
#define OD_FALLBACK_DISPLAY_PWR_PIN 0x00u
#endif

static uint8_t msd_payload[MSD_PAYLOAD_LEN];
static uint8_t msd_loop_counter;
static uint8_t dynamic_return[11];
static uint8_t reboot_flag = 1u;
static uint8_t connection_requested = 0u;

static uint16_t g_od_pipe_char;
static uint16_t g_od_appver_char;
static uint8_t g_connection;
static uint8_t s_adv_handle = 0xFFu;
static char s_dev_name[16];
static struct GlobalConfig s_od_global_config;
static uint32_t s_last_msd_refresh_ms;
static bool s_pending_dfu;
static bool s_pending_deep_sleep;
static uint32_t s_last_batt_measure_ms;
static uint16_t s_batt_voltage_mv_cache;
typedef struct {
  bool active;
  uint8_t pin_cfg;
  int32_t int_no;
  uint8_t button_id;
  uint8_t byte_index;
  bool inverted;
  uint8_t press_count;
  uint8_t current_state;
  uint32_t last_press_ms;
} od_button_state_t;
static od_button_state_t s_buttons[32];
static uint8_t s_button_count;
static volatile bool s_button_event_pending;
static volatile uint8_t s_last_changed_button_index = 0xFFu;

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
  (void)int_no;
  if (st == NULL || !st->active) {
    return;
  }
  s_last_changed_button_index = (uint8_t)(st - s_buttons);
  s_button_event_pending = true;
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
  }
}

static void od_buttons_init_from_config(void)
{
  const struct GlobalConfig *cfg = &s_od_global_config;
  uint8_t i;
  uint8_t bi;

  od_buttons_deinit_interrupts();
  s_button_count = 0u;
  s_button_event_pending = false;
  s_last_changed_button_index = 0xFFu;
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

static bool od_process_button_event(uint32_t now_ms)
{
  uint8_t idx;
  od_button_state_t *st;
  uint8_t pin_state;
  uint8_t logical_state;
  uint8_t encoded;

  if (!s_button_event_pending) {
    return false;
  }
  s_button_event_pending = false;
  idx = s_last_changed_button_index;
  s_last_changed_button_index = 0xFFu;
  if (idx >= s_button_count) {
    return false;
  }
  st = &s_buttons[idx];
  if (!st->active) {
    return false;
  }
  pin_state = od_read_button_pin(st->pin_cfg);
  logical_state = (st->inverted ? (pin_state == 0u) : (pin_state != 0u)) ? 1u : 0u;
  if (logical_state == st->current_state) {
    return false;
  }
  st->current_state = logical_state;
  if (logical_state != 0u) {
    if (st->last_press_ms == 0u || (now_ms - st->last_press_ms) > BUTTON_PRESS_TIMEOUT_MS) {
      st->press_count = 0u;
    }
    if (st->press_count < BUTTON_MAX_PRESS_COUNT) {
      st->press_count++;
    }
    st->last_press_ms = now_ms;
  }
  encoded = (uint8_t)((st->button_id & BUTTON_ID_MASK)
            | ((st->press_count & PRESS_COUNT_MASK) << PRESS_COUNT_SHIFT)
            | ((st->current_state & 0x01u) << BUTTON_STATE_SHIFT));
  dynamic_return[st->byte_index] = encoded;
  return true;
}

static void od_park_display_pins_from_config(void)
{
  const struct DisplayConfig *d;
  GPIO_Port_TypeDef port;
  uint8_t pin;

  if (s_od_global_config.display_count == 0u) {
    return;
  }
  d = &s_od_global_config.displays[0];

  if (od_pin_decode(d->cs_pin, &port, &pin)) {
    GPIO_PinModeSet(port, pin, gpioModePushPull, 1);
  }
  if (od_pin_decode(d->data_pin, &port, &pin)) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 0);
  }
  if (od_pin_decode(d->clk_pin, &port, &pin)) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 0);
  }
  if (od_pin_decode(d->dc_pin, &port, &pin)) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 0);
  }
  if (od_pin_decode(d->reset_pin, &port, &pin)) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 1);
  }
  if (od_pin_decode(d->busy_pin, &port, &pin)) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 0);
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

static void od_nfc_init_sequence(void)
{
  enum {
    NFC_PWR_PORT = gpioPortD, NFC_PWR_PIN = 0,
    NFC_SCL_PORT = gpioPortD, NFC_SCL_PIN = 1,
    NFC_SDA_PORT = gpioPortD, NFC_SDA_PIN = 3
  };
  uint8_t sink = 0;

  GPIO_PinModeSet(NFC_SCL_PORT, NFC_SCL_PIN, gpioModeWiredAndFilter, 0);
  GPIO_PinModeSet(NFC_SDA_PORT, NFC_SDA_PIN, gpioModeWiredAndFilter, 0);
  GPIO_PinModeSet(NFC_PWR_PORT, NFC_PWR_PIN, gpioModeWiredOrPullDown, 1);
  sl_udelay_wait(40000);

  od_nfc_i2c_start(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, (uint8_t)(0x30u << 1));
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, 0x21u);
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, 0x04u);
  od_nfc_i2c_stop(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);

  od_nfc_i2c_start(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, (uint8_t)(0x30u << 1));
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, 0x25u);
  od_nfc_i2c_start(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, (uint8_t)((0x30u << 1) | 1u));
  sink = od_nfc_i2c_read_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, false);
  (void)sink;
  od_nfc_i2c_stop(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);

  sl_udelay_wait(20000);

  od_nfc_i2c_start(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, (uint8_t)(0x43u << 1));
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, 0x30u);
  od_nfc_i2c_start(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, (uint8_t)((0x43u << 1) | 1u));
  for (uint8_t i = 0; i < 16u; i++) {
    sink = od_nfc_i2c_read_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, i < 15u);
  }
  (void)sink;
  od_nfc_i2c_stop(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);

  sl_udelay_wait(20000);

  od_nfc_i2c_start(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, (uint8_t)(0x30u << 1));
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, 0x21u);
  (void)od_nfc_i2c_write_byte(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN, 0x01u);
  od_nfc_i2c_stop(NFC_SCL_PORT, NFC_SCL_PIN, NFC_SDA_PORT, NFC_SDA_PIN);

  sl_udelay_wait(14000);

  GPIO_PinOutClear(NFC_PWR_PORT, NFC_PWR_PIN);
  GPIO_PinModeSet(NFC_SCL_PORT, NFC_SCL_PIN, gpioModeInput, 1);
  GPIO_PinModeSet(NFC_SDA_PORT, NFC_SDA_PIN, gpioModeInput, 1);
  GPIO_PinModeSet(NFC_PWR_PORT, NFC_PWR_PIN, gpioModeInput, 1);
}

static void od_flash_enter_deep_sleep(void)
{
  enum {
    FLASH_MOSI_PORT = gpioPortC, FLASH_MOSI_PIN = 1,
    FLASH_SCK_PORT = gpioPortC, FLASH_SCK_PIN = 2,
    FLASH_CS_PORT = gpioPortC, FLASH_CS_PIN = 3
  };
  uint8_t cmd = 0xB9u;

  GPIO_PinModeSet(FLASH_MOSI_PORT, FLASH_MOSI_PIN, gpioModePushPull, 0);
  GPIO_PinModeSet(FLASH_SCK_PORT, FLASH_SCK_PIN, gpioModePushPull, 0);
  GPIO_PinModeSet(FLASH_CS_PORT, FLASH_CS_PIN, gpioModePushPull, 1);

  GPIO_PinOutClear(FLASH_CS_PORT, FLASH_CS_PIN);
  for (uint8_t bit = 0; bit < 8u; bit++) {
    bool one = (cmd & 0x80u) != 0u;
    if (one) {
      GPIO_PinOutSet(FLASH_MOSI_PORT, FLASH_MOSI_PIN);
    } else {
      GPIO_PinOutClear(FLASH_MOSI_PORT, FLASH_MOSI_PIN);
    }
    cmd <<= 1;
    sl_udelay_wait(1);
    GPIO_PinOutSet(FLASH_SCK_PORT, FLASH_SCK_PIN);
    sl_udelay_wait(1);
    GPIO_PinOutClear(FLASH_SCK_PORT, FLASH_SCK_PIN);
  }
  GPIO_PinOutSet(FLASH_CS_PORT, FLASH_CS_PIN);
  sl_udelay_wait(30);
}

static void od_init_aux_peripherals(void)
{
  /* Hardcoded board wiring used for low-power pin parking at boot. */
  enum {
    NFC_PWR_PORT = gpioPortD, NFC_PWR_PIN = 0,
    NFC_SCL_PORT = gpioPortD, NFC_SCL_PIN = 1,
    FLASH_CS_PORT = gpioPortC, FLASH_CS_PIN = 3,
    NFC_SDA_PORT = gpioPortD, NFC_SDA_PIN = 3
  };

  CMU_ClockEnable(cmuClock_GPIO, true);

  od_nfc_init_sequence();

  od_flash_enter_deep_sleep();

  /* Park external flash SPI lines in passive states. */
  GPIO_PinModeSet(gpioPortC, 1, gpioModeInputPull, 1);
  GPIO_PinModeSet(gpioPortC, 2, gpioModeInputPull, 0);
  GPIO_PinModeSet(FLASH_CS_PORT, FLASH_CS_PIN, gpioModeInputPull, 1);
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
  od_park_display_pins_from_config();
  opendisplay_led_init();
  opendisplay_display_boot_apply();
}

static void od_apply_idle_advertising_timing(uint8_t adv_handle)
{
  sl_status_t sc;

  if (adv_handle == 0xFFu) {
    return;
  }
  sc = sl_bt_advertiser_set_timing(adv_handle, OD_ADV_INTERVAL_IDLE_SLOTS, OD_ADV_INTERVAL_IDLE_SLOTS, 0, 0);
  if (sc != SL_STATUS_OK) {
    printf("[OD] advertiser_set_timing sc=0x%04lX\r\n", (unsigned long)sc);
  }
  app_assert_status(sc);
}

/* Undirected connectable legacy adv stops when a central connects; restart so other scanners still see us. */
static void restart_connectable_advertising(void)
{
  sl_status_t sc;

  if (s_adv_handle == 0xFFu) {
    return;
  }
  od_apply_idle_advertising_timing(s_adv_handle);
  sc = sl_bt_legacy_advertiser_start(s_adv_handle, sl_bt_legacy_advertiser_connectable);
  if (sc == SL_STATUS_OK) {
    printf("[OD] advertising restarted (stack pauses adv while connected)\r\n");
  } else {
    printf("[OD] adv restart after connect sc=0x%04lX (expected if no free conn slots)\r\n",
           (unsigned long)sc);
  }
}

static void chip_id_hex6(char out[7])
{
  uint64_t u = SYSTEM_GetUnique();
  uint32_t id = (uint32_t)(u & 0xFFFFFFu);
  snprintf(out, 7, "%06lX", (unsigned long)id);
}

static void update_msd_payload(void)
{
  float chip_temperature = EMU_TemperatureGet();
  int16_t temp_encoded;
  uint16_t battery_voltage_10mv = 0u;
  uint32_t now_ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
  bool measure_batt = (s_batt_voltage_mv_cache == 0u)
                      || ((now_ms - s_last_batt_measure_ms) > OD_MSD_UPDATE_INTERVAL_MS);
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
  uint16_t ch_ver;
  sl_bt_uuid_16_t uuid_svc = { .data = { 0x46, 0x24 } };
  sl_bt_uuid_16_t uuid_pipe = { .data = { 0x46, 0x24 } };
  sl_bt_uuid_16_t uuid_ver = { .data = { 0x03, 0x00 } };
  uint8_t ver_init[2] = { (uint8_t)(OD_APP_VERSION & 0xFFu), (uint8_t)((OD_APP_VERSION >> 8) & 0xFFu) };
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

  sc = sl_bt_gattdb_add_uuid16_characteristic(session,
                                              svc,
                                              SL_BT_GATTDB_CHARACTERISTIC_READ,
                                              0,
                                              0,
                                              uuid_ver,
                                              sl_bt_gattdb_fixed_length_value,
                                              2,
                                              sizeof(ver_init),
                                              ver_init,
                                              &ch_ver);
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
  g_od_appver_char = ch_ver;
  (void)g_od_appver_char;
  return SL_STATUS_OK;
}

static void build_and_apply_adv(uint8_t adv_set, const char *name)
{
  uint8_t adv[31];
  uint8_t sr[31];
  size_t ai = 0;
  size_t si = 0;
  size_t nl = strlen(name);
  sl_status_t sc;

  update_msd_payload();

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
  printf("[OD] GATT 0x2446 ok, pipe_char=%u appver=%u\r\n",
         (unsigned)g_od_pipe_char, (unsigned)g_od_appver_char);
  opendisplay_pipe_set_characteristic(g_od_pipe_char);

  if (loadGlobalConfig(&s_od_global_config)) {
    printf("[OD] config: loaded displays=%u leds=%u ver=%u.%u\r\n",
           (unsigned)s_od_global_config.display_count,
           (unsigned)s_od_global_config.led_count,
           (unsigned)s_od_global_config.version,
           (unsigned)s_od_global_config.minor_version);
  } else {
    printf("[OD] config: none or invalid (defaults)\r\n");
  }
  od_buttons_init_from_config();
  od_park_display_pins_from_config();
  opendisplay_led_init();
  opendisplay_display_boot_apply();

  build_and_apply_adv(advertising_set_handle, s_dev_name);
  printf("[OD] advertising + scan rsp set\r\n");

  od_apply_idle_advertising_timing(advertising_set_handle);

  sc = sl_bt_legacy_advertiser_start(advertising_set_handle, sl_bt_legacy_advertiser_connectable);
  if (sc != SL_STATUS_OK) {
    printf("[OD] legacy_advertiser_start sc=0x%04lX\r\n", (unsigned long)sc);
  }
  app_assert_status(sc);
  printf("[OD] advertising started (~1 s interval while idle)\r\n");
}

void opendisplay_ble_restart_advertising(uint8_t advertising_set_handle)
{
  build_and_apply_adv(advertising_set_handle, s_dev_name);
  od_apply_idle_advertising_timing(advertising_set_handle);
  app_assert_status(sl_bt_legacy_advertiser_start(advertising_set_handle,
                                                  sl_bt_legacy_advertiser_connectable));
}

void opendisplay_ble_on_event(sl_bt_msg_t *evt)
{
  opendisplay_pipe_handle_gatt_event(evt);

  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_connection_opened_id:
      g_connection = evt->data.evt_connection_opened.connection;
      reboot_flag = 0u;
      printf("[OD] connection opened handle=%u\r\n", (unsigned)g_connection);
      restart_connectable_advertising();
      break;
    case sl_bt_evt_connection_closed_id:
      g_connection = 0xFF;
      opendisplay_pipe_on_connection_closed();
      printf("[OD] connection closed reason=0x%02X\r\n",
             (unsigned)evt->data.evt_connection_closed.reason);
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
  uint32_t now_ms = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
  if (od_process_button_event(now_ms) && s_adv_handle != 0xFFu && g_connection == 0xFFu) {
    build_and_apply_adv(s_adv_handle, s_dev_name);
  }
  if ((now_ms - s_last_msd_refresh_ms) >= OD_MSD_UPDATE_INTERVAL_MS) {
    s_last_msd_refresh_ms = now_ms;
    if (s_adv_handle != 0xFFu && g_connection == 0xFFu) {
      build_and_apply_adv(s_adv_handle, s_dev_name);
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
