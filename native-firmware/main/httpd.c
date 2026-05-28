#include "main.h"
#include "usb.h"
#include "nvs_config.h"
#include "wifi.h"
#include "auth.h"
#include "ota.h"

#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define HTTPD_PASSWORD_MAX_LEN 64
#define HTTPD_FORM_BODY_MAX 512

static const char *TAG = "httpd";

/* Embedded HTML pages (linked via EMBED_FILES). */
extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[]   asm("_binary_setup_html_end");
extern const uint8_t admin_html_start[] asm("_binary_admin_html_start");
extern const uint8_t admin_html_end[]   asm("_binary_admin_html_end");

/* ----- helpers ---------------------------------------------------------- */

static void send_simple(httpd_req_t *req, const char *status, const char *body) {
	httpd_resp_set_status(req, status);
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_body(httpd_req_t *req, char *out, size_t out_size) {
	if (req->content_len <= 0 || (size_t)req->content_len >= out_size) {
		send_simple(req, "400 Bad Request", "bad body size");
		return ESP_FAIL;
	}
	int total = 0;
	while (total < req->content_len) {
		int n = httpd_req_recv(req, out + total, req->content_len - total);
		if (n <= 0) {
			if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
			send_simple(req, "500 Internal Server Error", "recv failed");
			return ESP_FAIL;
		}
		total += n;
	}
	out[total] = '\0';
	return ESP_OK;
}

static void delayed_restart_task(void *arg) {
	vTaskDelay(pdMS_TO_TICKS(2000));
	ESP_LOGI(TAG, "rebooting now");
	esp_restart();
}

static void schedule_reboot(void) {
	xTaskCreate(delayed_restart_task, "restart", 2048, NULL, 5, NULL);
}

/* ----- handlers --------------------------------------------------------- */

static esp_err_t httpd_wakeup_get_handler(httpd_req_t *req) {
	bool auth_ok = false;

	size_t buf_len = httpd_req_get_url_query_len(req) + 1;
	if (buf_len > 1) {
		char *buf = malloc(buf_len);
		if (!buf) {
			ESP_LOGE(TAG, "nomem for query buffer");
			return ESP_ERR_NO_MEM;
		}
		if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
			char param[HTTPD_PASSWORD_MAX_LEN] = { 0 };
			if (httpd_query_key_value(buf, "pass", param, sizeof(param)) == ESP_OK) {
				ESP_LOGI(TAG, "pass param received (len=%u)", (unsigned)strnlen(param, sizeof(param)));
				if (nvs_config_check_httpd_password(param))
					auth_ok = true;
			}
		}
		free(buf);
	}

	if (auth_ok) {
		ESP_LOGI(TAG, "auth ok, sending keypress");
		usb_request_keypress_send(false);
		httpd_resp_set_type(req, "application/json");
		httpd_resp_send(req, "{\"keypress_sent\":true}", HTTPD_RESP_USE_STRLEN);
	} else {
		ESP_LOGW(TAG, "invalid auth on /wakeup");
		httpd_resp_set_status(req, "401 Unauthorized");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
	}
	return ESP_OK;
}

static esp_err_t httpd_root_get_handler(httpd_req_t *req) {
	if (wifi_is_softap_mode()) {
		/* Captive setup page, no auth (we're isolated on SoftAP). */
		const size_t len = setup_html_end - setup_html_start;
		httpd_resp_set_type(req, "text/html; charset=utf-8");
		httpd_resp_set_hdr(req, "Cache-Control", "no-store");
		httpd_resp_send(req, (const char *)setup_html_start, len);
		return ESP_OK;
	}
	if (!auth_require_admin(req)) return ESP_OK;
	const size_t len = admin_html_end - admin_html_start;
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	httpd_resp_send(req, (const char *)admin_html_start, len);
	return ESP_OK;
}

static esp_err_t httpd_setup_get_handler(httpd_req_t *req) {
	const size_t len = setup_html_end - setup_html_start;
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	httpd_resp_send(req, (const char *)setup_html_start, len);
	return ESP_OK;
}

static esp_err_t httpd_setup_post_handler(httpd_req_t *req) {
	char body[HTTPD_FORM_BODY_MAX];
	if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_OK;

	char ssid[NVS_CONFIG_WIFI_SSID_MAX] = { 0 };
	char password[NVS_CONFIG_WIFI_PASSWORD_MAX] = { 0 };
	if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
		send_simple(req, "400 Bad Request", "missing ssid");
		return ESP_OK;
	}
	(void)httpd_query_key_value(body, "password", password, sizeof(password));

	ESP_LOGI(TAG, "saving wifi creds via setup");
	esp_err_t err = nvs_config_set_wifi(ssid, password);
	if (err != ESP_OK) {
		send_simple(req, "500 Internal Server Error", "nvs write failed");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"ok\":true,\"reboot_in_ms\":2000}", HTTPD_RESP_USE_STRLEN);
	schedule_reboot();
	return ESP_OK;
}

