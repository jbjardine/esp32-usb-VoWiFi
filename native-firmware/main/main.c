#include "usb.h"
#include "wifi.h"
#include "gpio.h"
#include "httpd.h"
#include "led.h"
#include "mqtt.h"
#include "ota.h"
#include "nvs_config.h"
#include "main.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void app_main(void) {
	esp_log_level_set("*", ESP_LOG_INFO);

	ESP_ERROR_CHECK(nvs_config_init());

	led_init();
	gpio_init();
	usb_init();
	wifi_init();
	httpd_init();
	mqtt_init();  /* No-op if disabled or in SoftAP mode (no broker reachable). */
	ota_start_validation_task();  /* After 30s healthy uptime, mark app valid (rollback armed). */

	vTaskSuspend(NULL);
}
