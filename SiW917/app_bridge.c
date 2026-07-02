// app_bridge.c — placeholder for future BLE central work
#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef struct {
  uint8_t  node_id;
  int16_t  temp_c_x100;
  uint16_t humidity_x100;
  uint16_t lux;
} __attribute__((packed)) sensor_packet_t;

__attribute__((weak)) void ble_bridge_on_data(sensor_packet_t *pkt)
{
  (void)pkt;  // suppress unused-parameter warning
}

void ble_bridge_init(void)
{
  printf("[BLE] init (stub)\n");
  // TODO: real BLE central scan/connect/discover/subscribe
}

int ble_bridge_is_connected(void)
{
  return 0;
}
