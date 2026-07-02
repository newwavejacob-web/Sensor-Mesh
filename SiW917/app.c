/***************************************************************************/ /**
 * @file
 * @brief Embedded MQTT Client Example Application
 *******************************************************************************
 * # License
 * <b>Copyright 2022 Silicon Laboratories Inc. www.silabs.com</b>
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

#include "sl_net.h"
#include "sl_utility.h"
#include "cmsis_os2.h"
#include "sl_constants.h"
#include "sl_mqtt_client.h"
#include "cacert.pem.h"
#include "firebase.pem.h"
#include "sl_wifi.h"
#include "string.h"
#include "sl_wifi_callback_framework.h"
#include "sl_net_wifi_types.h"
#include "sl_net_dns.h"
#include <stdlib.h>   // for rand(), srand()
#include "socket.h"
#include "netinet_in.h"

/******************************************************
 *                    Constants
 ******************************************************/

//#ifdef SLI_SI91X_ENABLE_IPV6
//#define MQTT_BROKER_IP "2401:4901:1290:10de::1000"
//#else
// test mosquitto
#define MQTT_BROKER_IP "34.243.217.54"

//#define MQTT_BROKER_IP "test.mosquitto.org"
//#endif

#define MQTT_BROKER_PORT 8883

#define CLIENT_PORT 1

#define CLIENT_ID "siw917-bridge-jr"

#define TOPIC_TO_BE_SUBSCRIBED "ucf/security/jr-test"
#define QOS_OF_SUBSCRIPTION    SL_MQTT_QOS_LEVEL_1

#define PUBLISH_TOPIC          "ucf/security/jacob-cmd"
#define PUBLISH_MESSAGE        "Hello, World"
#define QOS_OF_PUBLISH_MESSAGE 0

#define IS_DUPLICATE_MESSAGE 0
#define IS_MESSAGE_RETAINED  0
#define IS_CLEAN_SESSION     1

#define LAST_WILL_TOPIC       "ucf/security/last-will"
#define LAST_WILL_MESSAGE     "WISECONNECT-SDK-MQTT-CLIENT has been disconnect from network"
#define QOS_OF_LAST_WILL      0
#define IS_LAST_WILL_RETAINED 0

#define ENCRYPT_CONNECTION     1
#define CERTIFICATE_INDEX      1
#define FIREBASE_CERT_INDEX      2
#define KEEP_ALIVE_INTERVAL    60
#define MQTT_CONNECT_TIMEOUT   5000
#define MQTT_KEEPALIVE_RETRIES 3

#define SEND_CREDENTIALS 0

#define USERNAME "jSIW917"
#define PASSWORD "MesosProb1005"

#define NUM_NODES             2
#define TELEMETRY_INTERVAL_MS 4000

typedef struct {
  const char *id;
  float       temp_c;
  float       humidity;
  float       lux;
  uint32_t    seq;
} node_t;

static node_t g_nodes[NUM_NODES] = {
  { "node-0", 22.0f, 45.0f, 300.0f, 0 },
  { "node-1", 21.5f, 50.0f, 280.0f, 0 },
};

static volatile uint8_t g_mqtt_connected = 0;
static char topic_buf[128];
static char payload_buf[256];

/******************************************************
 *               Variable Definitions
 ******************************************************/

const osThreadAttr_t thread_attributes = {
  .name       = "app",
  .attr_bits  = 0,
  .cb_mem     = 0,
  .cb_size    = 0,
  .stack_mem  = 0,
  .stack_size = 3072,
  .priority   = osPriorityLow,
  .tz_module  = 0,
  .reserved   = 0,
};

