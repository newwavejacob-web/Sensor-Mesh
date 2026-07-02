// sensors.c — minimal SHT40 + VEML7700 reader, prints via app_log
#include <stdint.h>
#include <stdbool.h>
#include "sl_i2cspm.h"
#include "sl_i2cspm_instances.h"
#include "app_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// The Thermometer FreeRTOS example configures an instance called "sensor".
// If yours is named differently, grep autogen/sl_i2cspm_instances.h
#define I2C_BUS  sl_i2cspm_qwiic

// ---- SHT40 ----
#define SHT40_ADDR              0x44
#define SHT40_CMD_MEASURE_HIGH  0xFD

// ---- VEML7700 ----
#define VEML7700_ADDR        0x10
#define VEML7700_REG_CONF    0x00
#define VEML7700_REG_ALS     0x04

static SemaphoreHandle_t cache_mtx = NULL;
static int32_t cache_temp_mC  = 0;
static int32_t cache_rh_mPct  = 0;
static int32_t cache_lux_x100 = 0;
static bool    cache_valid    = false;

bool sensors_get_latest(int32_t *t_mC, int32_t *rh_mPct, int32_t *lux_x100)
{
  if (cache_mtx == NULL) return false;
  if (xSemaphoreTake(cache_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return false;

  bool ok = cache_valid;
  if (ok) {
    if (t_mC)     *t_mC     = cache_temp_mC;
    if (rh_mPct)  *rh_mPct  = cache_rh_mPct;
    if (lux_x100) *lux_x100 = cache_lux_x100;
  }
  xSemaphoreGive(cache_mtx);
  return ok;
}


// emlib I2C takes the 8-bit (shifted) form of the address.
static inline uint16_t a8(uint8_t a7) { return (uint16_t)a7 << 1; }

static I2C_TransferReturn_TypeDef i2c_write(uint8_t addr7,
                                            const uint8_t *tx, uint16_t n)
{
  I2C_TransferSeq_TypeDef seq = {0};
  seq.addr        = a8(addr7);
  seq.flags       = I2C_FLAG_WRITE;
  seq.buf[0].data = (uint8_t *)tx;
  seq.buf[0].len  = n;
  return I2CSPM_Transfer(I2C_BUS, &seq);
}

static I2C_TransferReturn_TypeDef i2c_read(uint8_t addr7,
                                           uint8_t *rx, uint16_t n)
{
  I2C_TransferSeq_TypeDef seq = {0};
  seq.addr        = a8(addr7);
  seq.flags       = I2C_FLAG_READ;
  seq.buf[0].data = rx;
  seq.buf[0].len  = n;
  return I2CSPM_Transfer(I2C_BUS, &seq);
}

static I2C_TransferReturn_TypeDef i2c_write_read(uint8_t addr7,
                                                 const uint8_t *tx, uint16_t tn,
                                                 uint8_t *rx, uint16_t rn)
{
  I2C_TransferSeq_TypeDef seq = {0};
  seq.addr        = a8(addr7);
  seq.flags       = I2C_FLAG_WRITE_READ;
  seq.buf[0].data = (uint8_t *)tx;
  seq.buf[0].len  = tn;
  seq.buf[1].data = rx;
  seq.buf[1].len  = rn;
  return I2CSPM_Transfer(I2C_BUS, &seq);
}

// ---- SHT40 ----
// Outputs in milli-units to avoid pulling in soft-float.
static bool sht40_read(int32_t *temp_mC, int32_t *rh_mPct)
{
  uint8_t cmd = SHT40_CMD_MEASURE_HIGH;
  if (i2c_write(SHT40_ADDR, &cmd, 1) != i2cTransferDone) return false;

  vTaskDelay(pdMS_TO_TICKS(10));   // high-precision = max 8.3ms

  uint8_t rx[6];
  if (i2c_read(SHT40_ADDR, rx, 6) != i2cTransferDone)   return false;

  // Skipping CRC for now — add later if you see corruption.
  uint16_t t_ticks  = ((uint16_t)rx[0] << 8) | rx[1];
  uint16_t rh_ticks = ((uint16_t)rx[3] << 8) | rx[4];

  // t_mC  = -45000 + 175000 * t_ticks  / 65535
  // rh_mP = -6000  + 125000 * rh_ticks / 65535, then clamp 0..100000
  *temp_mC = -45000 + (int32_t)((175000LL * t_ticks) / 65535);
  int32_t rh = -6000 + (int32_t)((125000LL * rh_ticks) / 65535);
  if (rh < 0)       rh = 0;
  if (rh > 100000)  rh = 100000;
  *rh_mPct = rh;
  return true;
}

// ---- VEML7700 ----
// Default cfg: gain x1, IT=100ms. Resolution = 0.0576 lux/count, ~7700 lux max.
// If you'll see direct sunlight, change cfg LSB to set gain=1/4 (bits 12:11 = 11).
static bool veml7700_init(void)
{
  // VEML7700 wants register pointer + 16-bit value LSB-first.
  uint8_t tx[3] = { VEML7700_REG_CONF, 0x00, 0x00 };
  if (i2c_write(VEML7700_ADDR, tx, 3) != i2cTransferDone) return false;
  vTaskDelay(pdMS_TO_TICKS(120));  // settle one integration period
  return true;
}

// Returns lux * 100 (centilux) for two-decimal display without floats.
static bool veml7700_read(int32_t *lux_x100)
{
  uint8_t reg = VEML7700_REG_ALS;
  uint8_t rx[2];
  if (i2c_write_read(VEML7700_ADDR, &reg, 1, rx, 2) != i2cTransferDone)
    return false;

  uint16_t raw = ((uint16_t)rx[1] << 8) | rx[0];  // LSB first
  // 0.0576 lux/count -> *100 -> 5.76 -> integer math via *576/100
  *lux_x100 = ((int32_t)raw * 576) / 100;
  return true;
}

// ---- Task ----
#define SENSOR_TASK_STACK 512
#define SENSOR_TASK_PRIO  20

static void sensor_task(void *arg)
{
  (void)arg;
  app_log("sensors task starting" APP_LOG_NL);

  if (!veml7700_init()) app_log("VEML7700 init FAILED" APP_LOG_NL);
  else                  app_log("VEML7700 init OK"     APP_LOG_NL);

  for (;;) {
      static uint32_t loop_count = 0;
      if (++loop_count % 5 == 0) {
        app_log("sensors: loop alive %lu" APP_LOG_NL, loop_count);
      }
    int32_t t_mC = 0, rh_mP = 0, lux_x100 = 0;
    bool th_ok  = sht40_read(&t_mC, &rh_mP);
    bool lux_ok = veml7700_read(&lux_x100);

    if (th_ok && lux_ok) {
        if (xSemaphoreTake(cache_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
           cache_temp_mC  = t_mC;
           cache_rh_mPct  = rh_mP;
           cache_lux_x100 = lux_x100;
           cache_valid    = true;
           xSemaphoreGive(cache_mtx);
         }

      app_log("T=%ld.%03ld C  RH=%ld.%03ld %%  Lux=%ld.%02ld" APP_LOG_NL,
              (long)(t_mC / 1000),  (long)((t_mC < 0 ? -t_mC : t_mC) % 1000),
              (long)(rh_mP / 1000), (long)(rh_mP % 1000),
              (long)(lux_x100 / 100), (long)(lux_x100 % 100));
    } else {
      app_log("read fail: th=%d lux=%d" APP_LOG_NL, th_ok, lux_ok);
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
  }
}

void sensors_start(void)
{
  cache_mtx = xSemaphoreCreateMutex();
  (void)xTaskCreate(sensor_task, "sensors", SENSOR_TASK_STACK,
                    NULL, SENSOR_TASK_PRIO, NULL);
}
