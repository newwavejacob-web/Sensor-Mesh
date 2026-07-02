// bridge_telemetry.c — MQTT + Firebase publish path for siw_dual
//
// Reads from g_nodes[] (updated by BLE notification callback in ble_subtasks.c).
// Publishes to MQTT broker + Firebase Realtime DB every TELEMETRY_INTERVAL_MS.

#include "sl_net.h"
#include "sl_utility.h"
#include "cmsis_os2.h"
#include "sl_constants.h"
#include "sl_mqtt_client.h"
#include "sl_wifi.h"
#include "sl_net_dns.h"
#include "sl_wifi_callback_framework.h"
#include "sl_net_wifi_types.h"
#include "socket.h"
#include "netinet_in.h"
#include "cacert.pem.h"
#include "firebase.pem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "bridge_mqtt.h"

/* ------ MQTT broker config ------ */
#define MQTT_BROKER_IP        "34.243.217.54"
#define MQTT_BROKER_PORT      8883
#define CLIENT_PORT           1
#define CLIENT_ID             "siw917-bridge-jr1"
#define ENCRYPT_CONNECTION    1
#define CERTIFICATE_INDEX     1
#define FIREBASE_CERT_INDEX   2
#define KEEP_ALIVE_INTERVAL   60
#define MQTT_CONNECT_TIMEOUT  5000
#define MQTT_KEEPALIVE_RETRIES 3
#define IS_CLEAN_SESSION      1
// ---- Wire protocol with MG24 bridge ----
#define TARGET_LOCAL   0x01   // Node 2 (the bridge MG24 itself)
#define TARGET_REMOTE  0x02   // Node 1 (reachable via mesh)
#define TARGET_BOTH    0x03

#define OPCODE_SET_LED     0x10
#define OPCODE_SET_PERIOD  0x20

#define TELEMETRY_INTERVAL_MS 5000

extern int32_t rsi_ble_set_att_cmd(uint8_t *dev_addr, uint16_t handle,
                                   uint16_t data_len, uint8_t *data);

/* ------ Public node table (defined here, accessed elsewhere via extern) ------ */
node_t g_nodes[NUM_NODES] = {
  { .id = "node-0" },
  { .id = "node-1" },
};

/* ------ Internal state ------ */
static sl_mqtt_client_t client = { 0 };
static volatile uint8_t g_mqtt_connected = 0;
static char topic_buf[128];
static char payload_buf[256];

static sl_mqtt_client_configuration_t mqtt_client_configuration = {
  .is_clean_session = IS_CLEAN_SESSION,
  .client_id        = (uint8_t *)CLIENT_ID,
  .client_id_length = sizeof(CLIENT_ID) - 1,
  .tls_flags        = SL_MQTT_TLS_ENABLE | SL_MQTT_TLS_TLSV_1_2 | SL_MQTT_TLS_CERT_INDEX_1,
  .client_port      = CLIENT_PORT,
};

static sl_mqtt_broker_t mqtt_broker_configuration = {
  .port                    = MQTT_BROKER_PORT,
  .is_connection_encrypted = ENCRYPT_CONNECTION,
  .connect_timeout         = MQTT_CONNECT_TIMEOUT,
  .keep_alive_interval     = KEEP_ALIVE_INTERVAL,
  .keep_alive_retries      = MQTT_KEEPALIVE_RETRIES,
};

static sl_mqtt_client_last_will_message_t last_will_message = {
  .is_retained         = 0,
  .will_qos_level      = 0,
  .will_topic          = (uint8_t *)"ucf/security/last-will",
  .will_topic_length   = sizeof("ucf/security/last-will") - 1,
  .will_message        = (uint8_t *)"siw917 disconnected",
  .will_message_length = sizeof("siw917 disconnected") - 1,
};

