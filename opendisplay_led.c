#include "opendisplay_led.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "opendisplay_structs.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "sl_sleeptimer.h"
#include "sl_udelay.h"
#include <stdbool.h>
#include <string.h>

#define LED_FLAG_INVERT_RED    0x01u
#define LED_FLAG_INVERT_GREEN  0x02u
#define LED_FLAG_INVERT_BLUE   0x04u
#define LED_FLAG_INVERT_LED4   0x08u

#define LED_DELAY_FACTOR_MS    100u
#define LED_PWM_DELAY_US       100u

typedef enum {
  LED_PHASE_IDLE = 0,
  LED_PHASE_GROUP,
  LED_PHASE_LOOP1,
  LED_PHASE_LOOP1_DELAY,
  LED_PHASE_INTER1_DELAY,
  LED_PHASE_LOOP2,
  LED_PHASE_LOOP2_DELAY,
  LED_PHASE_INTER2_DELAY,
  LED_PHASE_LOOP3,
  LED_PHASE_LOOP3_DELAY,
  LED_PHASE_INTER3_DELAY,
} led_phase_t;

static struct {
  bool active;
  uint8_t instance;
  struct LedConfig *led;
  uint8_t brightness;
  uint8_t c1;
  uint8_t c2;
  uint8_t c3;
  uint8_t loop1delay;
  uint8_t loop2delay;
  uint8_t loop3delay;
  uint8_t loopcnt1;
  uint8_t loopcnt2;
  uint8_t loopcnt3;
  uint8_t ildelay1;
  uint8_t ildelay2;
  uint8_t ildelay3;
  uint8_t grouprepeats;
  uint8_t group_pos;
  uint8_t i1;
  uint8_t i2;
  uint8_t i3;
  led_phase_t phase;
  bool waiting_delay;
} s_run;

static sl_sleeptimer_timer_handle_t s_led_timer;
static volatile bool s_timer_due;

static bool od_led_pin_decode(uint8_t v, GPIO_Port_TypeDef *port_out, uint8_t *pin_out)
{
  if (v == GPIO_PIN_UNUSED) {
    return false;
  }
  unsigned pr = (unsigned)(v >> 4) & 0x0Fu;
  unsigned pn = (unsigned)(v & 0x0Fu);
  if (pr > (unsigned)GPIO_PORT_MAX) {
    return false;
  }
  if (pn > 15u) {
    return false;
  }
  *port_out = (GPIO_Port_TypeDef)(gpioPortA + pr);
  *pin_out = (uint8_t)pn;
  return true;
}

static void od_gpio_mode_push_pull(uint8_t cfg)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (!od_led_pin_decode(cfg, &port, &pin)) {
    return;
  }
  GPIO_PinModeSet(port, pin, gpioModePushPull, 0);
}

static void od_gpio_write(uint8_t cfg, bool level_high)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (!od_led_pin_decode(cfg, &port, &pin)) {
    return;
  }
  if (level_high) {
    GPIO_PinOutSet(port, pin);
  } else {
    GPIO_PinOutClear(port, pin);
  }
}

static void od_led_all_off(const struct LedConfig *led)
{
  bool inv_r = (led->led_flags & LED_FLAG_INVERT_RED) != 0u;
  bool inv_g = (led->led_flags & LED_FLAG_INVERT_GREEN) != 0u;
  bool inv_b = (led->led_flags & LED_FLAG_INVERT_BLUE) != 0u;

  if (led->led_1_r != GPIO_PIN_UNUSED) {
    od_gpio_write(led->led_1_r, inv_r);
  }
  if (led->led_2_g != GPIO_PIN_UNUSED) {
    od_gpio_write(led->led_2_g, inv_g);
  }
  if (led->led_3_b != GPIO_PIN_UNUSED) {
    od_gpio_write(led->led_3_b, inv_b);
  }
}

