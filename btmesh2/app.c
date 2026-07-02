/***************************************************************************//**
 * Node 2 — mesh CLIENT side. Same peripherals as Node 1, mesh roles flipped.
 ******************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sl_main_init.h"
#include "sl_status.h"
#include "sl_udelay.h"

#include "app.h"
#include "app_assert.h"
#include "app_log.h"
#include "app_timer.h"

#include "sl_btmesh_api.h"
#include "sl_bluetooth.h"
#include "sl_btmesh.h"
#include "sl_bt_api.h"

#include "sl_simple_button.h"
#include "sl_simple_button_instances.h"
#include "sl_simple_led.h"
#include "sl_simple_led_instances.h"
#include "app_button_press.h"

#include "sl_component_catalog.h"

#ifdef SL_CATALOG_BTMESH_FACTORY_RESET_PRESENT
#include "sl_btmesh_factory_reset.h"
#endif

#include "sl_btmesh_sensor_client.h"

// ---- constants ----
#define APP_LED_BLINKING_TIMEOUT          250
#define NAME_BUF_LEN                       20
#define NO_CALLBACK_DATA                  NULL

#define MESH_GENERIC_ONOFF_CLIENT_MODEL_ID 0x1001
#define MESH_GENERIC_ONOFF_TYPE            0   // generic on_off state type

// ---- module state ----
static app_timer_t app_led_blinking_timer;
static bool        init_done = false;
static uint8_t     g_tid     = 0;     // transaction id for OnOff client
static uint8_t     g_onoff   = 0;     // last value sent (toggle target)

// ---- forward decls ----
static void app_led_blinking_timer_cb(app_timer_t *handle, void *data);
static void set_device_name(uuid_128 *uuid);

extern void sensors_start(void);
extern sl_status_t vendor_model_init(void);

extern bool vendor_model_on_event(sl_btmesh_msg_t *evt);


/*******************************************************************************
 * Pin-share helpers (BRD2703A shares button/LED pins) — unchanged from Node 1
 ******************************************************************************/


void app_button_press_cb(uint8_t button, uint8_t duration)
{
  (void)duration;
  if (!init_done) return;

  if (button == APP_BUTTON_PRESS_PRESSED_DOWN) {
    g_onoff ^= 1;
    uint8_t params[1] = { g_onoff };
    sl_status_t sc = sl_btmesh_generic_client_publish(
        MESH_GENERIC_ONOFF_CLIENT_MODEL_ID,
        0,                    // elem_index
        g_tid++,              // transaction id (must increment)
        0,                    // transition_ms
        0,                    // delay_ms
        0x00,                 // flags
        MESH_GENERIC_ONOFF_TYPE,
        sizeof(params),
        params);
    app_log("OnOff client: publish %u sc=0x%lx" APP_LOG_NL, g_onoff, sc);
  }
}
void app_change_buttons_to_leds(void)
{
  app_button_press_disable();
  sl_simple_button_disable(&sl_button_btn0);
  sl_simple_led_init(sl_led_led0.context);
#if SL_SIMPLE_BUTTON_COUNT >= 2
  sl_simple_button_disable(&sl_button_btn1);
#endif
#if SL_SIMPLE_LED_COUNT >= 2
  sl_simple_led_init(sl_led_led1.context);
#endif
}

void app_change_leds_to_buttons(void)
{
  sl_simple_button_enable(&sl_button_btn0);
#if SL_SIMPLE_BUTTON_COUNT >= 2
  sl_simple_button_enable(&sl_button_btn1);
#endif
  sl_udelay_wait(1000);
  app_button_press_enable();
}

bool app_handle_reset_conditions(void)
{
#ifdef SL_CATALOG_BTMESH_FACTORY_RESET_PRESENT
  if (sl_simple_button_get_state(&sl_button_btn0) == SL_SIMPLE_BUTTON_PRESSED) {
    app_button_press_disable();
    sl_btmesh_initiate_full_reset();
    return false;
  }
#if SL_SIMPLE_BUTTON_COUNT >= 2
  if (sl_simple_button_get_state(&sl_button_btn1) == SL_SIMPLE_BUTTON_PRESSED) {
    app_button_press_disable();
    sl_btmesh_initiate_node_reset();
    return false;
  }
#endif
#endif
  return true;
}

/*******************************************************************************
 * App init / process action
 ******************************************************************************/
void app_init_early(void) { }

void app_init(void)
{
  app_log("Mesh Node 2 (client) boot" APP_LOG_NL);
  app_change_buttons_to_leds();
  app_change_leds_to_buttons();
  app_handle_reset_conditions();

  sensors_start();   // local sensors run — ready for the bridge later
}

void app_process_action(void)
{
  if (app_is_process_required()) { }
}

/*******************************************************************************
 * Button handler — drives OnOff client + SET_PERIOD test
 *   BTN0 short press -> publish OnOff toggle (lights Node 1's LED via mesh)
 *   BTN1 short press -> send SET_PERIOD 10000 ms to Node 1 unicast
 ******************************************************************************/