static const sl_wifi_device_configuration_t wifi_mqtt_client_configuration = {
  .boot_option = LOAD_NWP_FW,
  .mac_address = NULL,
  .band        = SL_SI91X_WIFI_BAND_2_4GHZ,
  .boot_config = { .oper_mode              = SL_SI91X_CLIENT_MODE,
                   .coex_mode              = SL_SI91X_WLAN_BLE_MODE,
                   .feature_bit_map        = (SL_SI91X_FEAT_SECURITY_PSK | SL_SI91X_FEAT_AGGREGATION),
                   .tcp_ip_feature_bit_map = (SL_SI91X_TCP_IP_FEAT_DHCPV4_CLIENT | SL_SI91X_TCP_IP_FEAT_DNS_CLIENT
                                              | SL_SI91X_TCP_IP_FEAT_SSL | SL_SI91X_TCP_IP_FEAT_EXTENSION_VALID
#ifdef SLI_SI91X_ENABLE_IPV6
                                              | SL_SI91X_TCP_IP_FEAT_DHCPV6_CLIENT | SL_SI91X_TCP_IP_FEAT_IPV6
#endif
                                              ),
                   .custom_feature_bit_map     = (SL_SI91X_CUSTOM_FEAT_EXTENTION_VALID),
                   .ext_custom_feature_bit_map = (SL_SI91X_EXT_FEAT_SSL_VERSIONS_SUPPORT | SL_SI91X_EXT_FEAT_XTAL_CLK
                                                  | SL_SI91X_EXT_FEAT_UART_SEL_FOR_DEBUG_PRINTS | MEMORY_CONFIG
#if defined(SLI_SI917) || defined(SLI_SI915)
                                                  | SL_SI91X_EXT_FEAT_FRONT_END_SWITCH_PINS_ULP_GPIO_4_5_0
#endif
                                                  ),
                   .bt_feature_bit_map = 0,
                   .ext_tcp_ip_feature_bit_map =
                     (SL_SI91X_EXT_TCP_IP_WINDOW_SCALING | SL_SI91X_EXT_TCP_IP_TOTAL_SELECTS(10)
                      | SL_SI91X_EXT_TCP_IP_FEAT_SSL_THREE_SOCKETS | SL_SI91X_EXT_TCP_IP_FEAT_SSL_MEMORY_CLOUD
                      | SL_SI91X_EXT_EMB_MQTT_ENABLE),
                   .ble_feature_bit_map     = 0,
                   .ble_ext_feature_bit_map = 0,
                   .config_feature_bit_map  = 0 }
};

sl_mqtt_client_t client = { 0 };

uint8_t is_execution_completed = 0;

sl_mqtt_client_credentials_t *client_credentails = NULL;

sl_mqtt_client_configuration_t mqtt_client_configuration = { .is_clean_session = IS_CLEAN_SESSION,
                                                             .client_id        = (uint8_t *)CLIENT_ID,
                                                             .client_id_length = strlen(CLIENT_ID),
#if ENCRYPT_CONNECTION
                                                             .tls_flags = SL_MQTT_TLS_ENABLE | SL_MQTT_TLS_TLSV_1_2
                                                                          | SL_MQTT_TLS_CERT_INDEX_1,
#endif
                                                             .client_port = CLIENT_PORT };

sl_mqtt_broker_t mqtt_broker_configuration = {
  .port                    = MQTT_BROKER_PORT,
  .is_connection_encrypted = ENCRYPT_CONNECTION,
  .connect_timeout         = MQTT_CONNECT_TIMEOUT,
  .keep_alive_interval     = KEEP_ALIVE_INTERVAL,
  .keep_alive_retries      = MQTT_KEEPALIVE_RETRIES,
};

sl_mqtt_client_message_t message_to_be_published = {
  .qos_level            = QOS_OF_PUBLISH_MESSAGE,
  .is_retained          = IS_MESSAGE_RETAINED,
  .is_duplicate_message = IS_DUPLICATE_MESSAGE,
  .topic                = (uint8_t *)PUBLISH_TOPIC,
  .topic_length         = strlen(PUBLISH_TOPIC),
  .content              = (uint8_t *)PUBLISH_MESSAGE,
  .content_length       = strlen(PUBLISH_MESSAGE),
};

