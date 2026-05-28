#include "main.h"
#include "led.h"
#include "nvs_config.h"
#include "captive_portal.h"
#include "wifi.h"

#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <mdns.h>

static const char *TAG = "wifi";

#define APP_WIFI_STA_NAME "wifi"
#define APP_WIFI_AP_NAME "ap"
#define APP_WIFI_STA_MAX_ATTEMPTS 3
#define APP_WIFI_STA_ATTEMPT_TIMEOUT_MS 15000

static esp_netif_t *wifi_sta_netif = NULL;
static esp_netif_t *wifi_ap_netif = NULL;
static SemaphoreHandle_t wifi_semph_get_ip4_addrs = NULL;
static SemaphoreHandle_t wifi_semph_get_ip6_addrs = NULL;

static esp_event_handler_instance_t s_handler_disconnect = NULL;
static esp_event_handler_instance_t s_handler_connect = NULL;
static esp_event_handler_instance_t s_handler_got_ip = NULL;
static esp_event_handler_instance_t s_handler_got_ipv6 = NULL;

static bool s_sta_connected = false;
static bool s_ap_mode = false;

static esp_err_t wifi_connect_sta(int max_attempts);
static void wifi_start_softap(void);

static void wifi_handler_on_wifi_disconnect(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	led_handle_wifi_disconnected();
	if (!s_sta_connected) {
		/* Initial connect phase: the bounded retry loop in wifi_connect_sta()
		 * drives reconnection attempts; do not double-up from here. */
		return;
	}
	ESP_LOGW(TAG, "wi-fi disconnected, trying to reconnect...");
	esp_err_t err = esp_wifi_connect();
	if (err == ESP_ERR_WIFI_NOT_STARTED)
		return;
	if (err != ESP_OK)
		ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
}

static void wifi_handler_on_wifi_connect(void *esp_netif, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	esp_netif_create_ip6_linklocal(esp_netif);
}

static bool wifi_is_our_sta_netif(esp_netif_t *esp_netif) {
	const char *desc = esp_netif_get_desc(esp_netif);
	return desc && strcmp(APP_WIFI_STA_NAME, desc) == 0;
}

static void wifi_handler_on_sta_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
	if (!wifi_is_our_sta_netif(event->esp_netif))
		return;

	ESP_LOGI(TAG, "got ipv4 event: interface \"%s\" address: " IPSTR, esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
	s_sta_connected = true;
	xSemaphoreGive(wifi_semph_get_ip4_addrs);
	led_handle_wifi_connected();
}

static void wifi_handler_on_sta_got_ipv6(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
	if (!wifi_is_our_sta_netif(event->esp_netif))
		return;

	const char *ipv6_addr_types_to_str[6] = {
		"ESP_IP6_ADDR_IS_UNKNOWN",
		"ESP_IP6_ADDR_IS_GLOBAL",
		"ESP_IP6_ADDR_IS_LINK_LOCAL",
		"ESP_IP6_ADDR_IS_SITE_LOCAL",
		"ESP_IP6_ADDR_IS_UNIQUE_LOCAL",
		"ESP_IP6_ADDR_IS_IPV4_MAPPED_IPV6"
	};

	esp_ip6_addr_type_t ipv6_type = esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
	ESP_LOGI(TAG, "got ipv6 event: interface \"%s\" address: " IPV6STR ", type: %s", esp_netif_get_desc(event->esp_netif),
		IPV62STR(event->ip6_info.ip), ipv6_addr_types_to_str[ipv6_type]);

	if (ipv6_type == ESP_IP6_ADDR_IS_LINK_LOCAL)
		xSemaphoreGive(wifi_semph_get_ip6_addrs);
}

