#include "auth.h"
#include "nvs_config.h"

#include <string.h>
#include <esp_log.h>
#include <mbedtls/base64.h>

static const char *TAG = "auth";

#define AUTH_HEADER_MAX 256
#define AUTH_DECODED_MAX 192
#define AUTH_USERNAME "admin"

static void send_401(httpd_req_t *req) {
	httpd_resp_set_status(req, "401 Unauthorized");
	httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP Wakeup\"");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_send(req, "401 Unauthorized", HTTPD_RESP_USE_STRLEN);
}

bool auth_require_admin(httpd_req_t *req) {
	char header[AUTH_HEADER_MAX];
	esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header));
	if (err != ESP_OK) {
		ESP_LOGD(TAG, "no Authorization header");
		send_401(req);
		return false;
	}

	const char *prefix = "Basic ";
	if (strncmp(header, prefix, strlen(prefix)) != 0) {
		ESP_LOGW(TAG, "unsupported auth scheme");
		send_401(req);
		return false;
	}

	const char *b64 = header + strlen(prefix);
	unsigned char decoded[AUTH_DECODED_MAX + 1];
	size_t decoded_len = 0;
	int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
	                                (const unsigned char *)b64, strlen(b64));
	if (ret != 0) {
		ESP_LOGW(TAG, "base64 decode failed: -0x%x", -ret);
		send_401(req);
		return false;
	}
	decoded[decoded_len] = '\0';

	char *colon = strchr((char *)decoded, ':');
	if (!colon) {
		ESP_LOGW(TAG, "missing : in credentials");
		send_401(req);
		return false;
	}
	*colon = '\0';
	const char *user = (const char *)decoded;
	const char *pass = colon + 1;

	if (strcmp(user, AUTH_USERNAME) != 0) {
		ESP_LOGW(TAG, "unknown user");
		send_401(req);
		return false;
	}
	if (!nvs_config_check_admin_password(pass)) {
		ESP_LOGW(TAG, "wrong admin password");
		send_401(req);
		return false;
	}
	return true;
}