/* ------ MQTT callback ------ */
static void mqtt_client_event_handler(void *c, sl_mqtt_client_event_t event, void *event_data, void *context)
{
  (void)c; (void)event_data;
  switch (event) {
    case SL_MQTT_CLIENT_CONNECTED_EVENT:
      printf("[mqtt] connected\r\n");
      g_mqtt_connected = 1;
      break;
    case SL_MQTT_CLIENT_MESSAGE_PUBLISHED_EVENT: {
      sl_mqtt_client_message_t *m = (sl_mqtt_client_message_t *)context;
      printf("[mqtt] published on: %.*s\r\n", (int)m->topic_length, (char *)m->topic);
      break;
    }
    case SL_MQTT_CLIENT_DISCONNECTED_EVENT:
      printf("[mqtt] disconnected\r\n");
      g_mqtt_connected = 0;
      break;
    case SL_MQTT_CLIENT_ERROR_EVENT:
      printf("[mqtt] error event\r\n");
      break;
    default:
      break;
  }
}

/* ------ Publish to MQTT ------ */
static void publish_node_telemetry(node_t *n)
{
  snprintf(topic_buf, sizeof(topic_buf),
           "ucf/security/nodes/%s/telemetry", n->id);
  snprintf(payload_buf, sizeof(payload_buf),
           "{\"node\":\"%s\",\"seq\":%lu,\"temp_c\":%.2f,"
           "\"humidity\":%.2f,\"lux\":%.1f}",
           n->id, (unsigned long)n->seq,
           n->temp_c, n->humidity, n->lux);

  sl_mqtt_client_message_t msg = {
    .qos_level            = 0,
    .is_retained          = 0,
    .is_duplicate_message = 0,
    .topic                = (uint8_t *)topic_buf,
    .topic_length         = (uint16_t)strlen(topic_buf),
    .content              = (uint8_t *)payload_buf,
    .content_length       = (uint32_t)strlen(payload_buf),
  };
  sl_status_t s = sl_mqtt_client_publish(&client, &msg, 0, NULL);
  if (s != SL_STATUS_IN_PROGRESS) {
    printf("[mqtt] publish failed: 0x%lx\r\n", s);
  }
}

/* ------ Publish to Firebase Realtime DB ------ */
static void firebase_put(node_t *n)
{
  sl_status_t status;
  int sock;
  char request[512];
  char response[512];
  char body[128];

  snprintf(body, sizeof(body),
    "{\"temp\":%.2f,\"humidity\":%.2f,\"lux\":%.1f}",
    n->temp_c, n->humidity, n->lux);

  snprintf(request, sizeof(request),
    "PUT /nodes/%s.json HTTP/1.1\r\n"
    "Host: ucf-jr-default-rtdb.firebaseio.com\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n"
    "\r\n"
    "%s",
    n->id, (int)strlen(body), body);

  sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) { printf("[firebase] Socket failed\n"); return; }

  uint8_t cert_index = FIREBASE_CERT_INDEX;
  setsockopt(sock, SOL_SOCKET, SL_SO_CERT_INDEX, &cert_index, sizeof(cert_index));
  setsockopt(sock, SOL_TCP, TCP_ULP, "tls", sizeof("tls"));

  sl_ip_address_t ip;
  status = sl_net_dns_resolve_hostname(
    "ucf-jr-default-rtdb.firebaseio.com", 10000, SL_NET_DNS_TYPE_IPV4, &ip);
  if (status != SL_STATUS_OK) { printf("[firebase] DNS failed\n"); close(sock); return; }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(443);
  addr.sin_addr.s_addr = ip.ip.v4.value;

  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    printf("[firebase] Connect failed\n");
    close(sock);
    return;
  }

  send(sock, request, strlen(request), 0);

  int len = recv(sock, response, sizeof(response) - 1, 0);
  if (len > 0) {
    response[len] = '\0';
    // Print just the status line
    char *eol = strchr(response, '\r');
    if (eol) *eol = '\0';
    printf("[firebase %s] %s\n", n->id, response);
  }
  close(sock);
}

