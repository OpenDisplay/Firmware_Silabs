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

static uint8_t s_active_instance = 0xFFu;
static bool s_flash_active;
static uint8_t s_flash_position;

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

static void od_led_flash_logic(struct LedConfig *led)
{
  uint8_t *ledcfg = led->reserved;
  uint8_t brightness = (uint8_t)(((ledcfg[0] >> 4) & 0x0Fu) + 1u);
  uint8_t mode = (uint8_t)(ledcfg[0] & 0x0Fu);

  if (mode != 1u) {
    return;
  }

  uint8_t c1 = ledcfg[1];
  uint8_t c2 = ledcfg[4];
  uint8_t c3 = ledcfg[7];
  uint8_t loop1delay = (uint8_t)((ledcfg[2] >> 4) & 0x0Fu);
  uint8_t loop2delay = (uint8_t)((ledcfg[5] >> 4) & 0x0Fu);
  uint8_t loop3delay = (uint8_t)((ledcfg[8] >> 4) & 0x0Fu);
  uint8_t loopcnt1 = (uint8_t)(ledcfg[2] & 0x0Fu);
  uint8_t loopcnt2 = (uint8_t)(ledcfg[5] & 0x0Fu);
  uint8_t loopcnt3 = (uint8_t)(ledcfg[8] & 0x0Fu);
  uint8_t ildelay1 = ledcfg[3];
  uint8_t ildelay2 = ledcfg[6];
  uint8_t ildelay3 = ledcfg[9];
  uint8_t grouprepeats = (uint8_t)(ledcfg[10] + 1u);

  while (s_flash_active) {
    if (s_flash_position >= grouprepeats && grouprepeats != 255u) {
      ledcfg[0] = 0x00u;
      s_flash_position = 0;
      break;
    }

    for (uint8_t i = 0; i < loopcnt1; i++) {
      od_flash_led(led, c1, brightness);
      if (loop1delay > 0u) {
        sl_sleeptimer_delay_millisecond((uint16_t)(loop1delay * LED_DELAY_FACTOR_MS));
      }
    }
    if (ildelay1 > 0u) {
      sl_sleeptimer_delay_millisecond((uint16_t)(ildelay1 * LED_DELAY_FACTOR_MS));
    }

    for (uint8_t i = 0; i < loopcnt2; i++) {
      od_flash_led(led, c2, brightness);
      if (loop2delay > 0u) {
        sl_sleeptimer_delay_millisecond((uint16_t)(loop2delay * LED_DELAY_FACTOR_MS));
      }
    }
    if (ildelay2 > 0u) {
      sl_sleeptimer_delay_millisecond((uint16_t)(ildelay2 * LED_DELAY_FACTOR_MS));
    }

    for (uint8_t i = 0; i < loopcnt3; i++) {
      od_flash_led(led, c3, brightness);
      if (loop3delay > 0u) {
        sl_sleeptimer_delay_millisecond((uint16_t)(loop3delay * LED_DELAY_FACTOR_MS));
      }
    }
    if (ildelay3 > 0u) {
      sl_sleeptimer_delay_millisecond((uint16_t)(ildelay3 * LED_DELAY_FACTOR_MS));
    }

    s_flash_position++;
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

  if (gc == NULL || !gc->loaded || instance >= gc->led_count) {
    return 2;
  }

  struct LedConfig *led = &gc->leds[instance];
  uint8_t *ledcfg = led->reserved;

  if (rest_len >= 12u) {
    memcpy(ledcfg, rest, 12);
  }

  s_active_instance = instance;
  s_flash_position = 0;
  s_flash_active = true;
  od_led_flash_logic(led);
  s_flash_active = false;

  return 0;
}