static void od_flash_led(const struct LedConfig *led, uint8_t color, uint8_t brightness)
{
  uint8_t led_red = led->led_1_r;
  uint8_t led_green = led->led_2_g;
  uint8_t led_blue = led->led_3_b;
  bool inv_r = (led->led_flags & LED_FLAG_INVERT_RED) != 0u;
  bool inv_g = (led->led_flags & LED_FLAG_INVERT_GREEN) != 0u;
  bool inv_b = (led->led_flags & LED_FLAG_INVERT_BLUE) != 0u;
  uint8_t colorred = (color >> 5) & 0x07u;
  uint8_t colorgreen = (color >> 2) & 0x07u;
  uint8_t colorblue = color & 0x03u;

  for (uint16_t i = 0; i < brightness; i++) {
    od_gpio_write(led_red, inv_r ? !(colorred >= 7u) : (colorred >= 7u));
    od_gpio_write(led_green, inv_g ? !(colorgreen >= 7u) : (colorgreen >= 7u));
    od_gpio_write(led_blue, inv_b ? !(colorblue >= 3u) : (colorblue >= 3u));
    sl_udelay_wait(LED_PWM_DELAY_US);
    od_gpio_write(led_red, inv_r ? !(colorred >= 1u) : (colorred >= 1u));
    od_gpio_write(led_green, inv_g ? !(colorgreen >= 1u) : (colorgreen >= 1u));
    sl_udelay_wait(LED_PWM_DELAY_US);
    od_gpio_write(led_red, inv_r ? !(colorred >= 6u) : (colorred >= 6u));
    od_gpio_write(led_green, inv_g ? !(colorgreen >= 6u) : (colorgreen >= 6u));
    od_gpio_write(led_blue, inv_b ? !(colorblue >= 1u) : (colorblue >= 1u));
    sl_udelay_wait(LED_PWM_DELAY_US);
    od_gpio_write(led_red, inv_r ? !(colorred >= 2u) : (colorred >= 2u));
    od_gpio_write(led_green, inv_g ? !(colorgreen >= 2u) : (colorgreen >= 2u));
    sl_udelay_wait(LED_PWM_DELAY_US);
    od_gpio_write(led_red, inv_r ? !(colorred >= 5u) : (colorred >= 5u));
    od_gpio_write(led_green, inv_g ? !(colorgreen >= 5u) : (colorgreen >= 5u));
    sl_udelay_wait(LED_PWM_DELAY_US);
    od_gpio_write(led_red, inv_r ? !(colorred >= 3u) : (colorred >= 3u));
    od_gpio_write(led_green, inv_g ? !(colorgreen >= 3u) : (colorgreen >= 3u));
    od_gpio_write(led_blue, inv_b ? !(colorblue >= 2u) : (colorblue >= 2u));
    sl_udelay_wait(LED_PWM_DELAY_US);
    od_gpio_write(led_red, inv_r ? !(colorred >= 4u) : (colorred >= 4u));
    od_gpio_write(led_green, inv_g ? !(colorgreen >= 4u) : (colorgreen >= 4u));
    sl_udelay_wait(LED_PWM_DELAY_US);
    od_gpio_write(led_red, inv_r ? true : false);
    od_gpio_write(led_green, inv_g ? true : false);
    od_gpio_write(led_blue, inv_b ? true : false);
  }
}

static void led_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  s_timer_due = true;
}

static void led_timer_stop(void)
{
  (void)sl_sleeptimer_stop_timer(&s_led_timer);
  s_timer_due = false;
  s_run.waiting_delay = false;
}

static void led_schedule_delay_ms(uint16_t ms)
{
  if (ms == 0u) {
    s_timer_due = true;
    s_run.waiting_delay = true;
    return;
  }
  led_timer_stop();
  s_timer_due = false;
  s_run.waiting_delay = true;
  (void)sl_sleeptimer_start_timer_ms(&s_led_timer, ms, led_timer_cb, NULL, 0u, 0u);
}

static void led_run_finish(void)
{
  if (s_run.led != NULL) {
    od_led_all_off(s_run.led);
  }
  led_timer_stop();
  memset(&s_run, 0, sizeof(s_run));
}

