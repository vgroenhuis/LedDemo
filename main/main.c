// "Turtle": the on-board RGB LED of an ESP32-C5-DevKitC-1, set from a web page: a fixed colour, or (mode "wifi")
// a Wi-Fi signal light, green-yellow-red for the connection quality and blink patterns while it isn't connected.
// Wi-Fi, reporting to the lab demos page and over-the-air updates come from the closer_demo component.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "closer_demo.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "turtle";
extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");

static led_strip_handle_t strip;
static uint8_t rgb[3] = {0, 160, 255};
static int brightness = CONFIG_TURTLE_BRIGHTNESS;
static uint32_t led_changes, requests;
static bool wifi_mode;                  // false: the fixed colour; true: the Wi-Fi signal light
static volatile bool led_dirty = true;  // the fixed colour changed (the LED task shows it)
static uint8_t shown[3];                // what the LED shows now, in screen values (for the page)

#define MAX_CLIENTS 16
static struct { uint32_t ip; int64_t last_us; } seen[MAX_CLIENTS];

// The page gives sRGB values (what a screen shows); the LED's light is proportional to the value it is given.
// A screen value of 48 is only ~3 % of full light, so convert (gamma 2.2) before scaling by the brightness.
static float gamma_lut[256];
static void init_gamma(void) {
    for (int i = 0; i < 256; i++) gamma_lut[i] = powf(i / 255.0f, 2.2f) * 255.0f;
}
static uint8_t to_led(uint8_t screen) { return (uint8_t)(gamma_lut[screen] * brightness / 100.0f + 0.5f); }

static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    shown[0] = r, shown[1] = g, shown[2] = b;
    led_strip_set_pixel(strip, 0, to_led(r), to_led(g), to_led(b));
    led_strip_refresh(strip);
}

// Only the LED task writes the LED; this asks it to show the fixed colour again.
static void apply_led(void) { led_dirty = true; }

// Writing flash takes tens of milliseconds and stalls the web server, so it waits until the slider rests.
static esp_timer_handle_t save_timer;
static void save_led(void);
static void save_soon(void) {
    esp_timer_stop(save_timer);
    esp_timer_start_once(save_timer, 1500 * 1000);
}
static void save_cb(void *arg) { save_led(); }

