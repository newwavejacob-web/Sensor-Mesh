// bridge.c — custom GATT service that pushes sensor data to SiW917 (central)
#include <stdint.h>
#include <string.h>
#include "sl_status.h"
#include "sl_bt_api.h"
#include "gatt_db.h"
#include "app_log.h"
#include "FreeRTOS.h"
#include "task.h"

#include "sl_simple_button.h"
#include "sl_simple_button_instances.h"
#include "sl_simple_led.h"
#include "sl_simple_led_instances.h"

// Forward decl from sensors.c
extern bool sensors_get_latest(int32_t *t_mC, int32_t *rh_mPct, int32_t *lux_x100);
void bridge_update_period(uint32_t period_ms);

// State
static uint8_t  siw_connection = 0xFF;     // 0xFF = no connection
static bool     notify_enabled = false;
static uint32_t bridge_counter = 0;
static uint8_t  g_tid          = 0;

extern sl_status_t vendor_send_blink_led(uint16_t dst_addr);
extern sl_status_t vendor_send_set_period(uint16_t dst_addr, uint32_t period_ms);
// Pack the latest sensor reading into a 12-byte payload:
//   bytes  0..3   int32_t  temp_mC      (little-endian)
//   bytes  4..7   int32_t  rh_mPct
//   bytes  8..11  int32_t  lux_x100  (counter pushed in 4 bytes after via combined? no — keep 12)
// We'll add counter as a 13th-16th byte in v2 if needed.
static void pack_payload(uint8_t out[13])
{
  int32_t t = 0, rh = 0, lux = 0;
  sensors_get_latest(&t, &rh, &lux);

  out[0] = 0x01;
  memcpy(&out[1], &t,   4);
  memcpy(&out[5], &rh,  4);
  memcpy(&out[9], &lux, 4);
}

// Push a notification if SiW917 is connected and has notifications enabled
 void bridge_notify_sensor(void)
{
  if (siw_connection == 0xFF || !notify_enabled) return;

  uint8_t payload[13];
  pack_payload(payload);

  sl_status_t sc = sl_bt_gatt_server_send_notification(siw_connection,
                                                       gattdb_bridge_sensor_data,
                                                       sizeof(payload),
                                                       payload);
  if (sc != SL_STATUS_OK) {
    app_log("bridge: notify failed 0x%lx" APP_LOG_NL, sc);
  }
}

// Periodic timer — notify every 1 second
#include "app_timer.h"
static app_timer_t bridge_timer;

static void bridge_timer_cb(app_timer_t *handle, void *data)
{
  (void)handle; (void)data;
  bridge_notify_sensor();
  bridge_counter++;
}

// Hook into BT stack events. Call from sl_bt_on_event in app.c.
bool bridge_on_bt_event(sl_bt_msg_t *evt)
{
  switch (SL_BT_MSG_ID(evt->header)) {

    case sl_bt_evt_connection_opened_id: {
      // We don't know yet if this is the phone (mesh proxy) or SiW917.
      // We assume any connection that subscribes to our characteristic IS the SiW917.
      // The phone subscribes to mesh proxy chars, not ours.
      uint8_t handle = evt->data.evt_connection_opened.connection;
      app_log("bridge: connection opened, handle=%u" APP_LOG_NL, handle);
      return false;  // let other handlers see this too
    }

    case sl_bt_evt_connection_closed_id: {
      uint8_t handle = evt->data.evt_connection_closed.connection;
      if (handle == siw_connection) {
        app_log("bridge: SiW917 disconnected" APP_LOG_NL);
        siw_connection = 0xFF;
        notify_enabled = false;
      }
      return false;
    }

    case sl_bt_evt_gatt_server_characteristic_status_id: {
      // Fires when central enables/disables notifications on our char
      sl_bt_evt_gatt_server_characteristic_status_t *st =
        &evt->data.evt_gatt_server_characteristic_status;

      if (st->characteristic == gattdb_bridge_sensor_data
          && st->status_flags == sl_bt_gatt_server_client_config) {
        notify_enabled = (st->client_config_flags == sl_bt_gatt_notification);
        if (notify_enabled) {
          // First subscriber wins — claim this connection as SiW917
          siw_connection = st->connection;
          app_log("bridge: SiW917 subscribed (handle=%u)" APP_LOG_NL,
                  siw_connection);
        } else {
          app_log("bridge: notifications disabled" APP_LOG_NL);
        }
      }
      return true;
    }

    case sl_bt_evt_gatt_server_attribute_value_id: {
          sl_bt_evt_gatt_server_attribute_value_t *av =
            &evt->data.evt_gatt_server_attribute_value;

          app_log("bridge: write attr=%u len=%u expected_cmd=%u" APP_LOG_NL,
                    av->attribute, av->value.len, gattdb_bridge_command);

          if (av->attribute != gattdb_bridge_command) return false;
          if (av->value.len < 2) {
            app_log("bridge: cmd too short" APP_LOG_NL);
            return true;
          }

          uint8_t target = av->value.data[0];
          uint8_t opcode = av->value.data[1];
          const uint8_t *p = &av->value.data[2];
          uint16_t plen = av->value.len - 2;

          switch (opcode) {
            case 0x10:  // SET_LED (now via BLINK_LED vendor opcode)
              if (target & 0x01) {
                // local: toggle LED directly
                  sl_simple_led_toggle(sl_led_led0.context);
              }
              if (target & 0x02) {
                vendor_send_blink_led(0x0002);   // Node 1's unicast
              }
              break;

            case 0x20:  // SET_PERIOD
              if (plen < 4) return true;
              uint32_t period_ms = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
              if (target & 0x01) bridge_update_period(period_ms);
              if (target & 0x02) vendor_send_set_period(0x0002, period_ms);
              break;

            default:
              app_log("bridge: unknown opcode 0x%02x" APP_LOG_NL, opcode);
              break;
          }
          return true;
        }

    default:
      return false;
  }
}