sl_mqtt_client_last_will_message_t last_will_message = {
  .is_retained         = IS_LAST_WILL_RETAINED,
  .will_qos_level      = QOS_OF_LAST_WILL,
  .will_topic          = (uint8_t *)LAST_WILL_TOPIC,
  .will_topic_length   = strlen(LAST_WILL_TOPIC),
  .will_message        = (uint8_t *)LAST_WILL_MESSAGE,
  .will_message_length = strlen(LAST_WILL_MESSAGE),
};

/******************************************************
 *               Function Declarations
 ******************************************************/
static void application_start(void *argument);
void mqtt_client_message_handler(void *client, sl_mqtt_client_message_t *message, void *context);
void mqtt_client_event_handler(void *client, sl_mqtt_client_event_t event, void *event_data, void *context);
void mqtt_client_error_event_handler(void *client, sl_mqtt_client_error_status_t *error);
void mqtt_client_cleanup();
void print_char_buffer(char *buffer, uint32_t buffer_length);
sl_status_t mqtt_example();
static void publish_node_telemetry(node_t *n);
static void sample_node(node_t *n);
static float rand_walk(float cur, float lo, float hi, float step);


/******************************************************
 *               Function Definitions
 ******************************************************/

void app_init(const void *unused)
{
  UNUSED_PARAMETER(unused);
  osThreadNew((osThreadFunc_t)application_start, NULL, &thread_attributes);
}

static void application_start(void *argument)
{
  UNUSED_PARAMETER(argument);
  sl_status_t status;

  status = sl_net_init(SL_NET_WIFI_CLIENT_INTERFACE, &wifi_mqtt_client_configuration, NULL, NULL);
  if (status != SL_STATUS_OK && status != SL_STATUS_ALREADY_INITIALIZED) {
    printf("Failed to start Wi-Fi client interface: 0x%lx\r\n", status);
    return;
  }
  printf("\r\nWi-Fi client interface up Success\r\n");

  sl_net_wifi_client_profile_t wifi_client_profile = {
      .config = {
          .ssid.value = "iPhone",
          .ssid.length = sizeof("iPhone") - 1,
          .channel.channel = SL_WIFI_AUTO_CHANNEL,
          .channel.band = SL_WIFI_AUTO_BAND,
          .channel.bandwidth = SL_WIFI_AUTO_BANDWIDTH,
          .bssid = {{0}},
          .bss_type = SL_WIFI_BSS_TYPE_INFRASTRUCTURE,
          .security = SL_WIFI_WPA2,
          .encryption = SL_WIFI_DEFAULT_ENCRYPTION,
          .client_options = SL_WIFI_JOIN_WITH_SCAN,
          .credential_id = SL_NET_DEFAULT_WIFI_CLIENT_CREDENTIAL_ID,
      },
      .ip = {
          .mode = SL_IP_MANAGEMENT_DHCP,
          .type = SL_IPV4,
          .host_name = NULL,
          .ip = {{{0}}},
      },
  };

  sl_wifi_credential_t cred = { 0 };
  cred.type = SL_WIFI_PSK_CREDENTIAL;
  memcpy(cred.psk.value, "jacobrob", strlen("jacobrob"));

  status = sl_net_set_credential(SL_NET_DEFAULT_WIFI_CLIENT_CREDENTIAL_ID,
                                 SL_NET_WIFI_PSK,
                                 "jacobrob",
                                 strlen("jacobrob"));
  if (status != SL_STATUS_OK) {
      printf("Failed to set wifi credential: 0x%lx\r\n", status);
      return;
  }

  status = sl_net_set_profile(SL_NET_WIFI_CLIENT_INTERFACE,
                              SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID,
                              &wifi_client_profile);
  if (status != SL_STATUS_OK) {
      printf("Failed to set wifi profile: 0x%lx\r\n", status);
      return;
  }

  status = sl_net_up(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID);
  if (status != SL_STATUS_OK) {
    printf("Failed to bring Wi-Fi client interface up: 0x%lx\r\n", status);
    return;
  }

  printf("Wi-Fi client connected\r\n");

  mqtt_example();

  while (1) {
#if defined(SL_CATALOG_POWER_MANAGER_PRESENT)
    // Let the CPU go to sleep if the system allows it.
    sl_power_manager_sleep();
#else
    osDelay(osWaitForever);
#endif
  }
}

