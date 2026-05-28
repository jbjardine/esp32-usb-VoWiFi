#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Initialize and start the MQTT client if it is enabled and a broker URL
 * is configured in NVS. Safe to call even if MQTT is disabled — it just
 * returns without doing anything. */
void mqtt_init(void);

/* Bump the wakeup counter and publish the new value (called from any path
 * that successfully fires a wakeup — HTTP, admin UI, MQTT command, GPIO). */
void mqtt_notify_wakeup(void);

bool mqtt_is_connected(void);