static void save_led(void) {
    nvs_handle_t h;
    if (nvs_open("turtle", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t v = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
        nvs_set_u32(h, "rgb", v);
        nvs_set_u8(h, "bright", brightness);
        nvs_set_u8(h, "mode", wifi_mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_led(void) {
    nvs_handle_t h;
    if (nvs_open("turtle", NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        uint8_t b;
        if (nvs_get_u32(h, "rgb", &v) == ESP_OK) { rgb[0] = v >> 16; rgb[1] = v >> 8; rgb[2] = v; }
        if (nvs_get_u8(h, "bright", &b) == ESP_OK) brightness = b;
        if (nvs_get_u8(h, "mode", &b) == ESP_OK) wifi_mode = b;
        nvs_close(h);
    }
}

// Distinct web clients in the last two minutes.
static void touch_client(httpd_req_t *req) {
    struct sockaddr_in6 addr;
    socklen_t len = sizeof addr;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) return;
    uint32_t ip = addr.sin6_family == AF_INET6 ? addr.sin6_addr.un.u32_addr[3] : ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    int64_t now = esp_timer_get_time(), oldest = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (seen[i].ip == ip) { seen[i].last_us = now; return; }
        if (seen[i].last_us < seen[oldest].last_us) oldest = i;
    }
    seen[oldest].ip = ip;
    seen[oldest].last_us = now;
}

static void touch(httpd_req_t *req) {
    requests++;
    touch_client(req);
}


// ------------------------------------------------------------------ Wi-Fi signal light
//
// Twice a second: the signal strength (RSSI) of the access point, and a ping to the router (the gateway; if that
// never answers, the lab demos server instead). Over the last 20 pings (10 s): packet loss and mean round trip.
// Each gives a quality from 1 (good) to 0 (bad); the worst of the three sets the colour, green - yellow - red.

#define PING_WINDOW 20
#define PING_INTERVAL_MS 500
#define PING_TIMEOUT_MS 1000
#define OFFLINE_AFTER_S 30          // "reconnecting" becomes "offline" after this long without a connection

// Quality 1 at the first value, 0 at the second, linear in between (yellow halfway).
#define RSSI_GOOD (-60)
#define RSSI_BAD (-82)
#define LOSS_GOOD 0.0f
#define LOSS_BAD 0.20f
#define RTT_GOOD 20.0f
#define RTT_BAD 200.0f

typedef enum { TARGET_GATEWAY, TARGET_SERVER, TARGET_NONE } target_t;
static const char *TARGET_NAMES[] = {"router", "server", "none"};

static portMUX_TYPE ping_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int8_t ok[PING_WINDOW];          // 1 reply, 0 lost, -1 empty
    uint16_t rtt[PING_WINDOW];
    int head;
    uint32_t sent, replies;          // for the current target
    bool verified;                   // the target has answered at least once
    int64_t lost_at_us;              // the last lost ping (a short dark blip on the LED)
} P;

static struct {                      // what the page shows (GET /api/wifi)
    target_t target;
    char target_ip[16];
    float rssi, loss, rtt, quality;
    bool have_rssi, have_ping;
} W = {.target = TARGET_GATEWAY, .quality = -1};

static void ping_reset(void) {
    portENTER_CRITICAL(&ping_mux);
    memset(P.ok, -1, sizeof P.ok);
    P.head = 0;
    P.sent = P.replies = 0;
    portEXIT_CRITICAL(&ping_mux);
}

static void ping_add(bool ok, uint32_t rtt) {
    portENTER_CRITICAL(&ping_mux);
    P.ok[P.head] = ok;
    P.rtt[P.head] = rtt > 65535 ? 65535 : rtt;
    P.head = (P.head + 1) % PING_WINDOW;
    P.sent++;
    if (ok) P.replies++, P.verified = true;
    else P.lost_at_us = esp_timer_get_time();
    portEXIT_CRITICAL(&ping_mux);
}

static void on_ping_ok(esp_ping_handle_t h, void *arg) {
    uint32_t ms = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof ms);
    ping_add(true, ms);
}
static void on_ping_lost(esp_ping_handle_t h, void *arg) { ping_add(false, 0); }

static esp_ping_handle_t ping;

static void ping_stop(void) {
    if (!ping) return;
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    ping = NULL;
}

static bool ping_start(const char *ip) {
    ping_stop();
    ip_addr_t addr;
    if (!ipaddr_aton(ip, &addr)) return false;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = addr;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PING_INTERVAL_MS;
    cfg.timeout_ms = PING_TIMEOUT_MS;
    cfg.task_prio = 3;
    esp_ping_callbacks_t cbs = {.on_ping_success = on_ping_ok, .on_ping_timeout = on_ping_lost};
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) { ping = NULL; return false; }
    esp_ping_start(ping);
    return true;
}

// The address of the lab demos server (from the report URL), "" if it can't be looked up.
static void server_ip(char *out, size_t len) {
    char host[96] = "";
    const char *p = strstr(CONFIG_CLOSER_REPORT_URL, "://");
    p = p ? p + 3 : CONFIG_CLOSER_REPORT_URL;
    size_t n = strcspn(p, ":/");
    if (n >= sizeof host) n = sizeof host - 1;
    memcpy(host, p, n);
    host[n] = 0;
    out[0] = 0;
    struct addrinfo hints = {.ai_family = AF_INET}, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
        inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, out, len);
        freeaddrinfo(res);
    }
}

static float ramp(float v, float good, float bad) {
    float q = (v - bad) / (good - bad);
    return q < 0 ? 0 : q > 1 ? 1 : q;
}

