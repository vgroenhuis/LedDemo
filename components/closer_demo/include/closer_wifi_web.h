// The Wi-Fi settings page of a CloSer lab demo, on the demo's own web server:
//   GET  /wifi              the page (relative links: works on the board and through the lab demos site)
//   GET  /api/wifi/config   state: connection, saved networks, nearby access points, band, roaming, Bluetooth
//   POST /api/wifi/config   {"action": "connect" | "save" | "forget" | "band" | "unpin" | "roam" | "scan" | "bluetooth", ...}
// Three URI handlers: leave room for them in httpd_config_t.max_uri_handlers.
//
//   closer_wifi_web_register(server);
//
// Through the lab demos site, changes need the admin login ("admin_paths" in demo.json); on the board's own network
// anyone who can open the board's page can change them, as with the demo itself.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t closer_wifi_web_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