static void firebase_get_command(const char *node_id, char *out_cmd, int max_len)
{
  int sock;
  char request[256];
  char response[512];

  snprintf(request, sizeof(request),
    "GET /commands/%s.json HTTP/1.1\r\n"
    "Host: ucf-jr-default-rtdb.firebaseio.com\r\n"
    "\r\n",
    node_id);

  sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) return;

  uint8_t cert_index = FIREBASE_CERT_INDEX;
  setsockopt(sock, SOL_SOCKET, SL_SO_CERT_INDEX, &cert_index, sizeof(cert_index));
  setsockopt(sock, SOL_TCP, TCP_ULP, "tls", sizeof("tls"));

  sl_ip_address_t ip;
  if (sl_net_dns_resolve_hostname(
        "ucf-jr-default-rtdb.firebaseio.com",
        10000,
        SL_NET_DNS_TYPE_IPV4,
        &ip) != SL_STATUS_OK) {
    close(sock);
    return;
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(443);
  addr.sin_addr.s_addr = ip.ip.v4.value;

  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(sock);
    return;
  }

  send(sock, request, strlen(request), 0);

  int len = recv(sock, response, sizeof(response) - 1, 0);
  if (len > 0) {
    response[len] = '\0';

    // crude JSON parse (good enough for now)
    char *body = strstr(response, "\r\n\r\n");
    if (body) {
      body += 4;
      strncpy(out_cmd, body, max_len);
    }
  }

  close(sock);
}

static void firebase_clear_command(const char *node_id)
{
    int sock;
    char request[256], response[256];
    const char *body = "null";

    snprintf(request, sizeof(request),
        "PUT /commands/%s.json HTTP/1.1\r\n"
        "Host: ucf-jr-default-rtdb.firebaseio.com\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n\r\n%s",
        node_id, (int)strlen(body), body);

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return;

    uint8_t cert_index = FIREBASE_CERT_INDEX;
    setsockopt(sock, SOL_SOCKET, SL_SO_CERT_INDEX, &cert_index, sizeof(cert_index));
    setsockopt(sock, SOL_TCP, TCP_ULP, "tls", sizeof("tls"));

    sl_ip_address_t ip;
    if (sl_net_dns_resolve_hostname("ucf-jr-default-rtdb.firebaseio.com",
                                    10000, SL_NET_DNS_TYPE_IPV4, &ip) != SL_STATUS_OK) {
        close(sock); return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(443);
    addr.sin_addr.s_addr = ip.ip.v4.value;
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return; }

    send(sock, request, strlen(request), 0);
    recv(sock, response, sizeof(response) - 1, 0);  /* drain, ignore */
    close(sock);
}

void handle_command(node_t *n, char *cmd)
{
    if (!cmd || cmd[0] == '\0' || strstr(cmd, "null")) return;

    bool acted = false;

    if (strstr(cmd, "blink"))        { set_ble_blink(n);        acted = true; }
    else if (strstr(cmd, "arm_on"))  { set_ble_arm(n, 1);       acted = true; }
    else if (strstr(cmd, "arm_off")) { set_ble_arm(n, 0);       acted = true; }
    else if (strstr(cmd, "interval")) {
        char *colon = strchr(cmd, ':');
        if (colon) {
            int val = atoi(colon + 1);
            if (val > 0) { set_ble_interval(n, (uint32_t)val); acted = true; }
        }
    }

    if (acted) firebase_clear_command(n->id);
}
/*
static void send_cmd_to_node(node_t *n, const char *s, uint16_t len)
{
    if (!n->ble_ready || n->ble_cmd_handle == 0) {
        printf("[ble] %s not ready (cmd dropped: %.*s)\r\n", n->id, len, s);
        return;
    }
    int32_t st = rsi_ble_set_att_cmd(n->ble_dev_addr, n->ble_cmd_handle,
                                     len, (uint8_t *)s);
    if (st != 0) {
        printf("[ble] write to %s failed: 0x%lx\r\n", n->id, (unsigned long)st);
    } else {
        printf("[ble] -> %s: %.*s\r\n", n->id, len, s);
    }
}

void set_ble_blink(node_t *n)               { send_cmd_to_node(n, "blink",  5); }
void set_ble_arm  (node_t *n, uint8_t on)   { send_cmd_to_node(n, on ? "arm_on" : "arm_off", on ? 6 : 7); }
void set_ble_interval(node_t *n, uint32_t ms) {
    char buf[32];
    int  n_ = snprintf(buf, sizeof(buf), "interval:%lu", (unsigned long)ms);
    if (n_ > 0) send_cmd_to_node(n, buf, (uint16_t)n_);
}*/

