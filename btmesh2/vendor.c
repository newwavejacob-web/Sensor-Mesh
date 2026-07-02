// vendor.c — unified vendor model for both nodes
//
// Opcodes:
//   COUNTER_STATUS (1)  publisher -> subscribers, 4 bytes uint32 (heartbeat)
//   SET_PERIOD     (2)  controller -> publisher,  4 bytes uint32 ms
//   BLINK_LED      (3)  controller -> target,     0 bytes (toggle)
//   SENSOR_DATA    (4)  publisher -> subscribers, 12 bytes T/RH/lux mC/mPct/x100
//
// Node 1 (sensor server side) calls vendor_start_publishing() in app.c to
// kick off the periodic SENSOR_DATA + COUNTER_STATUS publish loop.
// Node 2 (bridge side) does NOT call that; it only uses send helpers driven
// by bridge.c command writes, and forwards received SENSOR_DATA to the bridge.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "sl_status.h"
#include "sl_btmesh_api.h"
#include "sl_bt_api.h"
#include "app_log.h"
#include "app_timer.h"
#include "sl_simple_led.h"
#include "sl_simple_led_instances.h"

// ---- IDs ----
#define VENDOR_COMPANY_ID  0x02FF
#define VENDOR_MODEL_ID    0x1234
#define VENDOR_ELEM_INDEX  0

// ---- Opcodes ----
#define OPCODE_COUNTER_STATUS  1
#define OPCODE_SET_PERIOD      2
#define OPCODE_BLINK_LED       3
#define OPCODE_SENSOR_DATA     4

static const uint8_t vendor_opcodes[] = {
  OPCODE_COUNTER_STATUS,
  OPCODE_SET_PERIOD,
  OPCODE_BLINK_LED,
  OPCODE_SENSOR_DATA,
};

// ---- State ----
static uint32_t    g_counter   = 0;
static uint32_t    g_period_ms = 5000;
static app_timer_t publish_timer;
//static bool        g_led_state = false;

// ---- Externs ----
//   sensors_get_latest is provided by sensors.c on both nodes.
//   bridge_publish_sample is weak — Node 1 doesn't link bridge.c, gets the stub.
//   Node 2's bridge.c provides the real implementation.
extern bool sensors_get_latest(int32_t *t_mC, int32_t *rh_mPct, int32_t *lux_x100);

__attribute__((weak)) void bridge_publish_sample(uint8_t source,
                                                 int32_t t_mC,
                                                 int32_t rh_mPct,
                                                 int32_t lux_x100)
{
  (void)source; (void)t_mC; (void)rh_mPct; (void)lux_x100;
  // weak stub — Node 1 has no bridge, so this is the default no-op.
}

/*============================================================
 * Publish path — Node 1 calls these via the timer
 *============================================================*/

static void publish_counter(void)
{
  uint8_t payload[4] = {
    (uint8_t)(g_counter      ),
    (uint8_t)(g_counter >>  8),
    (uint8_t)(g_counter >> 16),
    (uint8_t)(g_counter >> 24),
  };

  sl_status_t sc = sl_btmesh_vendor_model_set_publication(
      VENDOR_ELEM_INDEX, VENDOR_COMPANY_ID, VENDOR_MODEL_ID,
      OPCODE_COUNTER_STATUS, 1, sizeof(payload), payload);
  if (sc != SL_STATUS_OK) {
    app_log("vendor: counter set_pub failed 0x%lx" APP_LOG_NL, sc);
    return;
  }
  sc = sl_btmesh_vendor_model_publish(VENDOR_ELEM_INDEX,
                                      VENDOR_COMPANY_ID,
                                      VENDOR_MODEL_ID);
  if (sc == SL_STATUS_OK) {
    app_log("vendor: published counter=%lu" APP_LOG_NL, g_counter);
    g_counter++;
  }
}

static void publish_sensor_data(void)
{
  int32_t t = 0, rh = 0, lux = 0;
  if (!sensors_get_latest(&t, &rh, &lux)) {
    return;  // sensors not ready yet
  }

  uint8_t payload[12];
  memcpy(&payload[0], &t,   4);
  memcpy(&payload[4], &rh,  4);
  memcpy(&payload[8], &lux, 4);

  sl_status_t sc = sl_btmesh_vendor_model_set_publication(
      VENDOR_ELEM_INDEX, VENDOR_COMPANY_ID, VENDOR_MODEL_ID,
      OPCODE_SENSOR_DATA, 1, sizeof(payload), payload);
  if (sc != SL_STATUS_OK) {
    app_log("vendor: sensor set_pub failed 0x%lx" APP_LOG_NL, sc);
    return;
  }
  sc = sl_btmesh_vendor_model_publish(VENDOR_ELEM_INDEX,
                                      VENDOR_COMPANY_ID,
                                      VENDOR_MODEL_ID);
  if (sc == SL_STATUS_OK) {
    app_log("vendor: published sensor t=%ld rh=%ld lux=%ld" APP_LOG_NL,
            (long)t, (long)rh, (long)lux);
  }
}

static void publish_timer_cb(app_timer_t *handle, void *data)
{
  (void)handle; (void)data;
  publish_counter();
  publish_sensor_data();
}

static void restart_publish_timer(void)
{
  app_timer_stop(&publish_timer);
  if (g_period_ms == 0) return;
  sl_status_t sc = app_timer_start(&publish_timer, g_period_ms,
                                   publish_timer_cb, NULL, true);
  if (sc != SL_STATUS_OK) {
    app_log("vendor: timer start failed 0x%lx" APP_LOG_NL, sc);
  } else {
    app_log("vendor: publishing every %lu ms" APP_LOG_NL, g_period_ms);
  }
}

