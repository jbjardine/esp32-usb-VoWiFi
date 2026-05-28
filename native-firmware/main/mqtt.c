#include "mqtt.h"
#include "nvs_config.h"
#include "usb.h"

#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqtt_client.h>

static const char *TAG = "mqtt";

#define DEVICE_PREFIX_MAX 32
#define TOPIC_MAX 160
#define PAYLOAD_MAX 1024
#define DEVICE_BLOCK_MAX 384

static esp_mqtt_client_handle_t s_client = NULL;
static nvs_config_mqtt_t s_cfg;
static char s_device_id[DEVICE_PREFIX_MAX];   /* e.g. "esp_wakeup_a1b2c3" */
static char s_topic_root[DEVICE_PREFIX_MAX];  /* e.g. "esp-wakeup-a1b2c3" */
static bool s_connected = false;
static uint32_t s_wakeup_count = 0;

static const char *device_block(char *out, size_t out_size) {
	snprintf(out, out_size,
	         "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"ESP Wakeup %s\","
	         "\"manufacturer\":\"esp-wakeup-keypress\",\"model\":\"ESP32-S3\","
	         "\"sw_version\":\"v6.0.1\"}",
	         s_device_id, s_device_id + sizeof("esp_wakeup_") - 1);
	return out;
}

static void publish_discovery(void) {
	char topic[TOPIC_MAX];
	char payload[PAYLOAD_MAX];
	char dev[DEVICE_BLOCK_MAX];
	device_block(dev, sizeof(dev));

	const char *avail = s_topic_root;  /* concat /availability later */

	/* Button: Wakeup PC */
	snprintf(topic, sizeof(topic), "%s/button/%s/wakeup/config", s_cfg.discovery_prefix, s_device_id);
	snprintf(payload, sizeof(payload),
		"{\"name\":\"Wakeup PC\",\"object_id\":\"%s_wakeup\",\"unique_id\":\"%s_wakeup\","
		"\"command_topic\":\"%s/cmd/wakeup\",\"payload_press\":\"PRESS\","
		"\"availability_topic\":\"%s/availability\",%s}",
		s_device_id, s_device_id, s_topic_root, avail, dev);
	esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

	/* Sensor: RSSI */
	snprintf(topic, sizeof(topic), "%s/sensor/%s/rssi/config", s_cfg.discovery_prefix, s_device_id);
	snprintf(payload, sizeof(payload),
		"{\"name\":\"RSSI\",\"object_id\":\"%s_rssi\",\"unique_id\":\"%s_rssi\","
		"\"state_topic\":\"%s/state/rssi\",\"unit_of_measurement\":\"dBm\","
		"\"device_class\":\"signal_strength\",\"state_class\":\"measurement\","
		"\"entity_category\":\"diagnostic\","
		"\"availability_topic\":\"%s/availability\",%s}",
		s_device_id, s_device_id, s_topic_root, avail, dev);
	esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

	/* Sensor: Uptime */
	snprintf(topic, sizeof(topic), "%s/sensor/%s/uptime/config", s_cfg.discovery_prefix, s_device_id);
	snprintf(payload, sizeof(payload),
		"{\"name\":\"Uptime\",\"object_id\":\"%s_uptime\",\"unique_id\":\"%s_uptime\","
		"\"state_topic\":\"%s/state/uptime\",\"unit_of_measurement\":\"s\","
		"\"device_class\":\"duration\",\"state_class\":\"total_increasing\","
		"\"entity_category\":\"diagnostic\","
		"\"availability_topic\":\"%s/availability\",%s}",
		s_device_id, s_device_id, s_topic_root, avail, dev);
	esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

	/* Sensor: IP */
	snprintf(topic, sizeof(topic), "%s/sensor/%s/ip/config", s_cfg.discovery_prefix, s_device_id);
	snprintf(payload, sizeof(payload),
		"{\"name\":\"IP\",\"object_id\":\"%s_ip\",\"unique_id\":\"%s_ip\","
		"\"state_topic\":\"%s/state/ip\","
		"\"entity_category\":\"diagnostic\","
		"\"availability_topic\":\"%s/availability\",%s}",
		s_device_id, s_device_id, s_topic_root, avail, dev);
	esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

	/* Binary sensor: USB Connected */
	snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/usb/config", s_cfg.discovery_prefix, s_device_id);
	snprintf(payload, sizeof(payload),
		"{\"name\":\"USB Connected\",\"object_id\":\"%s_usb\",\"unique_id\":\"%s_usb\","
		"\"state_topic\":\"%s/state/usb\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
		"\"device_class\":\"connectivity\","
		"\"availability_topic\":\"%s/availability\",%s}",
		s_device_id, s_device_id, s_topic_root, avail, dev);
	esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

	/* Sensor: Wakeup Count */
	snprintf(topic, sizeof(topic), "%s/sensor/%s/wakeup_count/config", s_cfg.discovery_prefix, s_device_id);
	snprintf(payload, sizeof(payload),
		"{\"name\":\"Wakeup Count\",\"object_id\":\"%s_wakeup_count\",\"unique_id\":\"%s_wakeup_count\","
		"\"state_topic\":\"%s/state/wakeup_count\","
		"\"state_class\":\"total_increasing\","
		"\"availability_topic\":\"%s/availability\",%s}",
		s_device_id, s_device_id, s_topic_root, avail, dev);
	esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);

	ESP_LOGI(TAG, "discovery configs published (6 entities)");
}

