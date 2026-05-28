idf_component_register(SRCS "main.c" "usb.c" "wifi.c" "gpio.c" "httpd.c" "led.c" "led_strip_encoder.c" "nvs_config.c" "captive_portal.c" "auth.c" "mqtt.c" "ota.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_rmt esp_wifi nvs_flash esp_http_server esp_event mbedtls esp_netif app_update espressif__mdns
                    EMBED_FILES "../web/setup.html" "../web/admin.html")
