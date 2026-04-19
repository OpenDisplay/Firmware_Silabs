/***************************************************************************//**
 * @file sl_bt_dynamic_gattdb_config.h
 * @brief Dynamic GATT database feature configuration (matches Simplicity Studio defaults)
 ******************************************************************************/
#ifndef SL_BT_DYNAMIC_GATTDB_CONFIG_H
#define SL_BT_DYNAMIC_GATTDB_CONFIG_H

#define SL_BT_GATTDB_ENABLE_GATT_CACHING       0
#define SL_BT_GATTDB_INCLUDE_STATIC_DATABASE   1

#define SL_BT_CONFIG_USER_DYNAMIC_GATTDBS      1

#ifndef SL_BT_COMPONENT_DYNAMIC_GATTDBS
#define SL_BT_COMPONENT_DYNAMIC_GATTDBS        0
#endif

#define SL_BT_CONFIG_MAX_DYNAMIC_GATTDBS_SUM \
  (SL_BT_CONFIG_USER_DYNAMIC_GATTDBS + SL_BT_COMPONENT_DYNAMIC_GATTDBS)

#endif