void mqtt_client_cleanup()
{
  SL_CLEANUP_MALLOC(client_credentails);
  is_execution_completed = 1;
}

void mqtt_client_message_handler(void *client, sl_mqtt_client_message_t *message, void *context)
{
  UNUSED_PARAMETER(context);
  UNUSED_PARAMETER(client);
  sl_status_t status;
  printf("Message Received on Topic: ");

  print_char_buffer((char *)message->topic, message->topic_length);
  print_char_buffer((char *)message->content, message->content_length);

  // Unsubscribing to already subscribed topic.
  status = sl_mqtt_client_unsubscribe(client,
                                      (uint8_t *)TOPIC_TO_BE_SUBSCRIBED,
                                      strlen(TOPIC_TO_BE_SUBSCRIBED),
                                      0,
                                      TOPIC_TO_BE_SUBSCRIBED);
  if (status != SL_STATUS_IN_PROGRESS) {
    printf("Failed to unsubscribe : 0x%lx\r\n", status);

    mqtt_client_cleanup();
    return;
  }
  //return;
}

void print_char_buffer(char *buffer, uint32_t buffer_length)
{
  for (uint32_t index = 0; index < buffer_length; index++) {
    printf("%c", buffer[index]);
  }

  printf("\r\n");
}

void mqtt_client_error_event_handler(void *client, sl_mqtt_client_error_status_t *error)
{
  UNUSED_PARAMETER(client);
  printf("Terminating program, Error: 0x%lx\r\n", (unsigned long)*error);
}

void mqtt_client_event_handler(void *client, sl_mqtt_client_event_t event, void *event_data, void *context)
{
  switch (event) {
    case SL_MQTT_CLIENT_CONNECTED_EVENT: {
      printf("[mqtt] connected\r\n");
      g_mqtt_connected = 1;
      break;

      break;
    }

    case SL_MQTT_CLIENT_MESSAGE_PUBLISHED_EVENT: {
      sl_mqtt_client_message_t *published_message = (sl_mqtt_client_message_t *)context;

      printf("Published message successfully on topic: ");
      print_char_buffer((char *)published_message->topic, published_message->topic_length);
      break;
    }

    case SL_MQTT_CLIENT_SUBSCRIBED_EVENT: {
      char *subscribed_topic = (char *)context;

      printf("Subscribed to Topic: %s\r\n", subscribed_topic);
      break;
    }

    case SL_MQTT_CLIENT_UNSUBSCRIBED_EVENT: {
      char *unsubscribed_topic = (char *)context;

      printf("Unsubscribed from topic: %s\r\n", unsubscribed_topic);

      sl_mqtt_client_disconnect(client, 0);
      break;
    }

    case SL_MQTT_CLIENT_DISCONNECTED_EVENT: {
      printf("Disconnected from MQTT broker\r\n");

      mqtt_client_cleanup();
      break;
    }

    case SL_MQTT_CLIENT_ERROR_EVENT: {
      mqtt_client_error_event_handler(client, (sl_mqtt_client_error_status_t *)event_data);
      break;
    }
    default:
      break;
  }
}

/* Bounded random walk — makes curves look physical, not noisy */
static float rand_walk(float cur, float lo, float hi, float step)
{
  int   r     = (rand() % 2001) - 1000;           // -1000..+1000
  float delta = ((float)r / 1000.0f) * step;
  float nxt   = cur + delta;
  if (nxt < lo) nxt = lo;
  if (nxt > hi) nxt = hi;
  return nxt;
}

