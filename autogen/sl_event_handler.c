#include "sl_event_handler.h"

#include "sl_clock_manager.h"
#include "sl_rail_util_compatible_pa.h"
#include "sl_rail_util_power_manager_init.h"
#include "sl_rail_util_pti.h"
#include "app.h"
#include "sl_bluetooth.h"
#include "sl_gpio.h"
#include "sl_mbedtls.h"
#include "psa/crypto.h"
#include "sl_se_manager.h"
#include "sli_protocol_crypto.h"
#include "sli_crypto.h"
#include "nvm3_default.h"

void sli_driver_permanent_allocation(void)
{
}

void sli_service_permanent_allocation(void)
{
}

void sli_stack_permanent_allocation(void)
{
  sli_bt_stack_permanent_allocation();
}

void sli_internal_permanent_allocation(void)
{
}

void sl_platform_init(void)
{
  sl_clock_manager_runtime_init();
  nvm3_initDefault();
}

void sli_internal_init_early(void)
{
  app_init_bt();
}

void sl_driver_init(void)
{
  sl_gpio_init();
}

void sl_service_init(void)
{
  sl_mbedtls_init();
  psa_crypto_init();
  sl_se_init();
  sli_protocol_crypto_init();
  sli_crypto_init();
  sli_aes_seed_mask();
}

void sl_stack_init(void)
{
  sl_rail_util_pa_init();
  sl_rail_util_power_manager_init();
  sl_rail_util_pti_init();
  sli_bt_stack_functional_init();
}

void sl_internal_app_init(void)
{
}

void sli_platform_process_action(void)
{
}

void sli_service_process_action(void)
{
}

void sli_stack_process_action(void)
{
  sl_bt_step();
}

void sli_internal_app_process_action(void)
{
}

