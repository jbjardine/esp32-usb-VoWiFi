#pragma once

#include <esp_http_server.h>

/* Verify Basic Auth credentials against the admin password stored in NVS.
 * Expects "Authorization: Basic <base64(admin:password)>" header.
 * If absent or invalid, sends a 401 with WWW-Authenticate and returns false.
 * Returns true if credentials are valid; caller can then continue handling. */
bool auth_require_admin(httpd_req_t *req);