// Every 0.5 s: keeps a ping running to the right target and works out the quality.
static void wifi_watch(void) {
    static char gateway[16];
    closer_demo_status_t st = {0};
    closer_demo_get_status(&st);
    if (!st.connected) {
        if (ping) { ping_stop(); gateway[0] = 0; }
        W.have_rssi = W.have_ping = false;
        W.quality = -1;
        return;
    }
    // (Re)connected, or a new router: start pinging it (or the server, if the router turned out not to answer).
    if (strcmp(gateway, st.gateway) != 0 || !ping) {
        strlcpy(gateway, st.gateway, sizeof gateway);
        ping_reset();
        if (W.target == TARGET_SERVER) server_ip(W.target_ip, sizeof W.target_ip);
        else if (W.target == TARGET_GATEWAY) strlcpy(W.target_ip, gateway, sizeof W.target_ip);
        if (W.target != TARGET_NONE && !(W.target_ip[0] && ping_start(W.target_ip))) ping_stop();
    }
    // A target that has never answered after 10 tries probably doesn't answer pings at all: try the next one.
    if (!P.verified && P.sent >= 10 && P.replies == 0 && W.target != TARGET_NONE) {
        ESP_LOGW(TAG, "No ping replies from the %s (%s)", TARGET_NAMES[W.target], W.target_ip);
        W.target++;
        ping_stop();
        W.target_ip[0] = 0;
        gateway[0] = 0;          // start again next round
        return;
    }
    // RSSI, smoothed over ~2 s.
    if (st.rssi) {
        W.rssi = W.have_rssi ? W.rssi + 0.25f * (st.rssi - W.rssi) : st.rssi;
        W.have_rssi = true;
    }
    int n = 0, ok = 0;
    float sum = 0;
    portENTER_CRITICAL(&ping_mux);
    for (int i = 0; i < PING_WINDOW; i++) {
        if (P.ok[i] < 0) continue;
        n++;
        if (P.ok[i]) ok++, sum += P.rtt[i];
    }
    bool verified = P.verified;
    portEXIT_CRITICAL(&ping_mux);
    // Pings only count once the target has answered (until then a silent router would look like a dead link).
    W.have_ping = verified && n >= 3;
    if (W.have_ping) {
        W.loss = 1.0f - (float)ok / n;
        W.rtt = ok ? sum / ok : PING_TIMEOUT_MS;
    }
    float q = W.have_rssi ? ramp(W.rssi, RSSI_GOOD, RSSI_BAD) : 1;
    if (W.have_ping) {
        float ql = ramp(W.loss, LOSS_GOOD, LOSS_BAD), qr = ramp(W.rtt, RTT_GOOD, RTT_BAD);
        if (ql < q) q = ql;
        if (qr < q) q = qr;
    }
    W.quality = q;
}

// Quality 0..1 to red - yellow - green (screen values: full red, then full yellow, then full green).
static void quality_colour(float q, uint8_t out[3]) {
    if (q < 0.5f) out[0] = 255, out[1] = (uint8_t)(510 * q), out[2] = 0;
    else out[0] = (uint8_t)(510 * (1 - q)), out[1] = 255, out[2] = 0;
}