static esp_err_t wifi_connect_sta(int max_attempts) {
	char ssid[NVS_CONFIG_WIFI_SSID_MAX] = { 0 };
	char password[NVS_CONFIG_WIFI_PASSWORD_MAX] = { 0 };
	esp_err_t cred_err = nvs_config_get_wifi(ssid, sizeof(ssid), password, sizeof(password));
	if (cred_err != ESP_OK || ssid[0] == '\0') {
		ESP_LOGW(TAG, "no usable wifi credentials in NVS: %s",
		         cred_err == ESP_OK ? "(empty ssid)" : esp_err_to_name(cred_err));
		return ESP_FAIL;
	}

	wifi_config_t wifi_config = {
		.sta = {
			.scan_method = WIFI_ALL_CHANNEL_SCAN
		},
	};
	strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
	strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

	/* Apply the configured hostname before starting wifi so it lands in
	 * the initial DHCP DISCOVER (some routers tag clients off this only).
	 * Also publish it via mDNS so '<hostname>.local' resolves on every OS. */
	char hostname[NVS_CONFIG_HOSTNAME_MAX] = { 0 };
	if (nvs_config_get_hostname(hostname, sizeof(hostname)) == ESP_OK && hostname[0]) {
		esp_netif_set_hostname(wifi_sta_netif, hostname);
		if (mdns_init() == ESP_OK) {
			mdns_hostname_set(hostname);
			mdns_instance_name_set("ESP Wakeup Keypress");
			mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
		}
	}

	ESP_ERROR_CHECK(esp_wifi_start());
	/* WIFI_PS_NONE: device is USB-powered, latency > energy for HTTP/MQTT. */
	esp_wifi_set_ps(WIFI_PS_NONE);

	for (int attempt = 1; attempt <= max_attempts; attempt++) {
		ESP_LOGI(TAG, "STA connect attempt %d/%d to '%s'", attempt, max_attempts, wifi_config.sta.ssid);
		esp_err_t ret = esp_wifi_connect();
		if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
			ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(ret));
			vTaskDelay(pdMS_TO_TICKS(2000));
			continue;
		}

		if (xSemaphoreTake(wifi_semph_get_ip4_addrs, pdMS_TO_TICKS(APP_WIFI_STA_ATTEMPT_TIMEOUT_MS)) == pdTRUE) {
			/* Best-effort IPv6 wait, doesn't affect success. */
			xSemaphoreTake(wifi_semph_get_ip6_addrs, pdMS_TO_TICKS(5000));
			ESP_LOGI(TAG, "STA connected on attempt %d", attempt);
			return ESP_OK;
		}

		ESP_LOGW(TAG, "no ip4 within timeout on attempt %d", attempt);
		esp_wifi_disconnect();
		vTaskDelay(pdMS_TO_TICKS(2000));
	}

	ESP_LOGE(TAG, "STA connection failed after %d attempts", max_attempts);
	return ESP_FAIL;
}

static void wifi_start_softap(void) {
	ESP_LOGI(TAG, "falling back to SoftAP mode");
	esp_wifi_stop();

	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
	char ap_ssid[33];
	snprintf(ap_ssid, sizeof(ap_ssid), "ESP-Wakeup-%02X%02X", mac[4], mac[5]);

	wifi_config_t ap_config = {
		.ap = {
			.channel = 1,
			.max_connection = 4,
			.authmode = WIFI_AUTH_OPEN,
		},
	};
	strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
	ap_config.ap.ssid_len = strlen(ap_ssid);

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	s_ap_mode = true;

	ESP_LOGI(TAG, "SoftAP up: SSID=\"%s\" (open), gateway 192.168.4.1", ap_ssid);
	led_handle_wifi_disconnected();  /* visual cue: solid red */

	captive_portal_start();
}

bool wifi_is_softap_mode(void) {
	return s_ap_mode;
}

void wifi_init(void) {
	ESP_LOGI(TAG, "initializing wifi");
	/* NVS is initialized by nvs_config_init() in app_main(), no need to repeat. */
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	/* Create STA netif with our custom description (so wifi_is_our_sta_netif()
	 * can identify our STA in the multi-netif IP_EVENT handler). */
	esp_netif_inherent_config_t sta_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
	sta_netif_config.if_desc = APP_WIFI_STA_NAME;
	sta_netif_config.route_prio = 128;
	wifi_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &sta_netif_config);
	esp_wifi_set_default_wifi_sta_handlers();

	/* Always create the AP netif as well so we can switch to SoftAP without
	 * recreating networking primitives at runtime. */
	esp_netif_inherent_config_t ap_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
	ap_netif_config.if_desc = APP_WIFI_AP_NAME;
	wifi_ap_netif = esp_netif_create_wifi(WIFI_IF_AP, &ap_netif_config);
	esp_wifi_set_default_wifi_ap_handlers();

	wifi_semph_get_ip4_addrs = xSemaphoreCreateBinary();
	wifi_semph_get_ip6_addrs = xSemaphoreCreateBinary();

	/* Register event handlers once. The disconnect handler is gated on
	 * s_sta_connected so it stays quiet during the initial bounded retry. */
	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
		&wifi_handler_on_wifi_disconnect, NULL, &s_handler_disconnect));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		IP_EVENT, IP_EVENT_STA_GOT_IP,
		&wifi_handler_on_sta_got_ip, NULL, &s_handler_got_ip));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
		&wifi_handler_on_wifi_connect, wifi_sta_netif, &s_handler_connect));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		IP_EVENT, IP_EVENT_GOT_IP6,
		&wifi_handler_on_sta_got_ipv6, NULL, &s_handler_got_ipv6));

	if (nvs_config_has_wifi()) {
		if (wifi_connect_sta(APP_WIFI_STA_MAX_ATTEMPTS) == ESP_OK)
			return;
	} else {
		ESP_LOGI(TAG, "no wifi credentials configured, going straight to SoftAP");
	}

	wifi_start_softap();
}
