#ifndef BRIDGE_MQTT_H
#define BRIDGE_MQTT_H

#include <stdint.h>

#define NUM_NODES 2

typedef struct {
    char     id[16];
    float    temp_c;
    float    humidity;
    float    lux;
    uint32_t seq;

    /* BLE state - populated by ble_subtasks.c after discovery */
    volatile uint8_t  ble_ready;          /* 1 once cmd write handle is known */
    uint8_t           ble_dev_addr[6];    /* raw 6-byte BD addr (not ASCII) */
    uint16_t          ble_cmd_handle;     /* GATT handle of command char */
} node_t;

extern node_t g_nodes[NUM_NODES];

/* Implemented in bridge_telemetry.c, callable from anywhere */
void set_ble_blink(node_t *n);
void set_ble_arm(node_t *n, uint8_t on);
void set_ble_interval(node_t *n, uint32_t ms);
sl_status_t bridge_telemetry_start(void);

#endif
