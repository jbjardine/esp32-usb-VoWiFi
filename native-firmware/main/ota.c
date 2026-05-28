#include "ota.h"

#include <inttypes.h>
#include <string.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "auth.h"

static const char *TAG = "ota";

#define ROLLBACK_VALIDATE_DELAY_S 30
#define OTA_CHUNK 4096

static void send_err(httpd_req_t *req, const char *status, const char *msg) {
	httpd_resp_set_status(req, status);
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static void delayed_reboot(void *arg) {
	vTaskDelay(pdMS_TO_TICKS(2000));
	esp_restart();
}

esp_err_t ota_post_handler(httpd_req_t *req) {
	if (!auth_require_admin(req)) return ESP_OK;

	if (req->content_len <= 0) {
		send_err(req, "400 Bad Request", "empty body");
		return ESP_OK;
	}

	const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
	if (!target) {
		send_err(req, "500 Internal Server Error", "no OTA partition");
		return ESP_OK;
	}
	if ((size_t)req->content_len > target->size) {
		send_err(req, "413 Payload Too Large", "firmware exceeds partition size");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "OTA: writing %d bytes to '%s' @0x%08" PRIx32,
	         req->content_len, target->label, target->address);

	esp_ota_handle_t handle = 0;
	esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
		send_err(req, "500 Internal Server Error", "ota begin failed");
		return ESP_OK;
	}

	char buf[OTA_CHUNK];
	int total = 0;
	while (total < req->content_len) {
		int n = httpd_req_recv(req, buf, sizeof(buf));
		if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
		if (n <= 0) {
			ESP_LOGE(TAG, "recv failed at %d/%d", total, req->content_len);
			esp_ota_abort(handle);
			send_err(req, "500 Internal Server Error", "recv failed");
			return ESP_OK;
		}
		err = esp_ota_write(handle, buf, n);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "esp_ota_write at %d: %s", total, esp_err_to_name(err));
			esp_ota_abort(handle);
			send_err(req, "500 Internal Server Error", "ota write failed");
			return ESP_OK;
		}
		total += n;
	}

	err = esp_ota_end(handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
		send_err(req, "400 Bad Request", "invalid image (esp_ota_end failed)");
		return ESP_OK;
	}

	err = esp_ota_set_boot_partition(target);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
		send_err(req, "500 Internal Server Error", "set boot partition failed");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "OTA success, rebooting into '%s' in 2s", target->label);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, "{\"ok\":true,\"reboot_in_ms\":2000}", HTTPD_RESP_USE_STRLEN);
	xTaskCreate(delayed_reboot, "ota_reboot", 2048, NULL, 5, NULL);
	return ESP_OK;
}

static void validation_task(void *arg) {
	vTaskDelay(pdMS_TO_TICKS(ROLLBACK_VALIDATE_DELAY_S * 1000));
	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_ota_img_states_t state;
	if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
	    state == ESP_OTA_IMG_PENDING_VERIFY) {
		ESP_LOGI(TAG, "marking running partition '%s' valid after %ds healthy uptime",
		         running->label, ROLLBACK_VALIDATE_DELAY_S);
		esp_ota_mark_app_valid_cancel_rollback();
	}
	vTaskDelete(NULL);
}

void ota_start_validation_task(void) {
	xTaskCreate(validation_task, "ota_validate", 2048, NULL, 3, NULL);
}