static void sample_node(node_t *n)
{
  n->temp_c   = rand_walk(n->temp_c,   18.0f,   30.0f, 0.15f);
  n->humidity = rand_walk(n->humidity, 30.0f,   70.0f, 0.50f);
  n->lux      = rand_walk(n->lux,       0.0f, 1000.0f, 8.00f);
  n->seq++;
}

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

static void firebase_put(node_t *n)
{
  sl_status_t status;
  int sock;

  char request[512];
  char response[512];

  //uint32_t ts = n->seq; // or real timestamp later
  //uint32_t ts = sl_sleeptimer_get_tick_count();
  //uint32_t ts = (uint32_t)time(NULL);
  char body[128];

  snprintf(body, sizeof(body),
    "{\"temp\":%.2f,\"humidity\":%.2f,\"lux\":%.1f}",
    n->temp_c, n->humidity, n->lux
  );

  snprintf(request, sizeof(request),
    "PUT /nodes/%s.json HTTP/1.1\r\n"
    "Host: ucf-jr-default-rtdb.firebaseio.com\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n"
    "\r\n"
    "%s",
    n->id,
    strlen(body),
    body
  );

  // 1. Create TLS socket
  sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    printf("Socket failed\n");
    return;
  }

  // 2. Enable TLS
  uint8_t cert_index = FIREBASE_CERT_INDEX;

  setsockopt(sock,
             SOL_SOCKET,
             SL_SO_CERT_INDEX,
             &cert_index,
             sizeof(cert_index));

  setsockopt(sock,
             SOL_TCP,
             TCP_ULP,
             "tls",
             sizeof("tls"));

  // 3. Resolve hostname
  sl_ip_address_t ip;
  status = sl_net_dns_resolve_hostname(
    "ucf-jr-default-rtdb.firebaseio.com",
    10000,
    SL_NET_DNS_TYPE_IPV4,
    &ip
  );

  if (status != SL_STATUS_OK) {
    printf("DNS failed\n");
    return;
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(443);
  addr.sin_addr.s_addr = ip.ip.v4.value;

  // 4. Connect
  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    printf("Connect failed\n");
    close(sock);
    return;
  }

  // 5. Send request
  send(sock, request, strlen(request), 0);

  // 6. Read response (optional)
  int len = recv(sock, response, sizeof(response), 0);
  if (len > 0) {
    response[len] = '\0';
    printf("[firebase] %s\n", response);
  }

  // 7. Close immediately
  close(sock);
}