static void led_load_config(struct LedConfig *led)
{
  uint8_t *ledcfg = led->reserved;

  s_run.led = led;
  s_run.brightness = (uint8_t)(((ledcfg[0] >> 4) & 0x0Fu) + 1u);
  s_run.c1 = ledcfg[1];
  s_run.c2 = ledcfg[4];
  s_run.c3 = ledcfg[7];
  s_run.loop1delay = (uint8_t)((ledcfg[2] >> 4) & 0x0Fu);
  s_run.loop2delay = (uint8_t)((ledcfg[5] >> 4) & 0x0Fu);
  s_run.loop3delay = (uint8_t)((ledcfg[8] >> 4) & 0x0Fu);
  s_run.loopcnt1 = (uint8_t)(ledcfg[2] & 0x0Fu);
  s_run.loopcnt2 = (uint8_t)(ledcfg[5] & 0x0Fu);
  s_run.loopcnt3 = (uint8_t)(ledcfg[8] & 0x0Fu);
  s_run.ildelay1 = ledcfg[3];
  s_run.ildelay2 = ledcfg[6];
  s_run.ildelay3 = ledcfg[9];
  s_run.grouprepeats = (uint8_t)(ledcfg[10] + 1u);
  s_run.group_pos = 0;
  s_run.i1 = 0;
  s_run.i2 = 0;
  s_run.i3 = 0;
  s_run.phase = LED_PHASE_GROUP;
  s_run.waiting_delay = false;
}

static void led_run_stop_internal(bool clear_mode)
{
  struct LedConfig *led = s_run.led;

  led_timer_stop();
  if (led != NULL) {
    od_led_all_off(led);
    if (clear_mode) {
      led->reserved[0] = 0x00u;
    }
  }
  memset(&s_run, 0, sizeof(s_run));
}

static bool led_delay_ready(void)
{
  if (!s_run.waiting_delay) {
    return true;
  }
  if (!s_timer_due) {
    return false;
  }
  s_timer_due = false;
  s_run.waiting_delay = false;
  return true;
}

static void led_run_step(void)
{
  struct LedConfig *led;
  uint8_t mode;

  if (!s_run.active || s_run.led == NULL) {
    return;
  }
  led = s_run.led;
  mode = (uint8_t)(led->reserved[0] & 0x0Fu);
  if (mode != 1u) {
    led_run_finish();
    return;
  }

  for (;;) {
    if (!s_run.active) {
      return;
    }

    switch (s_run.phase) {
      case LED_PHASE_GROUP:
        if (s_run.group_pos >= s_run.grouprepeats && s_run.grouprepeats != 255u) {
          led->reserved[0] = 0x00u;
          led_run_finish();
          return;
        }
        s_run.i1 = 0;
        s_run.i2 = 0;
        s_run.i3 = 0;
        s_run.phase = LED_PHASE_LOOP1;
        break;

      case LED_PHASE_LOOP1:
        if (s_run.i1 >= s_run.loopcnt1) {
          if (s_run.ildelay1 > 0u) {
            s_run.phase = LED_PHASE_INTER1_DELAY;
            led_schedule_delay_ms((uint16_t)(s_run.ildelay1 * LED_DELAY_FACTOR_MS));
            return;
          }
          s_run.phase = LED_PHASE_LOOP2;
          break;
        }
        od_flash_led(led, s_run.c1, s_run.brightness);
        s_run.i1++;
        if (s_run.loop1delay > 0u) {
          s_run.phase = LED_PHASE_LOOP1_DELAY;
          led_schedule_delay_ms((uint16_t)(s_run.loop1delay * LED_DELAY_FACTOR_MS));
          return;
        }
        break;

      case LED_PHASE_LOOP1_DELAY:
        if (!led_delay_ready()) {
          return;
        }
        s_run.phase = LED_PHASE_LOOP1;
        break;

      case LED_PHASE_INTER1_DELAY:
        if (!led_delay_ready()) {
          return;
        }
        s_run.phase = LED_PHASE_LOOP2;
        break;

      case LED_PHASE_LOOP2:
        if (s_run.i2 >= s_run.loopcnt2) {
          if (s_run.ildelay2 > 0u) {
            s_run.phase = LED_PHASE_INTER2_DELAY;
            led_schedule_delay_ms((uint16_t)(s_run.ildelay2 * LED_DELAY_FACTOR_MS));
            return;
          }
          s_run.phase = LED_PHASE_LOOP3;
          break;
        }
        od_flash_led(led, s_run.c2, s_run.brightness);
        s_run.i2++;
        if (s_run.loop2delay > 0u) {
          s_run.phase = LED_PHASE_LOOP2_DELAY;
          led_schedule_delay_ms((uint16_t)(s_run.loop2delay * LED_DELAY_FACTOR_MS));
          return;
        }
        break;

      case LED_PHASE_LOOP2_DELAY:
        if (!led_delay_ready()) {
          return;
        }
        s_run.phase = LED_PHASE_LOOP2;
        break;

      case LED_PHASE_INTER2_DELAY:
        if (!led_delay_ready()) {
          return;
        }
        s_run.phase = LED_PHASE_LOOP3;
        break;

      case LED_PHASE_LOOP3:
        if (s_run.i3 >= s_run.loopcnt3) {
          if (s_run.ildelay3 > 0u) {
            s_run.phase = LED_PHASE_INTER3_DELAY;
            led_schedule_delay_ms((uint16_t)(s_run.ildelay3 * LED_DELAY_FACTOR_MS));
            return;
          }
          s_run.group_pos++;
          s_run.phase = LED_PHASE_GROUP;
          break;
        }
        od_flash_led(led, s_run.c3, s_run.brightness);
        s_run.i3++;
        if (s_run.loop3delay > 0u) {
          s_run.phase = LED_PHASE_LOOP3_DELAY;
          led_schedule_delay_ms((uint16_t)(s_run.loop3delay * LED_DELAY_FACTOR_MS));
          return;
        }
        break;

      case LED_PHASE_LOOP3_DELAY:
        if (!led_delay_ready()) {
          return;
        }
        s_run.phase = LED_PHASE_LOOP3;
        break;

      case LED_PHASE_INTER3_DELAY:
        if (!led_delay_ready()) {
          return;
        }
        s_run.group_pos++;
        s_run.phase = LED_PHASE_GROUP;
        break;

      default:
        led_run_finish();
        return;
    }
  }
}

