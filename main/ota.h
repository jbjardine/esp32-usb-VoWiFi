#pragma once

#include <esp_http_server.h>

/* HTTP POST handler that streams an incoming raw firmware binary into the
 * inactive OTA partition. On success, sets the new partition as boot target
 * and schedules a reboot. Requires admin Basic Auth. */
esp_err_t ota_post_handler(httpd_req_t *req);

/* Start the rollback-arming task. After ROLLBACK_VALIDATE_DELAY_S seconds
 * of healthy uptime, the current app is marked valid so subsequent boots
 * stay on it. If we crash before that, the bootloader auto-reverts to
 * the previously known-good partition. */
void ota_start_validation_task(void);
