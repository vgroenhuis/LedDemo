// UDP tests of a CloSer lab demo, on the demo's own web server:
//   GET  /udp               the page (relative links: works on the board and through the lab demos site)
//   GET  /api/udp?from=N    the running or last test: counters and round trips from packet N on
//   POST /api/udp           {"action": "start", "mode": "echo" | "ntp", "host", "port", "count", "interval_ms", "size"}
//                           or {"action": "stop"}
// "echo" sends test packets to the lab demos server's UDP echo (labdemos/udp_echo.py, port 8131), "ntp" sends NTP
// requests (any NTP server answers: a check of the way out while the echo port isn't reachable).
// The board also answers the same test packets itself on UDP port 8131, so the server can measure from its side
// (POST /labdemos/api/udp/<demo>/start) when it can reach the board.
// Three URI handlers: leave room for them in httpd_config_t.max_uri_handlers.
//
//   closer_udp_register(server);
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOSER_UDP_PORT 8131

esp_err_t closer_udp_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
