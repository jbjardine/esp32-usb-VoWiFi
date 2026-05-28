#include "nvs_config.h"

#include <string.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <psa/crypto.h>
#include <mbedtls/constant_time.h>

static const char *TAG = "nvs_config";

#define NVS_NAMESPACE "ewk_cfg"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_HTTPD_PASS_HASH "httpd_hash"
#define KEY_ADMIN_PASS_HASH "admin_hash"
#define KEY_MQTT_ENABLED   "mqtt_en"
#define KEY_MQTT_BROKER    "mqtt_broker"
#define KEY_MQTT_USER      "mqtt_user"
#define KEY_MQTT_PASS      "mqtt_pass"
#define KEY_MQTT_PREFIX    "mqtt_pfx"
#define KEY_HOSTNAME       "hostname"

#define MQTT_DEFAULT_DISCOVERY_PREFIX "homeassistant"
#define DEFAULT_HOSTNAME              "esp-wakeup-keypress"

static bool check_password_against_key(const char *key, const char *plaintext);
static esp_err_t set_password_at_key(const char *key, const char *plaintext);

static void sha256_string(const char *plain, uint8_t out[NVS_CONFIG_SHA256_LEN]) {
	size_t hash_len = 0;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256,
	                                   (const uint8_t *)plain, strlen(plain),
	                                   out, NVS_CONFIG_SHA256_LEN, &hash_len);
	if (st != PSA_SUCCESS || hash_len != NVS_CONFIG_SHA256_LEN) {
		ESP_LOGE(TAG, "psa_hash_compute failed: %d", (int)st);
		memset(out, 0, NVS_CONFIG_SHA256_LEN);
	}
}

static esp_err_t ensure_str_default(nvs_handle_t h, const char *key, const char *def) {
	size_t len = 0;
	esp_err_t err = nvs_get_str(h, key, NULL, &len);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		ESP_LOGI(TAG, "seeding %s from Kconfig default", key);
		return nvs_set_str(h, key, def);
	}
	return err;
}

static esp_err_t ensure_hash_default(nvs_handle_t h, const char *key, const char *plain_default) {
	size_t len = NVS_CONFIG_SHA256_LEN;
	uint8_t buf[NVS_CONFIG_SHA256_LEN];
	esp_err_t err = nvs_get_blob(h, key, buf, &len);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		ESP_LOGI(TAG, "seeding %s (sha256 of Kconfig default)", key);
		uint8_t hash[NVS_CONFIG_SHA256_LEN];
		sha256_string(plain_default, hash);
		return nvs_set_blob(h, key, hash, sizeof(hash));
	}
	return err;
}

esp_err_t nvs_config_init(void) {
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_LOGW(TAG, "nvs needs erase, erasing...");
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	/* PSA crypto must be initialized before psa_hash_compute(). Idempotent
	 * after the first call (returns PSA_ERROR_ALREADY_EXISTS, which is fine). */
	psa_status_t ps = psa_crypto_init();
	if (ps != PSA_SUCCESS) {
		ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)ps);
		return ESP_FAIL;
	}

	nvs_handle_t h;
	err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
		return err;
	}

	ensure_str_default(h, KEY_WIFI_SSID, CONFIG_ESP_WAKEUP_KEYPRESS_WIFI_SSID);
	ensure_str_default(h, KEY_WIFI_PASS, CONFIG_ESP_WAKEUP_KEYPRESS_WIFI_PASSWORD);
	ensure_hash_default(h, KEY_HTTPD_PASS_HASH, CONFIG_ESP_WAKEUP_KEYPRESS_HTTPD_PASSWORD);
	ensure_hash_default(h, KEY_ADMIN_PASS_HASH, CONFIG_ESP_WAKEUP_KEYPRESS_HTTPD_PASSWORD);
	ensure_str_default(h, KEY_HOSTNAME, DEFAULT_HOSTNAME);

	nvs_commit(h);
	nvs_close(h);
	ESP_LOGI(TAG, "init done");
	return ESP_OK;
}

esp_err_t nvs_config_get_wifi(char *ssid, size_t ssid_size,
                              char *password, size_t password_size) {
	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
	if (err != ESP_OK) return err;

	size_t len = ssid_size;
	err = nvs_get_str(h, KEY_WIFI_SSID, ssid, &len);
	if (err != ESP_OK) goto out;
	len = password_size;
	err = nvs_get_str(h, KEY_WIFI_PASS, password, &len);
out:
	nvs_close(h);
	return err;
}

esp_err_t nvs_config_set_wifi(const char *ssid, const char *password) {
	if (!ssid || !password) return ESP_ERR_INVALID_ARG;
	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
	if (err != ESP_OK) return err;
	err = nvs_set_str(h, KEY_WIFI_SSID, ssid);
	if (err == ESP_OK) err = nvs_set_str(h, KEY_WIFI_PASS, password);
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	return err;
}

