// sensor_server_overrides.c — redirect SDK's RHT read to our SHT40 cache
#include <stddef.h>     // NULL  (this fixes your earlier compile error)
#include <stdbool.h>
#include <stdint.h>
#include "sl_status.h"

extern bool sensors_get_latest(int32_t *t_mC, int32_t *rh_mPct, int32_t *lux_x100);

// The SDK's get_temperature() does this conversion right after our return:
//   temp_data = (temp_data * 2 + 499) / 1000;
// That's milli-Celsius to signed half-degrees, rounded.
// So we just return milli-Celsius — exactly what our sensor_task already caches.

sl_status_t sl_sensor_rht_get(uint32_t *rh, int32_t *temp)
{
  int32_t t_mC, rh_mP;
  if (!sensors_get_latest(&t_mC, &rh_mP, NULL)) {
    return SL_STATUS_NOT_INITIALIZED;
  }
  if (temp) *temp = t_mC;
  if (rh)   *rh   = (uint32_t)rh_mP;
  return SL_STATUS_OK;
}

// The SDK's component init walks I2C to find a sensor. We don't want it
// fighting our task for the bus, so stub these out.
sl_status_t sl_sensor_rht_init(void)   { return SL_STATUS_OK; }
void        sl_sensor_rht_deinit(void) { }