// The LED pattern for what Wi-Fi is doing; t in milliseconds.
static void wifi_pattern(int64_t t, uint8_t out[3]) {
    static float shown_q = -1;
    closer_demo_status_t st = {0};
    closer_demo_get_status(&st);
    const uint8_t BLUE[3] = {0, 60, 255}, CYAN[3] = {0, 220, 255}, ORANGE[3] = {255, 110, 0}, RED[3] = {255, 0, 0},
                  PURPLE[3] = {200, 0, 255}, WHITE[3] = {200, 200, 200}, OFF[3] = {0, 0, 0};
    const uint8_t *c = OFF;
    uint8_t q[3];
    switch (st.phase) {
    case CLOSER_WIFI_STARTING:
    case CLOSER_WIFI_SEARCHING: {           // blue, breathing slowly (2 s)
        if (st.down_s >= OFFLINE_AFTER_S) goto offline;
        float s = 0.5f - 0.5f * cosf(2 * (float)M_PI * (t % 2000) / 2000.0f);
        out[0] = BLUE[0] * s, out[1] = BLUE[1] * s, out[2] = BLUE[2] * s;
        return;
    }
    case CLOSER_WIFI_GETTING_IP:            // cyan, blinking fast (4 Hz)
        c = t % 250 < 125 ? CYAN : OFF;
        break;
    case CLOSER_WIFI_RECONNECTING:          // orange, blinking (2 Hz); after 30 s offline
        if (st.down_s >= OFFLINE_AFTER_S) goto offline;
        c = t % 500 < 250 ? ORANGE : OFF;
        break;
    case CLOSER_WIFI_AUTH_FAILED:           // purple double flash every 2 s
        c = (t % 2000 < 120 || (t % 2000 >= 300 && t % 2000 < 420)) ? PURPLE : OFF;
        break;
    case CLOSER_WIFI_ROAMING:               // white, blinking fast (4 Hz)
        c = t % 250 < 125 ? WHITE : OFF;
        break;
    case CLOSER_WIFI_SCANNING:              // the quality colour, flickering with white
    case CLOSER_WIFI_CONNECTED:
        if (W.quality < 0) { c = t % 250 < 125 ? CYAN : OFF; break; }        // (just connected)
        shown_q = shown_q < 0 ? W.quality : shown_q + 0.1f * (W.quality - shown_q);     // ~0.5 s fade
        quality_colour(shown_q, q);
        c = q;
        if (st.phase == CLOSER_WIFI_SCANNING && t % 200 < 60) c = WHITE;
        else if (esp_timer_get_time() - P.lost_at_us < 120 * 1000) c = OFF;   // a lost ping: a short dark blip
        break;
    }
    if (st.phase != CLOSER_WIFI_CONNECTED && st.phase != CLOSER_WIFI_SCANNING) shown_q = -1;
    memcpy(out, c, 3);
    return;
offline:                                    // red, one short flash every 1.5 s
    memcpy(out, t % 1500 < 150 ? RED : OFF, 3);
}

