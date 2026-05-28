#pragma once

#include <stdbool.h>

/* Start the DNS hijack task. Once running, any DNS A query received on
 * UDP/53 is answered with 192.168.4.1 (the SoftAP gateway). This causes
 * every client OS to think "this network has captive portal" and prompts
 * the user to open the captive portal page (which is /setup on us). */
void captive_portal_start(void);

/* True if the DNS hijack task is running. */
bool captive_portal_is_running(void);