sl_status_t mqtt_example()
{
  sl_status_t status;

  if (ENCRYPT_CONNECTION) {
    // Load SSL CA certificate
    status = sl_net_set_credential(SL_NET_TLS_SERVER_CREDENTIAL_ID(CERTIFICATE_INDEX),
                                   SL_NET_SIGNING_CERTIFICATE,
                                   cacert,
                                   sizeof(cacert) - 1);
    if (status != SL_STATUS_OK) {
      printf("\r\nLoading TLS CA certificate in to FLASH Failed, Error Code : 0x%lX\r\n", status);
      return status;
    }
    printf("\r\nLoad TLS CA certificate at index %d Success\r\n", CERTIFICATE_INDEX);

    status = sl_net_set_credential(
          SL_NET_TLS_SERVER_CREDENTIAL_ID(FIREBASE_CERT_INDEX),
          SL_NET_SIGNING_CERTIFICATE,
          firebase_ca,
          sizeof(firebase_ca) - 1
        );

        if (status != SL_STATUS_OK) {
          mqtt_client_cleanup();
          printf("Failed to load firebase certificate: 0x%lx\r\n ", status);

          return status;
        }
        printf("load firebase credentials Success \r\n ");
  }

  if (SEND_CREDENTIALS) {
    uint16_t username_length, password_length;

    username_length = strlen(USERNAME);
    password_length = strlen(PASSWORD);

    uint32_t malloc_size = sizeof(sl_mqtt_client_credentials_t) + username_length + password_length;

    client_credentails = malloc(malloc_size);
    if (client_credentails == NULL)
      return SL_STATUS_ALLOCATION_FAILED;
    memset(client_credentails, 0, malloc_size);
    client_credentails->username_length = username_length;
    client_credentails->password_length = password_length;

    memcpy(&client_credentails->data[0], USERNAME, username_length);
    memcpy(&client_credentails->data[username_length], PASSWORD, password_length);

    status = sl_net_set_credential(SL_NET_MQTT_CLIENT_CREDENTIAL_ID(0),
                                   SL_NET_MQTT_CLIENT_CREDENTIAL,
                                   client_credentails,
                                   malloc_size);

    if (status != SL_STATUS_OK) {
      mqtt_client_cleanup();
      printf("Failed to set credentials: 0x%lx\r\n ", status);

      return status;
    }
    printf("Set credentials Success \r\n ");

    free(client_credentails);
    mqtt_client_configuration.credential_id = SL_NET_MQTT_CLIENT_CREDENTIAL_ID(0);
  }

  status = sl_mqtt_client_init(&client, mqtt_client_event_handler);
  if (status != SL_STATUS_OK) {
    printf("Failed to init mqtt client: 0x%lx\r\n", status);

    mqtt_client_cleanup();
    return status;
  }
  printf("Init mqtt client Success \r\n");

#ifdef SLI_SI91X_ENABLE_IPV6
  unsigned char hex_addr[SL_IPV6_ADDRESS_LENGTH] = { 0 };
  status                                         = sl_inet_pton6(MQTT_BROKER_IP,
                         MQTT_BROKER_IP + strlen(MQTT_BROKER_IP),
                         hex_addr,
                         (unsigned int *)mqtt_broker_configuration.ip.ip.v6.value);
  if (status != 0x1) {
    printf("\r\nIPv6 conversion failed.\r\n");
    mqtt_client_cleanup();
    return status;
  }
  mqtt_broker_configuration.ip.type = SL_IPV6;
#else
  sl_ip_address_t broker_ip = {0};

  status = sl_net_dns_resolve_hostname("broker.emqx.io",
                                        10000,
                                        SL_NET_DNS_TYPE_IPV4,
                                        &broker_ip);
  if (status != SL_STATUS_OK) {
    printf("Failed to convert IP address \r\n");

    mqtt_client_cleanup();
    return status;
  }

  mqtt_broker_configuration.ip.ip.v4.value = broker_ip.ip.v4.value;
  mqtt_broker_configuration.ip.type = SL_IPV4;

  printf("Resolved to IP: %u.%u.%u.%u\r\n",
          (uint8_t)(broker_ip.ip.v4.value & 0xFF),
          (uint8_t)((broker_ip.ip.v4.value >> 8) & 0xFF),
          (uint8_t)((broker_ip.ip.v4.value >> 16) & 0xFF),
          (uint8_t)((broker_ip.ip.v4.value >> 24) & 0xFF));

#endif

  status =
    sl_mqtt_client_connect(&client, &mqtt_broker_configuration, &last_will_message, &mqtt_client_configuration, 0);
  if (status != SL_STATUS_IN_PROGRESS) {
    printf("Failed to connect to mqtt broker: 0x%lx\r\n", status);

    mqtt_client_cleanup();
    return status;
  }
  printf("Connect to mqtt broker Success \r\n");

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

  srand((unsigned)osKernelGetTickCount());

  printf("[aegis] telemetry loop starting (%d nodes, %d ms)\r\n",
         NUM_NODES, TELEMETRY_INTERVAL_MS);

  while (1) {
    for (int i = 0; i < NUM_NODES; i++) {
      sample_node(&g_nodes[i]);
      publish_node_telemetry(&g_nodes[i]);
      firebase_put(&g_nodes[i]);

      osDelay(200); // IMPORTANT: give TLS time
    }
    osDelay(TELEMETRY_INTERVAL_MS);
  }

  printf("Example execution completed \r\n");

  return SL_STATUS_OK;
}