static void publish_telemetry(void) {
	if (!s_connected) return;
	char topic[TOPIC_MAX];
	char val[64];

	wifi_ap_record_t ap = { 0 };
	int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
	snprintf(topic, sizeof(topic), "%s/state/rssi", s_topic_root);
	snprintf(val, sizeof(val), "%d", rssi);
	esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);

	snprintf(topic, sizeof(topic), "%s/state/uptime", s_topic_root);
	snprintf(val, sizeof(val), "%llu", (unsigned long long)(esp_timer_get_time() / 1000000ULL));
	esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);

	esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	if (sta) {
		esp_netif_ip_info_t ip_info;
		if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
			snprintf(topic, sizeof(topic), "%s/state/ip", s_topic_root);
			snprintf(val, sizeof(val), IPSTR, IP2STR(&ip_info.ip));
			esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);
		}
	}

	snprintf(topic, sizeof(topic), "%s/state/usb", s_topic_root);
	esp_mqtt_client_publish(s_client, topic, usb_is_mounted() ? "ON" : "OFF", 0, 0, 1);
}

static void telemetry_task(void *arg) {
	while (1) {
		publish_telemetry();
		vTaskDelay(pdMS_TO_TICKS(30000));
	}
}

void mqtt_notify_wakeup(void) {
	if (!s_connected) return;
	s_wakeup_count++;
	char topic[TOPIC_MAX];
	char val[16];
	snprintf(topic, sizeof(topic), "%s/state/wakeup_count", s_topic_root);
	snprintf(val, sizeof(val), "%lu", (unsigned long)s_wakeup_count);
	esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
	esp_mqtt_event_handle_t event = event_data;
	char topic[TOPIC_MAX];

	switch ((esp_mqtt_event_id_t)event_id) {
	case MQTT_EVENT_CONNECTED:
		ESP_LOGI(TAG, "connected to broker");
		s_connected = true;
		/* Publish availability "online" (retained). */
		snprintf(topic, sizeof(topic), "%s/availability", s_topic_root);
		esp_mqtt_client_publish(s_client, topic, "online", 0, 1, 1);
		/* Subscribe to command. */
		snprintf(topic, sizeof(topic), "%s/cmd/wakeup", s_topic_root);
		esp_mqtt_client_subscribe(s_client, topic, 1);
		publish_discovery();
		publish_telemetry();
		break;
	case MQTT_EVENT_DISCONNECTED:
		ESP_LOGW(TAG, "disconnected from broker");
		s_connected = false;
		break;
	case MQTT_EVENT_DATA:
		if (event->topic && event->topic_len < TOPIC_MAX) {
			memcpy(topic, event->topic, event->topic_len);
			topic[event->topic_len] = '\0';
			ESP_LOGI(TAG, "data received on %s (%d bytes)", topic, event->data_len);
			if (strstr(topic, "/cmd/wakeup")) {
				ESP_LOGI(TAG, "remote wakeup command received");
				usb_request_keypress_send(false);
				/* mqtt_notify_wakeup() is called by usb_task once the
				 * wakeup actually fires, so no double-publish here. */
			}
		}
		break;
	case MQTT_EVENT_ERROR:
		ESP_LOGW(TAG, "mqtt error");
		break;
	default:
		break;
	}
}

void mqtt_init(void) {
	if (nvs_config_get_mqtt(&s_cfg) != ESP_OK) {
		ESP_LOGW(TAG, "no mqtt config in NVS");
		return;
	}
	if (!s_cfg.enabled) {
		ESP_LOGI(TAG, "mqtt disabled, skipping init");
		return;
	}
	if (s_cfg.broker[0] == '\0') {
		ESP_LOGW(TAG, "mqtt enabled but broker URL empty, skipping");
		return;
	}

	/* Build device id from MAC: "esp_wakeup_XXYYZZ" (last 3 bytes hex). */
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	snprintf(s_device_id, sizeof(s_device_id), "esp_wakeup_%02x%02x%02x", mac[3], mac[4], mac[5]);
	snprintf(s_topic_root, sizeof(s_topic_root), "esp-wakeup-%02x%02x%02x", mac[3], mac[4], mac[5]);

	char lwt_topic[TOPIC_MAX];
	snprintf(lwt_topic, sizeof(lwt_topic), "%s/availability", s_topic_root);

	esp_mqtt_client_config_t mqtt_cfg = {
		.broker.address.uri = s_cfg.broker,
		.credentials.client_id = s_device_id,
		.session.last_will.topic = lwt_topic,
		.session.last_will.msg = "offline",
		.session.last_will.qos = 1,
		.session.last_will.retain = 1,
	};
	if (s_cfg.user[0]) {
		mqtt_cfg.credentials.username = s_cfg.user;
		mqtt_cfg.credentials.authentication.password = s_cfg.password;
	}

	s_client = esp_mqtt_client_init(&mqtt_cfg);
	if (!s_client) {
		ESP_LOGE(TAG, "esp_mqtt_client_init failed");
		return;
	}
	esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
	esp_mqtt_client_start(s_client);

	xTaskCreate(telemetry_task, "mqtt_telem", 4096, NULL, 4, NULL);
	ESP_LOGI(TAG, "mqtt started (broker=%s, device=%s)", s_cfg.broker, s_device_id);
}

bool mqtt_is_connected(void) {
	return s_connected;
}