static void send_cmd_bytes(node_t *n, uint8_t target, uint8_t opcode,
                           const uint8_t *payload, uint8_t plen)
{
    if (!n->ble_ready || n->ble_cmd_handle == 0) {
        printf("[ble] %s not ready (cmd dropped)\r\n", n->id);
        return;
    }
    uint8_t buf[16];
    if (plen + 2u > sizeof(buf)) return;
    buf[0] = target;
    buf[1] = opcode;
    if (plen) memcpy(&buf[2], payload, plen);

    int32_t st = rsi_ble_set_att_cmd(n->ble_dev_addr, n->ble_cmd_handle,
                                     plen + 2, buf);
    if (st != 0) {
        printf("[ble] write to %s failed: 0x%lx\r\n",
               n->id, (unsigned long)st);
    } else {
        printf("[ble] -> %s: target=0x%02x op=0x%02x len=%u\r\n",
               n->id, target, opcode, plen);
    }
}

// Map node id -> target byte. node-0 is remote (Node 1), node-1 is local (Node 2).
static uint8_t target_for_node(node_t *n)
{
    if (strcmp(n->id, "node-0") == 0) return TARGET_REMOTE;
    if (strcmp(n->id, "node-1") == 0) return TARGET_LOCAL;
    return TARGET_BOTH;
}

void set_ble_blink(node_t *n)
{
    // SET_LED with no payload doesn't exist in our protocol; pulse on then off.
    uint8_t on = 1, off = 0;
    send_cmd_bytes(n, target_for_node(n), OPCODE_SET_LED, &on,  1);
    osDelay(300);
    send_cmd_bytes(n, target_for_node(n), OPCODE_SET_LED, &off, 1);
}

void set_ble_arm(node_t *n, uint8_t on)
{
    send_cmd_bytes(n, target_for_node(n), OPCODE_SET_LED, &on, 1);
}

void set_ble_interval(node_t *n, uint32_t ms)
{
    uint8_t payload[4] = {
        (uint8_t)(ms      ),
        (uint8_t)(ms >>  8),
        (uint8_t)(ms >> 16),
        (uint8_t)(ms >> 24),
    };
    send_cmd_bytes(n, target_for_node(n), OPCODE_SET_PERIOD, payload, 4);
}

