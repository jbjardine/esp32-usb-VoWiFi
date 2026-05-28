#include "captive_portal.h"

#include <errno.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

static const char *TAG = "captive";
static bool s_running = false;

#define SOFTAP_GATEWAY_IP "192.168.4.1"

static void dns_task(void *arg) {
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
		s_running = false;
		vTaskDelete(NULL);
		return;
	}

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(53),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
		close(sock);
		s_running = false;
		vTaskDelete(NULL);
		return;
	}

	ESP_LOGI(TAG, "dns hijack listening on UDP/53, replying with " SOFTAP_GATEWAY_IP);
	s_running = true;

	uint8_t buf[512];
	struct in_addr gw_addr;
	inet_aton(SOFTAP_GATEWAY_IP, &gw_addr);

	while (1) {
		struct sockaddr_in src;
		socklen_t srclen = sizeof(src);
		int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
		if (n < 12) continue;  /* min DNS header */

		/* Flip the QR/AA/RA bits to make a valid response. */
		buf[2] = 0x81;  /* QR=1, OPCODE=0, AA=0, TC=0, RD=1 */
		buf[3] = 0x80;  /* RA=1, Z=0, RCODE=0 (no error) */
		buf[6] = 0x00;
		buf[7] = 0x01;  /* ANCOUNT = 1 */

		/* Make sure there's room for the answer (max 16 extra bytes). */
		if (n + 16 > (int)sizeof(buf)) continue;

		uint8_t *p = &buf[n];
		*p++ = 0xC0; *p++ = 0x0C;             /* pointer to query name at offset 12 */
		*p++ = 0x00; *p++ = 0x01;             /* TYPE = A */
		*p++ = 0x00; *p++ = 0x01;             /* CLASS = IN */
		*p++ = 0x00; *p++ = 0x00;
		*p++ = 0x00; *p++ = 0x3C;             /* TTL = 60s */
		*p++ = 0x00; *p++ = 0x04;             /* RDLENGTH = 4 */
		memcpy(p, &gw_addr.s_addr, 4);
		p += 4;

		int resp_len = p - buf;
		sendto(sock, buf, resp_len, 0, (struct sockaddr *)&src, srclen);
	}
}

void captive_portal_start(void) {
	if (s_running) {
		ESP_LOGW(TAG, "already running");
		return;
	}
	if (xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 5, NULL) != pdPASS)
		ESP_LOGE(TAG, "failed to create dns task");
}

bool captive_portal_is_running(void) {
	return s_running;
}
