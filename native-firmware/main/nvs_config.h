#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#define NVS_CONFIG_WIFI_SSID_MAX 33
#define NVS_CONFIG_WIFI_PASSWORD_MAX 65
#define NVS_CONFIG_HTTPD_PASSWORD_MAX 64
#define NVS_CONFIG_SHA256_LEN 32
#define NVS_CONFIG_HOSTNAME_MAX 33

#define NVS_CONFIG_MQTT_BROKER_MAX 128
#define NVS_CONFIG_MQTT_USER_MAX 33
#define NVS_CONFIG_MQTT_PASS_MAX 65
#define NVS_CONFIG_MQTT_PREFIX_MAX 33

typedef struct {
	bool enabled;
	char broker[NVS_CONFIG_MQTT_BROKER_MAX];          /* e.g. mqtt://192.168.1.10:1883 */
	char user[NVS_CONFIG_MQTT_USER_MAX];              /* empty for anonymous */
	char password[NVS_CONFIG_MQTT_PASS_MAX];          /* empty for anonymous */
	char discovery_prefix[NVS_CONFIG_MQTT_PREFIX_MAX];/* default "homeassistant" */
} nvs_config_mqtt_t;

/* Initialize the NVS config namespace and seed defaults from Kconfig
 * the first time the device boots with an empty store. Subsequent boots
 * keep whatever the device or the admin UI has written. Must be called
 * once at boot before any other nvs_config_* function and before wifi_init. */
esp_err_t nvs_config_init(void);

/* WiFi STA credentials (plaintext — required by esp_wifi_set_config). */
esp_err_t nvs_config_get_wifi(char *ssid, size_t ssid_size,
                              char *password, size_t password_size);
esp_err_t nvs_config_set_wifi(const char *ssid, const char *password);
bool nvs_config_has_wifi(void);

/* HTTP wakeup password: stored as a SHA-256 hash. */
esp_err_t nvs_config_set_httpd_password(const char *plaintext);
bool nvs_config_check_httpd_password(const char *plaintext);

/* Admin (web UI) password: stored as a SHA-256 hash. */
esp_err_t nvs_config_set_admin_password(const char *plaintext);
bool nvs_config_check_admin_password(const char *plaintext);

/* MQTT broker configuration (plaintext credentials, LAN-only assumption). */
esp_err_t nvs_config_get_mqtt(nvs_config_mqtt_t *out);
esp_err_t nvs_config_set_mqtt(const nvs_config_mqtt_t *in);

/* Hostname used for mDNS / DHCP client identifier. Defaults to
 * "esp-wakeup-keypress" the first time the device boots. */
esp_err_t nvs_config_get_hostname(char *out, size_t size);
esp_err_t nvs_config_set_hostname(const char *hostname);