static esp_err_t httpd_captive_redirect_handler(httpd_req_t *req) {
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", "/");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

static esp_err_t httpd_api_status_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;

	wifi_ap_record_t ap = { 0 };
	bool sta_connected = !wifi_is_softap_mode() && (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

	char ip_str[16] = "0.0.0.0";
	esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	if (sta) {
		esp_netif_ip_info_t ip_info;
		if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK)
			snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
	}

	char hostname[NVS_CONFIG_HOSTNAME_MAX] = { 0 };
	nvs_config_get_hostname(hostname, sizeof(hostname));

	char body[320];
	int n = snprintf(body, sizeof(body),
		"{\"usb_mounted\":%s,\"wifi_ssid\":\"%.32s\",\"ip\":\"%s\",\"rssi\":%d,"
		"\"uptime_s\":%llu,\"free_heap\":%u,\"mode\":\"%s\",\"hostname\":\"%s\"}",
		usb_is_mounted() ? "true" : "false",
		sta_connected ? (const char *)ap.ssid : "",
		ip_str,
		sta_connected ? ap.rssi : 0,
		(unsigned long long)(esp_timer_get_time() / 1000000ULL),
		(unsigned)esp_get_free_heap_size(),
		wifi_is_softap_mode() ? "softap" : "sta",
		hostname);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, body, n > 0 ? n : 0);
	return ESP_OK;
}

static esp_err_t httpd_api_wakeup_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;
	ESP_LOGI(TAG, "admin wakeup");
	usb_request_keypress_send(false);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"keypress_sent\":true}", HTTPD_RESP_USE_STRLEN);
	return ESP_OK;
}

static esp_err_t httpd_admin_wifi_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;

	char body[HTTPD_FORM_BODY_MAX];
	if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_OK;

	char ssid[NVS_CONFIG_WIFI_SSID_MAX] = { 0 };
	char password[NVS_CONFIG_WIFI_PASSWORD_MAX] = { 0 };
	if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
		send_simple(req, "400 Bad Request", "missing ssid");
		return ESP_OK;
	}
	(void)httpd_query_key_value(body, "password", password, sizeof(password));

	if (nvs_config_set_wifi(ssid, password) != ESP_OK) {
		send_simple(req, "500 Internal Server Error", "nvs write failed");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"ok\":true,\"reboot_in_ms\":2000}", HTTPD_RESP_USE_STRLEN);
	schedule_reboot();
	return ESP_OK;
}

static esp_err_t httpd_admin_passwords_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;

	char body[HTTPD_FORM_BODY_MAX];
	if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_OK;

	char wakeup[HTTPD_PASSWORD_MAX_LEN] = { 0 };
	char admin_pwd[HTTPD_PASSWORD_MAX_LEN] = { 0 };
	bool any = false;
	if (httpd_query_key_value(body, "wakeup", wakeup, sizeof(wakeup)) == ESP_OK && wakeup[0]) {
		if (nvs_config_set_httpd_password(wakeup) != ESP_OK) {
			send_simple(req, "500 Internal Server Error", "wakeup pwd write failed");
			return ESP_OK;
		}
		any = true;
	}
	if (httpd_query_key_value(body, "admin", admin_pwd, sizeof(admin_pwd)) == ESP_OK && admin_pwd[0]) {
		if (nvs_config_set_admin_password(admin_pwd) != ESP_OK) {
			send_simple(req, "500 Internal Server Error", "admin pwd write failed");
			return ESP_OK;
		}
		any = true;
	}
	if (!any) {
		send_simple(req, "400 Bad Request", "no password provided");
		return ESP_OK;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
	return ESP_OK;
}

static esp_err_t httpd_admin_hostname_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;
	char body[HTTPD_FORM_BODY_MAX];
	if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_OK;
	char hostname[NVS_CONFIG_HOSTNAME_MAX] = { 0 };
	if (httpd_query_key_value(body, "hostname", hostname, sizeof(hostname)) != ESP_OK || hostname[0] == '\0') {
		send_simple(req, "400 Bad Request", "missing hostname");
		return ESP_OK;
	}
	if (nvs_config_set_hostname(hostname) != ESP_OK) {
		send_simple(req, "500 Internal Server Error", "nvs write failed");
		return ESP_OK;
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"ok\":true,\"reboot_in_ms\":2000}", HTTPD_RESP_USE_STRLEN);
	schedule_reboot();
	return ESP_OK;
}

static esp_err_t httpd_admin_reboot_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"ok\":true,\"reboot_in_ms\":2000}", HTTPD_RESP_USE_STRLEN);
	schedule_reboot();
	return ESP_OK;
}

