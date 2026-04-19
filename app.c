/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2024 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include "sl_bt_api.h"
#include "sl_main_init.h"
#include "app_assert.h"
#include "app.h"
#include "opendisplay_ble.h"
#include <stdio.h>

// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xff;

// Application Init.
void app_init(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application init code here!                         //
  // This is called once during start-up.                                    //
  /////////////////////////////////////////////////////////////////////////////
}

// Application Process Action.
void app_process_action(void)
{
  opendisplay_ble_process();
  if (app_is_process_required()) {
    /////////////////////////////////////////////////////////////////////////////
    // Put your additional application code here!                              //
    // This is will run each time app_proceed() is called.                     //
    // Do not call blocking functions from here!                               //
    /////////////////////////////////////////////////////////////////////////////
  }
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the default weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      printf("[app] sl_bt_evt_system_boot\r\n");
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      if (sc != SL_STATUS_OK) {
        printf("[app] advertiser_create_set sc=0x%04lX\r\n", (unsigned long)sc);
      }
      app_assert_status(sc);
      opendisplay_ble_on_boot(advertising_set_handle);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      opendisplay_ble_on_event(evt);
      break;

    case sl_bt_evt_connection_closed_id:
      opendisplay_ble_on_event(evt);
      opendisplay_ble_restart_advertising(advertising_set_handle);
      break;

    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////

    // -------------------------------
    default:
      opendisplay_ble_on_event(evt);
      break;
  }
}