static void led_task(void *arg) {
    int64_t last_watch = 0;
    bool was_wifi = false;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now - last_watch >= PING_INTERVAL_MS * 1000) { last_watch = now; wifi_watch(); }
        if (wifi_mode) {
            uint8_t c[3];
            wifi_pattern(now / 1000, c);
            set_led(c[0], c[1], c[2]);
            was_wifi = true;
        } else if (led_dirty || was_wifi) {
            led_dirty = false;
            was_wifi = false;
            set_led(rgb[0], rgb[1], rgb[2]);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static const char *PHASE_NAMES[] = {"starting", "searching", "getting_ip", "connected", "scanning", "roaming",
                                    "reconnecting", "auth_failed"};

static esp_err_t wifi_get(httpd_req_t *req) {
    closer_demo_status_t st = {0};
    closer_demo_get_status(&st);
    const char *phase = PHASE_NAMES[st.phase];
    if ((st.phase == CLOSER_WIFI_SEARCHING || st.phase == CLOSER_WIFI_RECONNECTING) && st.down_s >= OFFLINE_AFTER_S) phase = "offline";
    char out[512];
    int n = snprintf(out, sizeof out, "{\"phase\":\"%s\",\"text\":\"%s\",\"ssid\":\"%s\",\"bssid\":\"%s\",\"channel\":%d,"
                     "\"rssi\":%d,\"target\":\"%s\",\"target_ip\":\"%s\",\"led\":\"%02x%02x%02x\"",
                     phase, st.wifi, st.ssid, st.bssid, st.channel, st.rssi, TARGET_NAMES[W.target], W.target_ip,
                     shown[0], shown[1], shown[2]);
    if (W.have_ping) n += snprintf(out + n, sizeof out - n, ",\"loss\":%.0f,\"rtt\":%.0f", W.loss * 100, W.rtt);
    if (W.quality >= 0) n += snprintf(out + n, sizeof out - n, ",\"quality\":%.0f", W.quality * 100);
    snprintf(out + n, sizeof out - n, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

static void stats_task(void *arg) {
    for (;;) {
        int64_t now = esp_timer_get_time();
        int n = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) n += seen[i].ip && now - seen[i].last_us < 120 * 1000000LL;
        closer_demo_set_clients(n);
        closer_demo_set_stat("led_changes", led_changes);
        closer_demo_set_stat("requests", requests);
        if (W.have_ping) { closer_demo_set_stat("ping_ms", roundf(W.rtt)); closer_demo_set_stat("ping_loss_pct", roundf(W.loss * 100)); }
        if (W.quality >= 0) closer_demo_set_stat("wifi_quality_pct", roundf(W.quality * 100));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ------------------------------------------------------------------ web page

static esp_err_t page_get(httpd_req_t *req) {
    touch(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

static esp_err_t led_get(httpd_req_t *req) {
    char out[192];
    snprintf(out, sizeof out, "{\"hex\":\"%02x%02x%02x\",\"brightness\":%d,\"mode\":\"%s\",\"fw\":\"%s\",\"host\":\"turtle\"}",
             rgb[0], rgb[1], rgb[2], brightness, wifi_mode ? "wifi" : "fixed", closer_demo_version());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// "rrggbb" or "rrggbb,brightness" (a colour: back to the fixed colour), ",brightness" (brightness only),
// "mode=wifi" or "mode=fixed".
static void set_from_text(const char *t) {
    unsigned int c;
    int b;
    if (strncmp(t, "mode=", 5) == 0) {
        wifi_mode = strcmp(t + 5, "wifi") == 0;
        apply_led();
        save_soon();
        return;
    }
    if (strlen(t) >= 6 && sscanf(t, "%6x", &c) == 1) { rgb[0] = c >> 16; rgb[1] = c >> 8; rgb[2] = c; wifi_mode = false; }
    if (t[0] == ',' && (b = atoi(t + 1)) >= 1 && b <= 100) brightness = b;
    if (strlen(t) > 7 && t[6] == ',' && (b = atoi(t + 7)) >= 1 && b <= 100) brightness = b;
    apply_led();
    led_changes++;
    save_soon();
}

// WebSocket: the page sends "rrggbb,brightness" for every slider step; no reply, no new connection, no flash write.
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) { touch(req); return ESP_OK; }      // the handshake
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
    char buf[32];
    frame.payload = (uint8_t *)buf;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof buf - 1);
    if (err != ESP_OK) return err;
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        buf[frame.len] = 0;
        touch_client(req);
        set_from_text(buf);
    }
    return ESP_OK;
}

static esp_err_t led_post(httpd_req_t *req) {
    touch(req);
    char q[64], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "hex", v, sizeof v) == ESP_OK && strlen(v) == 6) {
            unsigned int c;
            if (sscanf(v, "%x", &c) == 1) { rgb[0] = c >> 16; rgb[1] = c >> 8; rgb[2] = c; wifi_mode = false; }
        }
        if (httpd_query_key_value(q, "mode", v, sizeof v) == ESP_OK) wifi_mode = strcmp(v, "wifi") == 0;
        if (httpd_query_key_value(q, "brightness", v, sizeof v) == ESP_OK) {
            int b = atoi(v);
            if (b >= 1 && b <= 100) brightness = b;
        }
    }
    apply_led();
    save_soon();
    led_changes++;
    return httpd_resp_sendstr(req, "ok");
}

static void web_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/led", .method = HTTP_GET, .handler = led_get},
        {.uri = "/api/led", .method = HTTP_POST, .handler = led_post},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
    };
    for (int i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);
}

void app_main(void) {
    closer_demo_start("turtle");        // also initialises NVS
    led_strip_config_t sc = {.strip_gpio_num = CONFIG_TURTLE_LED_GPIO, .max_leds = 1, .led_model = LED_MODEL_WS2812,
                             .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB};
    led_strip_rmt_config_t rc = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&sc, &rc, &strip));
    init_gamma();
    const esp_timer_create_args_t ta = {.callback = save_cb, .name = "save-led"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &save_timer));
    load_led();
    for (int i = 0; i < 3; i++) {          // since 1.1.0: three white blinks at start-up
        led_strip_set_pixel(strip, 0, 40, 40, 40);
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(150));
        led_strip_clear(strip);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    web_start();
    xTaskCreate(led_task, "led", 4096, NULL, 4, NULL);
    xTaskCreate(stats_task, "stats", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "Turtle running, firmware %s", closer_demo_version());
}