void opendisplay_led_init(void)
{
  const struct GlobalConfig *gc = opendisplay_get_global_config();

  CMU_ClockEnable(cmuClock_GPIO, true);

  if (gc == NULL || !gc->loaded || gc->led_count == 0u) {
    return;
  }

  for (uint8_t i = 0; i < gc->led_count; i++) {
    const struct LedConfig *led = &gc->leds[i];
    bool inv_r = (led->led_flags & LED_FLAG_INVERT_RED) != 0u;
    bool inv_g = (led->led_flags & LED_FLAG_INVERT_GREEN) != 0u;
    bool inv_b = (led->led_flags & LED_FLAG_INVERT_BLUE) != 0u;
    bool inv_4 = (led->led_flags & LED_FLAG_INVERT_LED4) != 0u;

    if (led->led_1_r != GPIO_PIN_UNUSED) {
      od_gpio_mode_push_pull(led->led_1_r);
      od_gpio_write(led->led_1_r, inv_r);
    }
    if (led->led_2_g != GPIO_PIN_UNUSED) {
      od_gpio_mode_push_pull(led->led_2_g);
      od_gpio_write(led->led_2_g, inv_g);
    }
    if (led->led_3_b != GPIO_PIN_UNUSED) {
      od_gpio_mode_push_pull(led->led_3_b);
      od_gpio_write(led->led_3_b, inv_b);
    }
    if (led->led_4 != GPIO_PIN_UNUSED) {
      od_gpio_mode_push_pull(led->led_4);
      od_gpio_write(led->led_4, inv_4);
    }
  }
}

int opendisplay_led_activate(uint8_t instance, const uint8_t *rest, uint16_t rest_len)
{
  struct GlobalConfig *gc = (struct GlobalConfig *)opendisplay_get_global_config();
  struct LedConfig *led;
  uint8_t mode;

  if (gc == NULL || !gc->loaded || instance >= gc->led_count) {
    return 2;
  }

  led = &gc->leds[instance];
  if (rest_len >= 12u) {
    memcpy(led->reserved, rest, 12);
  }

  mode = (uint8_t)(led->reserved[0] & 0x0Fu);
  if (mode != 1u) {
    opendisplay_led_stop(0, false);
    return 0;
  }

  opendisplay_led_stop(0, false);

  s_run.active = true;
  s_run.instance = instance;
  led_load_config(led);
  led_run_step();
  return 0;
}

int opendisplay_led_stop(uint8_t instance, bool instance_given)
{
  if (!s_run.active) {
    return 0;
  }
  if (instance_given && instance != s_run.instance) {
    return 2;
  }
  led_run_stop_internal(true);
  return 0;
}

void opendisplay_led_process(void)
{
  if (!s_run.active) {
    return;
  }
  led_run_step();
}

bool opendisplay_led_is_active(void)
{
  return s_run.active;
}