/* ------ Init MQTT + start publish loop ------ */
sl_status_t bridge_telemetry_start(void)
{
  sl_status_t status;

  /* Load broker CA cert */
  status = sl_net_set_credential(SL_NET_TLS_SERVER_CREDENTIAL_ID(CERTIFICATE_INDEX),
                                 SL_NET_SIGNING_CERTIFICATE,
                                 cacert, sizeof(cacert) - 1);
  if (status != SL_STATUS_OK) {
    printf("Loading broker CA failed: 0x%lx\r\n", status);
    return status;
  }
  printf("Load TLS CA certificate at index %d Success\r\n", CERTIFICATE_INDEX);

  /* Load Firebase CA cert */
  status = sl_net_set_credential(SL_NET_TLS_SERVER_CREDENTIAL_ID(FIREBASE_CERT_INDEX),
                                 SL_NET_SIGNING_CERTIFICATE,
                                 firebase_ca, sizeof(firebase_ca) - 1);
  if (status != SL_STATUS_OK) {
    printf("Loading Firebase CA failed: 0x%lx\r\n", status);
    return status;
  }
  printf("Load Firebase certificate at index %d Success\r\n", FIREBASE_CERT_INDEX);

  /* Init MQTT */
  status = sl_mqtt_client_init(&client, mqtt_client_event_handler);
  if (status != SL_STATUS_OK) {
    printf("MQTT init failed: 0x%lx\r\n", status);
    return status;
  }
  printf("Init MQTT client Success\r\n");

  /* Resolve broker IP
  sl_ip_address_t broker_ip = {0};
  status = sl_net_dns_resolve_hostname(MQTT_BROKER_IP, 10000, SL_NET_DNS_TYPE_IPV4, &broker_ip);
  if (status != SL_STATUS_OK) {
    uint32_t ip_num = 0;
    if (sl_inet_pton4(MQTT_BROKER_IP, MQTT_BROKER_IP + strlen(MQTT_BROKER_IP), &ip_num) == 1) {
      broker_ip.ip.v4.value = ip_num;
      status = SL_STATUS_OK;
    }
  }
  if (status != SL_STATUS_OK) {
    printf("MQTT broker IP resolve failed\r\n");
    return status;
  }*/
  sl_ip_address_t broker_ip = {0};
  {
    unsigned int a, b, c, d;
    if (sscanf(MQTT_BROKER_IP, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
      printf("MQTT broker IP parse failed\r\n");
      return SL_STATUS_FAIL;
    }
    /* Network byte order: a.b.c.d → bytes [a, b, c, d] in v4.value (little-endian uint32) */
    broker_ip.ip.v4.value =
        ((uint32_t)a)        |
        ((uint32_t)b << 8)   |
        ((uint32_t)c << 16)  |
        ((uint32_t)d << 24);
  }

  mqtt_broker_configuration.ip.ip.v4.value = broker_ip.ip.v4.value;
  mqtt_broker_configuration.ip.type = SL_IPV4;
  printf("MQTT broker IP: %u.%u.%u.%u\r\n",
         (uint8_t)(broker_ip.ip.v4.value),
         (uint8_t)(broker_ip.ip.v4.value >> 8),
         (uint8_t)(broker_ip.ip.v4.value >> 16),
         (uint8_t)(broker_ip.ip.v4.value >> 24));

  /* Connect */
  status = sl_mqtt_client_connect(&client, &mqtt_broker_configuration,
                                  &last_will_message, &mqtt_client_configuration, 0);
  if (status != SL_STATUS_IN_PROGRESS) {
    printf("MQTT connect failed: 0x%lx\r\n", status);
    return status;
  }

  /* Wait for CONNECTED event */
  int waited = 0;
  while (!g_mqtt_connected && waited < 10000) {
    osDelay(100);
    waited += 100;
  }
  if (!g_mqtt_connected) {
    printf("[mqtt] never connected, bailing\r\n");
    return SL_STATUS_FAIL;
  }

  char cmd[64];
  /* Publish loop */
  printf("[bridge] telemetry loop starting (%d nodes, %d ms)\r\n",
         NUM_NODES, TELEMETRY_INTERVAL_MS);
  while (1) {
    for (int i = 0; i < NUM_NODES; i++) {
      publish_node_telemetry(&g_nodes[i]);
      firebase_put(&g_nodes[i]);
      cmd[0] = '\0';
      firebase_get_command(g_nodes[i].id, cmd, sizeof(cmd));
      printf("[cmd %s] %s\n", g_nodes[i].id, cmd);
      handle_command(&g_nodes[i], cmd);
      osDelay(200);
    }
    osDelay(TELEMETRY_INTERVAL_MS);
  }
}