/*******************************************************************************
 * Provisioning lifecycle (identical to Node 1)
 ******************************************************************************/
void sl_btmesh_provisionee_on_init(sl_status_t result)
{
  if (SL_STATUS_OK != result) {
    app_log("provisionee init failed: 0x%lx" APP_LOG_NL, result);
    return;
  }
  uuid_128 uuid;
  sl_status_t sc = sl_btmesh_node_get_uuid(&uuid);
  app_assert_status_f(sc, "Failed to get UUID");
  set_device_name(&uuid);

   sc = sl_btmesh_generic_client_init_on_off();
  app_log("generic on off code 0x%lx" APP_LOG_NL, sc);
}

void sl_btmesh_on_node_provisioning_started(uint16_t result)
{
  app_log("Provisioning STARTED (result: 0x%04x)" APP_LOG_NL, result);
  app_change_buttons_to_leds();
  sl_status_t sc = app_timer_start(&app_led_blinking_timer,
                                   APP_LED_BLINKING_TIMEOUT,
                                   app_led_blinking_timer_cb,
                                   NO_CALLBACK_DATA, true);
  app_assert_status_f(sc, "Failed to start periodic timer");
}

void sl_btmesh_on_node_provisioning_failed(uint16_t result)
{
  app_log("Provisioning FAILED (result: 0x%04x), rebooting" APP_LOG_NL, result);
  sl_udelay_wait(2000000);
  sl_bt_system_reboot();
}

void sl_btmesh_on_node_provisioned(uint16_t address, uint32_t iv_index)
{
  sl_status_t sc = app_timer_stop(&app_led_blinking_timer);
  app_assert_status_f(sc, "Failed to stop periodic timer");
  init_done = true;
  sl_simple_led_turn_off(sl_led_led0.context);
#if SL_SIMPLE_LED_COUNT >= 2
  sl_simple_led_turn_off(sl_led_led1.context);
#endif
  app_change_leds_to_buttons();
  app_log("Provisioned: address=0x%04x iv_index=0x%lx" APP_LOG_NL,
          address, iv_index);
}

void sl_btmesh_on_provision_init_status(bool provisioned,
                                        uint16_t address,
                                        uint32_t iv_index)
{
  if (provisioned) {
    init_done = true;   // critical: button cb gates on this
    app_log("Boot: already provisioned, address=0x%04x iv_index=0x%lx" APP_LOG_NL,
            address, iv_index);
  } else {
    app_log("Boot: unprovisioned, beaconing..." APP_LOG_NL);
  }
}

/*******************************************************************************
 * Helpers
 ******************************************************************************/
static void set_device_name(uuid_128 *uuid)
{
  char name[NAME_BUF_LEN];
  snprintf(name, sizeof(name), "Therm %02x%02x", uuid->data[14], uuid->data[15]);
  app_log("Device name: '%s'" APP_LOG_NL, name);
}

static void app_led_blinking_timer_cb(app_timer_t *handle, void *data)
{
  (void)handle; (void)data;
  if (!init_done) {
    sl_simple_led_toggle(sl_led_led0.context);
#if SL_SIMPLE_LED_COUNT >= 2
    sl_simple_led_toggle(sl_led_led1.context);
#endif
  }
}

/*******************************************************************************
 * BT stack event handler
 ******************************************************************************/
void sl_bt_on_event(struct sl_bt_msg *evt)
{
  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_connection_opened_id:
      app_log("BT connection opened" APP_LOG_NL); break;
    case sl_bt_evt_connection_closed_id:
      app_log("BT connection closed" APP_LOG_NL); break;
    default: break;
  }
}

/*******************************************************************************
 * BT Mesh stack event handler — vendor model gets first crack
 ******************************************************************************/
void sl_btmesh_on_event(sl_btmesh_msg_t *evt)
{

  app_log("mesh evt: 0x%08lx" APP_LOG_NL, (unsigned long)SL_BT_MSG_ID(evt->header));
  if (vendor_model_on_event(evt)) return;

  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_btmesh_evt_node_initialized_id:
      app_log("Mesh: node_initialized, provisioned=%d" APP_LOG_NL,
              evt->data.evt_node_initialized.provisioned);
      //sl_btmesh_sensor_client_init();
      vendor_model_init();
      break;

    case sl_btmesh_evt_sensor_client_status_id: {
      sl_btmesh_evt_sensor_client_status_t *st =
        &evt->data.evt_sensor_client_status;

      app_log("Sensor client: status from 0x%04x len=%u" APP_LOG_NL,
                    st->client_address, st->sensor_data.len);
      //int32_t t = 0, rh = 0, lux = 0;
      //parse_sensor_status_tlv(&st->sensor_data, &t, &rh, &lux);  // see below
      //bridge_publish_sample(0x02, 200, 69, 420);   // 0x02 = remote
      break;
    }

    default:
      break;
  }
}