bool nvs_config_has_wifi(void) {
	char ssid[NVS_CONFIG_WIFI_SSID_MAX] = { 0 };
	char pass[NVS_CONFIG_WIFI_PASSWORD_MAX] = { 0 };
	if (nvs_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK)
		return false;
	return ssid[0] != '\0';
}

esp_err_t nvs_config_set_httpd_password(const char *plaintext) {
	return set_password_at_key(KEY_HTTPD_PASS_HASH, plaintext);
}

static bool check_password_against_key(const char *key, const char *plaintext) {
	if (!plaintext) return false;
	nvs_handle_t h;
	if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
		return false;
	uint8_t stored[NVS_CONFIG_SHA256_LEN];
	size_t len = sizeof(stored);
	esp_err_t err = nvs_get_blob(h, key, stored, &len);
	nvs_close(h);
	if (err != ESP_OK || len != NVS_CONFIG_SHA256_LEN)
		return false;
	uint8_t candidate[NVS_CONFIG_SHA256_LEN];
	sha256_string(plaintext, candidate);
	return mbedtls_ct_memcmp(candidate, stored, NVS_CONFIG_SHA256_LEN) == 0;
}

static esp_err_t set_password_at_key(const char *key, const char *plaintext) {
	if (!plaintext) return ESP_ERR_INVALID_ARG;
	uint8_t hash[NVS_CONFIG_SHA256_LEN];
	sha256_string(plaintext, hash);
	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
	if (err != ESP_OK) return err;
	err = nvs_set_blob(h, key, hash, sizeof(hash));
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	return err;
}

bool nvs_config_check_httpd_password(const char *plaintext) {
	return check_password_against_key(KEY_HTTPD_PASS_HASH, plaintext);
}

esp_err_t nvs_config_set_admin_password(const char *plaintext) {
	return set_password_at_key(KEY_ADMIN_PASS_HASH, plaintext);
}

bool nvs_config_check_admin_password(const char *plaintext) {
	return check_password_against_key(KEY_ADMIN_PASS_HASH, plaintext);
}

static esp_err_t get_str_or_default(nvs_handle_t h, const char *key,
                                    char *out, size_t out_size, const char *def) {
	size_t len = out_size;
	esp_err_t err = nvs_get_str(h, key, out, &len);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		strlcpy(out, def, out_size);
		return ESP_OK;
	}
	return err;
}

esp_err_t nvs_config_get_mqtt(nvs_config_mqtt_t *out) {
	if (!out) return ESP_ERR_INVALID_ARG;
	memset(out, 0, sizeof(*out));
	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		strlcpy(out->discovery_prefix, MQTT_DEFAULT_DISCOVERY_PREFIX, sizeof(out->discovery_prefix));
		return ESP_OK;
	}
	if (err != ESP_OK) return err;

	uint8_t enabled = 0;
	if (nvs_get_u8(h, KEY_MQTT_ENABLED, &enabled) == ESP_OK)
		out->enabled = (enabled != 0);

	get_str_or_default(h, KEY_MQTT_BROKER, out->broker, sizeof(out->broker), "");
	get_str_or_default(h, KEY_MQTT_USER, out->user, sizeof(out->user), "");
	get_str_or_default(h, KEY_MQTT_PASS, out->password, sizeof(out->password), "");
	get_str_or_default(h, KEY_MQTT_PREFIX, out->discovery_prefix, sizeof(out->discovery_prefix), MQTT_DEFAULT_DISCOVERY_PREFIX);
	nvs_close(h);
	return ESP_OK;
}

esp_err_t nvs_config_get_hostname(char *out, size_t size) {
	if (!out || size == 0) return ESP_ERR_INVALID_ARG;
	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
	if (err != ESP_OK) {
		strlcpy(out, DEFAULT_HOSTNAME, size);
		return ESP_OK;
	}
	size_t len = size;
	err = nvs_get_str(h, KEY_HOSTNAME, out, &len);
	nvs_close(h);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		strlcpy(out, DEFAULT_HOSTNAME, size);
		return ESP_OK;
	}
	return err;
}

esp_err_t nvs_config_set_hostname(const char *hostname) {
	if (!hostname || !hostname[0]) return ESP_ERR_INVALID_ARG;
	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
	if (err != ESP_OK) return err;
	err = nvs_set_str(h, KEY_HOSTNAME, hostname);
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	return err;
}

esp_err_t nvs_config_set_mqtt(const nvs_config_mqtt_t *in) {
	if (!in) return ESP_ERR_INVALID_ARG;
	nvs_handle_t h;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
	if (err != ESP_OK) return err;
	err = nvs_set_u8(h, KEY_MQTT_ENABLED, in->enabled ? 1 : 0);
	if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_BROKER, in->broker);
	if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_USER, in->user);
	if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_PASS, in->password);
	if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_PREFIX,
	                                     in->discovery_prefix[0] ? in->discovery_prefix : MQTT_DEFAULT_DISCOVERY_PREFIX);
	if (err == ESP_OK) err = nvs_commit(h);
	nvs_close(h);
	return err;
}
