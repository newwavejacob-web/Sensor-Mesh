// sensor_server_overrides.c
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "sl_status.h"

extern bool sensors_get_latest(int32_t *t_mC, int32_t *rh_mPct, int32_t *lux_x100);

// Temperature + humidity (one call, returns both)
sl_status_t sl_sensor_rht_get(uint32_t *rh, int32_t *temp)
{
  int32_t t_mC, rh_mP;
  if (!sensors_get_latest(&t_mC, &rh_mP, NULL)) {
    return SL_STATUS_NOT_INITIALIZED;
  }
  if (temp) *temp = t_mC;
  if (rh)   *rh   = rh_mP;
  return SL_STATUS_OK;
}

// Illuminance — check the exact signature in sl_sensor_light.h
// Common signatures:
//   sl_status_t sl_sensor_light_get(uint32_t *lux);          (lux)
//   sl_status_t sl_sensor_light_get(float *lux);             (some SDKs)
sl_status_t sl_sensor_lux_get(float *lux)
{
  int32_t lux_x100;
  if (!sensors_get_latest(NULL, NULL, &lux_x100)) {
    return SL_STATUS_NOT_INITIALIZED;
  }
  // SDK expects integer lux, you cache lux*100 — divide
  if (lux) *lux = (float)(lux_x100 * 1);
  return SL_STATUS_OK;
}

// Stub the SDK's I2C-walking init so it doesn't fight our task for the bus
sl_status_t sl_sensor_rht_init(void)    { return SL_STATUS_OK; }
void        sl_sensor_rht_deinit(void)  { }
sl_status_t sl_sensor_lux_init(void)  { return SL_STATUS_OK; }
void        sl_sensor_lux_deinit(void){ }