void bridge_init(void)
{
  sl_status_t sc = app_timer_start(&bridge_timer,
                                   1000,
                                   bridge_timer_cb,
                                   NULL,
                                   true);
  if (sc != SL_STATUS_OK) {
    app_log("bridge: timer start failed 0x%lx" APP_LOG_NL, sc);
  } else {
    app_log("bridge: ready, notifying every 1000 ms when subscribed" APP_LOG_NL);
  }
}

void bridge_update_period(uint32_t period_ms)
{
  app_timer_stop(&bridge_timer);
  if (period_ms > 0) {
    sl_status_t sc = app_timer_start(&bridge_timer,
                                     period_ms,
                                     bridge_timer_cb,
                                     NULL,
                                     true);
    if (sc == SL_STATUS_OK) {
      app_log("bridge: updated sensor notify period to %lu ms" APP_LOG_NL, period_ms);
    }
  }
}

void bridge_publish_sample(uint8_t source,
                           int32_t t_mC, int32_t rh_mPct, int32_t lux_x100)
{

  app_log("BPS STRONG: src=0x%02x conn=%u notify=%d" APP_LOG_NL,
          source, siw_connection, notify_enabled);

  if (siw_connection == 0xFF || !notify_enabled) return;

  uint8_t payload[13];
  payload[0] = source;
  memcpy(&payload[1], &t_mC,     4);
  memcpy(&payload[5], &rh_mPct,  4);
  memcpy(&payload[9], &lux_x100, 4);

  sl_status_t sc = sl_bt_gatt_server_send_notification(
      siw_connection, gattdb_bridge_sensor_data,
      sizeof(payload), payload);
  if (sc != SL_STATUS_OK) {
    app_log("bridge: notify failed 0x%lx" APP_LOG_NL, sc);
  }
}

// in bridge.c
static uint8_t bridge_adv_handle = 0xff;

void bridge_start_advertising(void)
{
  sl_status_t sc;

  if (bridge_adv_handle == 0xff) {
    sc = sl_bt_advertiser_create_set(&bridge_adv_handle);
    if (sc != SL_STATUS_OK) {
      app_log("bridge: adv create failed 0x%lx" APP_LOG_NL, sc);
      return;
    }
  }

  // Set advertising interval (100ms-200ms is standard)
  sc = sl_bt_advertiser_set_timing(bridge_adv_handle, 160, 320, 0, 0);
  if (sc != SL_STATUS_OK) {
    app_log("bridge: adv timing failed 0x%lx" APP_LOG_NL, sc);
  }

  // Generate advertising data from GATT DB (will include the device name + your service UUID)
  sc = sl_bt_legacy_advertiser_generate_data(bridge_adv_handle,
                                             sl_bt_advertiser_general_discoverable);
  if (sc != SL_STATUS_OK) {
    app_log("bridge: adv data gen failed 0x%lx" APP_LOG_NL, sc);
  }

  sc = sl_bt_legacy_advertiser_start(bridge_adv_handle,
                                     sl_bt_legacy_advertiser_connectable);
  if (sc != SL_STATUS_OK) {
    app_log("bridge: adv start failed 0x%lx" APP_LOG_NL, sc);
  } else {
    app_log("bridge: advertising bridge service" APP_LOG_NL);
  }
}