// Node 1 calls this from app.c after node_initialized + appkey bound
void vendor_start_publishing(void)
{
  restart_publish_timer();
}

/*============================================================
 * Send helpers — Node 2 calls these from bridge.c
 *============================================================*/

sl_status_t vendor_send_set_period(uint16_t dst_addr, uint32_t period_ms)
{
  uint8_t payload[4] = {
    (uint8_t)(period_ms      ),
    (uint8_t)(period_ms >>  8),
    (uint8_t)(period_ms >> 16),
    (uint8_t)(period_ms >> 24),
  };
  sl_status_t sc = sl_btmesh_vendor_model_send(
      dst_addr, -1, 0, VENDOR_ELEM_INDEX,
      VENDOR_COMPANY_ID, VENDOR_MODEL_ID,
      0, OPCODE_SET_PERIOD, 1,
      sizeof(payload), payload);
  app_log("vendor: SET_PERIOD %lu -> 0x%04x sc=0x%lx" APP_LOG_NL,
          period_ms, dst_addr, sc);
  return sc;
}

sl_status_t vendor_send_blink_led(uint16_t dst_addr)
{
  sl_status_t sc = sl_btmesh_vendor_model_send(
      dst_addr, -1, 0, VENDOR_ELEM_INDEX,
      VENDOR_COMPANY_ID, VENDOR_MODEL_ID,
      0, OPCODE_BLINK_LED, 1,
      0, NULL);
  app_log("vendor: BLINK_LED -> 0x%04x sc=0x%lx" APP_LOG_NL, dst_addr, sc);
  return sc;
}

/*============================================================
 * Receive handler — fires on both nodes
 *============================================================*/

static void handle_vendor_receive(sl_btmesh_evt_vendor_model_receive_t *evt)
{
  switch (evt->opcode) {

    case OPCODE_SET_PERIOD: {
      if (evt->payload.len < 4) {
        app_log("vendor: SET_PERIOD too short (%u)" APP_LOG_NL, evt->payload.len);
        return;
      }
      uint32_t new_period =  (uint32_t)evt->payload.data[0]
                          | ((uint32_t)evt->payload.data[1] <<  8)
                          | ((uint32_t)evt->payload.data[2] << 16)
                          | ((uint32_t)evt->payload.data[3] << 24);
      if (new_period < 100)       new_period = 100;
      if (new_period > 3600000UL) new_period = 3600000UL;

      app_log("vendor: SET_PERIOD %lu -> %lu ms" APP_LOG_NL,
              g_period_ms, new_period);
      g_period_ms = new_period;
      restart_publish_timer();
      break;
    }

    case OPCODE_BLINK_LED: {

          sl_simple_led_toggle(sl_led_led0.context);
            app_log("BLINK toggled" APP_LOG_NL);
            break;
    }

    case OPCODE_COUNTER_STATUS: {
      if (evt->payload.len < 4) return;
      uint32_t c =  (uint32_t)evt->payload.data[0]
                 | ((uint32_t)evt->payload.data[1] <<  8)
                 | ((uint32_t)evt->payload.data[2] << 16)
                 | ((uint32_t)evt->payload.data[3] << 24);
      app_log("vendor: rx counter=%lu from 0x%04x" APP_LOG_NL,
              c, evt->source_address);
      break;
    }

    case OPCODE_SENSOR_DATA: {
      if (evt->payload.len < 12) {
        app_log("vendor: SENSOR_DATA too short (%u)" APP_LOG_NL, evt->payload.len);
        return;
      }
      int32_t t, rh, lux;
      memcpy(&t,   &evt->payload.data[0], 4);
      memcpy(&rh,  &evt->payload.data[4], 4);
      memcpy(&lux, &evt->payload.data[8], 4);
      app_log("vendor: rx sensor t=%ld rh=%ld lux=%ld from 0x%04x" APP_LOG_NL,
              (long)t, (long)rh, (long)lux, evt->source_address);
      bridge_publish_sample(0x02, t, rh, lux);  // weak stub on Node 1
      break;
    }

    default:
      app_log("vendor: unknown opcode 0x%02x" APP_LOG_NL, evt->opcode);
      break;
  }
}

/*============================================================
 * Init + dispatcher
 *============================================================*/

sl_status_t vendor_model_init(void)
{
  app_log("vendor: init" APP_LOG_NL);
  sl_status_t sc = sl_btmesh_vendor_model_init(VENDOR_ELEM_INDEX,
                                               VENDOR_COMPANY_ID,
                                               VENDOR_MODEL_ID,
                                               1,                       // publish=1 on both, doesn't hurt Node 2
                                               sizeof(vendor_opcodes),
                                               vendor_opcodes);
  if (sc != SL_STATUS_OK) {
    app_log("vendor: model init failed 0x%lx" APP_LOG_NL, sc);
    return sc;
  }
  app_log("vendor: registered cid=0x%04x mid=0x%04x" APP_LOG_NL,
          VENDOR_COMPANY_ID, VENDOR_MODEL_ID);
  return SL_STATUS_OK;
}

bool vendor_model_on_event(sl_btmesh_msg_t *evt)
{
  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_btmesh_evt_vendor_model_receive_id:
      handle_vendor_receive(&evt->data.evt_vendor_model_receive);
      return true;
    case sl_btmesh_evt_vendor_model_send_complete_id:
      return true;
    default:
      return false;
  }
}