static esp_err_t httpd_admin_mqtt_get_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;
	nvs_config_mqtt_t cfg;
	if (nvs_config_get_mqtt(&cfg) != ESP_OK) {
		send_simple(req, "500 Internal Server Error", "nvs read failed");
		return ESP_OK;
	}
	char body[512];
	int n = snprintf(body, sizeof(body),
		"{\"enabled\":%s,\"broker\":\"%s\",\"user\":\"%s\",\"discovery_prefix\":\"%s\",\"has_password\":%s}",
		cfg.enabled ? "true" : "false",
		cfg.broker, cfg.user, cfg.discovery_prefix,
		cfg.password[0] ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, body, n);
	return ESP_OK;
}

static esp_err_t httpd_admin_mqtt_post_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;

	char body[HTTPD_FORM_BODY_MAX];
	if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_OK;

	nvs_config_mqtt_t cfg;
	nvs_config_get_mqtt(&cfg);  /* start with current to preserve fields not posted */

	char buf[NVS_CONFIG_MQTT_BROKER_MAX];
	if (httpd_query_key_value(body, "broker", buf, sizeof(buf)) == ESP_OK)
		strlcpy(cfg.broker, buf, sizeof(cfg.broker));
	if (httpd_query_key_value(body, "user", buf, sizeof(buf)) == ESP_OK)
		strlcpy(cfg.user, buf, sizeof(cfg.user));
	/* Password: only overwrite if a non-empty value is provided, otherwise keep current. */
	if (httpd_query_key_value(body, "password", buf, sizeof(buf)) == ESP_OK && buf[0])
		strlcpy(cfg.password, buf, sizeof(cfg.password));
	if (httpd_query_key_value(body, "discovery_prefix", buf, sizeof(buf)) == ESP_OK)
		strlcpy(cfg.discovery_prefix, buf, sizeof(cfg.discovery_prefix));
	char enabled_str[8] = { 0 };
	if (httpd_query_key_value(body, "enabled", enabled_str, sizeof(enabled_str)) == ESP_OK)
		cfg.enabled = (enabled_str[0] == '1' || enabled_str[0] == 't' || enabled_str[0] == 'o');

	if (nvs_config_set_mqtt(&cfg) != ESP_OK) {
		send_simple(req, "500 Internal Server Error", "nvs write failed");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"ok\":true,\"reboot_in_ms\":2000}", HTTPD_RESP_USE_STRLEN);
	schedule_reboot();
	return ESP_OK;
}

/* ----- init ------------------------------------------------------------- */

void httpd_init(void) {
	httpd_handle_t server = NULL;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.lru_purge_enable = true;
	config.max_uri_handlers = 20;

	ESP_LOGI(TAG, "starting server on port %d (mode: %s)",
	         config.server_port, wifi_is_softap_mode() ? "SoftAP" : "STA");
	if (httpd_start(&server, &config) != ESP_OK) {
		ESP_LOGE(TAG, "error starting server");
		return;
	}

	#define REG_URI(path, m, h)                                           \
		do {                                                              \
			static const httpd_uri_t u = { .uri = path, .method = m,      \
			                               .handler = h };                \
			httpd_register_uri_handler(server, &u);                       \
		} while (0)

	REG_URI("/", HTTP_GET, httpd_root_get_handler);
	REG_URI("/setup", HTTP_GET, httpd_setup_get_handler);
	REG_URI("/api/setup", HTTP_POST, httpd_setup_post_handler);
	REG_URI("/wakeup", HTTP_GET, httpd_wakeup_get_handler);
	REG_URI("/api/wakeup", HTTP_POST, httpd_api_wakeup_handler);
	REG_URI("/api/status", HTTP_GET, httpd_api_status_handler);
	REG_URI("/admin/wifi", HTTP_POST, httpd_admin_wifi_handler);
	REG_URI("/admin/passwords", HTTP_POST, httpd_admin_passwords_handler);
	REG_URI("/admin/hostname", HTTP_POST, httpd_admin_hostname_handler);
	REG_URI("/admin/reboot", HTTP_POST, httpd_admin_reboot_handler);
	REG_URI("/admin/mqtt", HTTP_POST, httpd_admin_mqtt_post_handler);
	REG_URI("/api/mqtt", HTTP_GET, httpd_admin_mqtt_get_handler);
	REG_URI("/admin/ota", HTTP_POST, ota_post_handler);

	/* Captive portal probe endpoints — redirect to / which dispatches to
	 * setup page (in SoftAP mode) or admin page (in STA mode). */
	REG_URI("/generate_204", HTTP_GET, httpd_captive_redirect_handler);
	REG_URI("/gen_204", HTTP_GET, httpd_captive_redirect_handler);
	REG_URI("/hotspot-detect.html", HTTP_GET, httpd_captive_redirect_handler);
	REG_URI("/library/test/success.html", HTTP_GET, httpd_captive_redirect_handler);
	REG_URI("/ncsi.txt", HTTP_GET, httpd_captive_redirect_handler);
	REG_URI("/connecttest.txt", HTTP_GET, httpd_captive_redirect_handler);
	REG_URI("/captive", HTTP_GET, httpd_captive_redirect_handler);

	#undef REG_URI

	ESP_LOGI(TAG, "server started");
}
