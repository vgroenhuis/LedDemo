// "Turtle": the on-board RGB LED of an ESP32-C5-DevKitC-1, set from a web page: a fixed colour, or (mode "wifi")
// a Wi-Fi signal light, green-yellow-red for the connection quality and blink patterns while it isn't connected.
// Wi-Fi, reporting to the lab demos page and over-the-air updates come from the closer_demo component.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "closer_ble.h"
#include "closer_demo.h"
#include "closer_udp.h"
#include "closer_wifi_web.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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

// Pings during a scan (roaming or survey) are slow or lost because the radio is listening on other channels: they don't count.
static bool scanning_now(void) {
    closer_demo_status_t st = {0};
    closer_demo_get_status(&st);
    return st.phase == CLOSER_WIFI_SCANNING;
}

static void on_ping_ok(esp_ping_handle_t h, void *arg) {
    if (scanning_now()) return;
    uint32_t ms = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof ms);
    ping_add(true, ms);
}
static void on_ping_lost(esp_ping_handle_t h, void *arg) { if (!scanning_now()) ping_add(false, 0); }

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

// Phase as a word, with "offline" for a long time without a connection.
static const char *phase_name(const closer_demo_status_t *st) {
    if ((st->phase == CLOSER_WIFI_SEARCHING || st->phase == CLOSER_WIFI_RECONNECTING) && st->down_s >= OFFLINE_AFTER_S) return "offline";
    return PHASE_NAMES[st->phase];
}

// The live state as JSON (GET /api/wifi, and pushed on /ws twice a second).
static int wifi_json(char *out, size_t len) {
    closer_demo_status_t st = {0};
    closer_demo_get_status(&st);
    int n = snprintf(out, len, "{\"wifi\":1,\"up_ms\":%lld,\"phase\":\"%s\",\"text\":\"%s\",\"ssid\":\"%s\",\"bssid\":\"%s\","
                     "\"channel\":%d,\"rssi\":%d,\"target\":\"%s\",\"target_ip\":\"%s\",\"led\":\"%02x%02x%02x\"",
                     esp_timer_get_time() / 1000, phase_name(&st), st.wifi, st.ssid, st.bssid, st.channel, st.rssi,
                     TARGET_NAMES[W.target], W.target_ip, shown[0], shown[1], shown[2]);
    if (W.have_ping) n += snprintf(out + n, len - n, ",\"loss\":%.0f,\"rtt\":%.0f", W.loss * 100, W.rtt);
    if (W.quality >= 0) n += snprintf(out + n, len - n, ",\"quality\":%.0f", W.quality * 100);
    n += snprintf(out + n, len - n, "}");
    return n;
}

static esp_err_t wifi_get(httpd_req_t *req) {
    char out[512];
    wifi_json(out, sizeof out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

// ------------------------------------------------------------------ the last 10 minutes, one sample a second
// Kept on the board, so time without a connection is in it too: the page fetches it again after a reconnect.

#define HIST_LEN 600
typedef struct {
    int8_t rssi;        // dBm, 0 unknown
    uint8_t loss;       // %, 255 unknown
    uint16_t rtt;       // ms, 0xffff unknown
    uint8_t quality;    // %, 255 unknown
    uint8_t phase;      // index into HIST_PHASES
    uint8_t bssid[6];   // the access point, zeros when none
} sample_t;
static const char *HIST_PHASES[] = {"starting", "searching", "getting_ip", "connected", "scanning", "roaming",
                                    "reconnecting", "auth_failed", "offline"};
static sample_t hist[HIST_LEN];
static int hist_head, hist_count;
static int64_t hist_last_ms;          // uptime of the newest sample
static portMUX_TYPE hist_mux = portMUX_INITIALIZER_UNLOCKED;

static void hist_record(void) {
    closer_demo_status_t st = {0};
    closer_demo_get_status(&st);
    const char *ph = phase_name(&st);
    sample_t s = {.rssi = st.rssi, .loss = 255, .rtt = 0xffff, .quality = 255, .phase = 0};
    for (int i = 0; i < sizeof HIST_PHASES / sizeof HIST_PHASES[0]; i++) if (strcmp(ph, HIST_PHASES[i]) == 0) s.phase = i;
    if (st.connected && W.have_ping) { s.loss = (uint8_t)roundf(W.loss * 100); s.rtt = W.rtt > 65000 ? 65000 : (uint16_t)roundf(W.rtt); }
    if (st.connected && W.quality >= 0) s.quality = (uint8_t)roundf(W.quality * 100);
    wifi_ap_record_t ap;
    if (st.channel && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) memcpy(s.bssid, ap.bssid, 6);
    portENTER_CRITICAL(&hist_mux);
    hist[hist_head] = s;
    hist_head = (hist_head + 1) % HIST_LEN;
    if (hist_count < HIST_LEN) hist_count++;
    hist_last_ms = esp_timer_get_time() / 1000;
    portEXIT_CRITICAL(&hist_mux);
}

// {"up_ms": now, "last_ms": newest sample, "step_ms": 1000, "phases": [...], "s": [[rssi, loss, rtt, quality, phase, bssid], ...]}
// oldest first; null for unknown; bssid "" for the same as the sample before (it rarely changes).
static esp_err_t history_get(httpd_req_t *req) {
    sample_t *copy = malloc(sizeof hist);
    char *out = malloc(HIST_LEN * 56 + 400);     // (worst case: another access point every second)
    if (!copy || !out) { free(copy); free(out); return httpd_resp_send_500(req); }
    portENTER_CRITICAL(&hist_mux);
    int count = hist_count, start = (hist_head - hist_count + HIST_LEN) % HIST_LEN;
    int64_t last = hist_last_ms;
    for (int i = 0; i < count; i++) copy[i] = hist[(start + i) % HIST_LEN];
    portEXIT_CRITICAL(&hist_mux);
    int n = sprintf(out, "{\"up_ms\":%lld,\"last_ms\":%lld,\"step_ms\":1000,\"phases\":[", esp_timer_get_time() / 1000, last);
    for (int i = 0; i < sizeof HIST_PHASES / sizeof HIST_PHASES[0]; i++) n += sprintf(out + n, "%s\"%s\"", i ? "," : "", HIST_PHASES[i]);
    n += sprintf(out + n, "],\"s\":[");
    uint8_t prev[6] = {1};       // (no real BSSID: the first sample always has one)
    for (int i = 0; i < count; i++) {
        sample_t s = copy[i];
        char a[8] = "null", b[8] = "null", c[8] = "null", d[8] = "null";
        if (s.rssi) sprintf(a, "%d", s.rssi);
        if (s.loss != 255) sprintf(b, "%u", s.loss);
        if (s.rtt != 0xffff) sprintf(c, "%u", s.rtt);
        if (s.quality != 255) sprintf(d, "%u", s.quality);
        char e[24] = "";
        if (memcmp(s.bssid, prev, 6) != 0) {
            static const uint8_t zero[6];
            if (memcmp(s.bssid, zero, 6) != 0) sprintf(e, "%02x:%02x:%02x:%02x:%02x:%02x", s.bssid[0], s.bssid[1], s.bssid[2], s.bssid[3], s.bssid[4], s.bssid[5]);
            else strcpy(e, "-");
            memcpy(prev, s.bssid, 6);
        }
        n += sprintf(out + n, "%s[%s,%s,%s,%s,%u,\"%s\"]", i ? "," : "", a, b, c, d, s.phase, e);
    }
    n += sprintf(out + n, "]}");
    free(copy);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, out, n);
    free(out);
    return e;
}

// ------------------------------------------------------------------ access points around (a survey while someone looks)

#define SURVEY_MAX 60
#define SURVEY_GOOD_S 300           // between scans while the signal is good (-65 dBm or better)
#define SURVEY_WEAK_S 60
static wifi_ap_record_t survey[SURVEY_MAX];
static uint16_t survey_n;
static int64_t survey_ms;            // uptime of the last survey, 0 never
static uint32_t survey_seq;
static SemaphoreHandle_t survey_lock;
static bool stream_has_viewers(void);

// {"aps_seq": n, "age_ms": ..., "aps": [{"bssid","ssid","rssi","ch","cur"}, ...]}: strongest first.
static int aps_json(char *out, size_t len) {
    uint8_t cur[6] = {0};
    wifi_ap_record_t now;
    if (esp_wifi_sta_get_ap_info(&now) == ESP_OK) memcpy(cur, now.bssid, 6);
    xSemaphoreTake(survey_lock, portMAX_DELAY);
    int n = snprintf(out, len, "{\"aps_seq\":%lu,\"age_ms\":%lld,\"aps\":[", (unsigned long)survey_seq,
                     survey_ms ? esp_timer_get_time() / 1000 - survey_ms : -1LL);
    for (int i = 0; i < survey_n && n < (int)len - 160; i++) {
        const wifi_ap_record_t *a = &survey[i];
        char ssid[2 * 33 + 1];          // escaped for JSON (quotes, backslashes, control characters dropped)
        int k = 0;
        for (const char *s = (const char *)a->ssid; *s && k < (int)sizeof ssid - 2; s++) {
            if ((unsigned char)*s < 0x20) continue;
            if (*s == '"' || *s == '\\') ssid[k++] = '\\';
            ssid[k++] = *s;
        }
        ssid[k] = 0;
        n += snprintf(out + n, len - n, "%s{\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"cur\":%d}",
                      i ? "," : "", a->bssid[0], a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5], ssid, a->rssi,
                      a->primary, memcmp(a->bssid, cur, 6) == 0);
    }
    xSemaphoreGive(survey_lock);
    n += snprintf(out + n, len - n, "]}");
    return n;
}

static int64_t aps_asked_ms;         // when the access point list was last asked for (uptime)
static esp_err_t aps_get(httpd_req_t *req) {
    aps_asked_ms = esp_timer_get_time() / 1000;
    char *out = malloc(SURVEY_MAX * 150 + 100);
    if (!out) return httpd_resp_send_500(req);
    int n = aps_json(out, SURVEY_MAX * 150 + 100);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, out, n);
    free(out);
    return e;
}

// ------------------------------------------------------------------ survey sessions
// Started from the lab demos page (POST /api/survey?minutes=60&interval=15): for that long, a full scan every interval;
// each scan, with the board's own connection and what happened to it since the last one, is sent to the server
// (/api/survey-data), which keeps it and analyses the session. Scans that can't be sent wait on the board (up to 24).

#define SV_BACKLOG 24
#define SV_EVENTS 32
static struct {
    bool active;
    int64_t end_ms;            // uptime when it stops
    int interval_s;
    uint32_t session;          // its start (Unix time): the session's name on the server
    uint32_t end_unix;         // when it stops (Unix time), kept in flash with the rest: see sv_save
    uint32_t seq, sent, failed, dropped;
    const char *restart;       // the board restarted during the survey (why): told with the next scan
    char *backlog[SV_BACKLOG];
    int backlog_n;
} SV;
static struct { uint32_t t; uint8_t phase; uint8_t bssid[6]; uint8_t ch; int8_t rssi; } sv_ev[SV_EVENTS];
static int sv_ev_n;
static portMUX_TYPE sv_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_unix(void) { return (uint32_t)time(NULL); }

// Called twice a second: notes every change of phase or access point (what the board itself went through).
static void sv_watch(void) {
    static int last_phase = -1;
    static uint8_t last_bssid[6];
    if (!SV.active) { last_phase = -1; return; }
    closer_demo_status_t st = {0};
    closer_demo_get_status(&st);
    int ph = 0;
    const char *name = phase_name(&st);
    for (int i = 0; i < sizeof HIST_PHASES / sizeof HIST_PHASES[0]; i++) if (strcmp(name, HIST_PHASES[i]) == 0) ph = i;
    uint8_t b[6] = {0};
    wifi_ap_record_t ap;
    if (st.channel && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) memcpy(b, ap.bssid, 6);
    if (ph == CLOSER_WIFI_SCANNING) ph = CLOSER_WIFI_CONNECTED;     // (our own scans are not news)
    if (ph == last_phase && memcmp(b, last_bssid, 6) == 0) return;
    last_phase = ph;
    memcpy(last_bssid, b, 6);
    portENTER_CRITICAL(&sv_mux);
    if (sv_ev_n < SV_EVENTS) {
        sv_ev[sv_ev_n].t = now_unix();
        sv_ev[sv_ev_n].phase = ph;
        memcpy(sv_ev[sv_ev_n].bssid, b, 6);
        sv_ev[sv_ev_n].ch = st.channel;
        sv_ev[sv_ev_n].rssi = st.rssi;
        sv_ev_n++;
    }
    portEXIT_CRITICAL(&sv_mux);
}

static int bw_mhz(wifi_bandwidth_t b) {
    switch (b) { case WIFI_BW20: return 20; case WIFI_BW40: return 40; case WIFI_BW80: return 80;
                 case WIFI_BW160: return 160; case WIFI_BW80_BW80: return 8080; default: return 0; }
}
static const char *phy_name(const wifi_ap_record_t *a) {
    return a->phy_11ax ? "ax" : a->phy_11ac ? "ac" : a->phy_11n ? "n" : a->phy_11a ? "a" : a->phy_11g ? "g" : a->phy_11b ? "b" : "";
}

// {"session","seq","t","interval","end","self":{...},["restart":why,]"events":[[t,phase,bssid,ch,rssi]...],
//  "aps":[[bssid,ssid,rssi,ch,second,bw,center1,center2,phy,auth,bss_color],...]}
static char *sv_build(const wifi_ap_record_t *recs, int n) {
    size_t cap = 600 + n * 190 + SV_EVENTS * 64;
    char *o = malloc(cap);
    if (!o) return NULL;
    closer_demo_status_t st = {0};
    closer_demo_get_status(&st);
    int k = snprintf(o, cap, "{\"session\":%lu,\"seq\":%lu,\"t\":%lu,\"interval\":%d,\"end\":%lu,\"fw\":\"%s\","
                     "\"self\":{\"bssid\":\"%s\",\"ch\":%d,\"rssi\":%d,\"phase\":\"%s\"",
                     (unsigned long)SV.session, (unsigned long)SV.seq, (unsigned long)now_unix(), SV.interval_s,
                     (unsigned long)(now_unix() + (SV.end_ms - esp_timer_get_time() / 1000) / 1000), closer_demo_version(),
                     st.bssid, st.channel, st.rssi, phase_name(&st));
    if (W.have_ping) k += snprintf(o + k, cap - k, ",\"rtt\":%.0f,\"loss\":%.0f", W.rtt, W.loss * 100);
    k += snprintf(o + k, cap - k, "}");
    if (SV.restart) {
        k += snprintf(o + k, cap - k, ",\"restart\":\"%s\"", SV.restart);
        SV.restart = NULL;
    }
    k += snprintf(o + k, cap - k, ",\"events\":[");
    portENTER_CRITICAL(&sv_mux);
    int ne = sv_ev_n;
    typeof(sv_ev[0]) ev[SV_EVENTS];
    memcpy(ev, sv_ev, ne * sizeof ev[0]);
    sv_ev_n = 0;
    portEXIT_CRITICAL(&sv_mux);
    for (int i = 0; i < ne; i++)
        k += snprintf(o + k, cap - k, "%s[%lu,\"%s\",\"%02x:%02x:%02x:%02x:%02x:%02x\",%d,%d]", i ? "," : "", (unsigned long)ev[i].t,
                      HIST_PHASES[ev[i].phase], ev[i].bssid[0], ev[i].bssid[1], ev[i].bssid[2], ev[i].bssid[3], ev[i].bssid[4],
                      ev[i].bssid[5], ev[i].ch, ev[i].rssi);
    k += snprintf(o + k, cap - k, "],\"aps\":[");
    for (int i = 0; i < n && k < (int)cap - 200; i++) {
        const wifi_ap_record_t *a = &recs[i];
        char ssid[2 * 33 + 1];
        int m = 0;
        for (const char *s = (const char *)a->ssid; *s && m < (int)sizeof ssid - 2; s++) {
            if ((unsigned char)*s < 0x20) continue;
            if (*s == '"' || *s == '\\') ssid[m++] = '\\';
            ssid[m++] = *s;
        }
        ssid[m] = 0;
        k += snprintf(o + k, cap - k, "%s[\"%02x:%02x:%02x:%02x:%02x:%02x\",\"%s\",%d,%d,%d,%d,%d,%d,\"%s\",%d,%d]", i ? "," : "",
                      a->bssid[0], a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5], ssid, a->rssi, a->primary,
                      a->second, bw_mhz(a->bandwidth), a->vht_ch_freq1, a->vht_ch_freq2, phy_name(a), a->authmode,
                      a->phy_11ax ? a->he_ap.bss_color : -1);
    }
    snprintf(o + k, cap - k, "]}");
    return o;
}

static void sv_queue(char *json) {
    if (SV.backlog_n == SV_BACKLOG) {          // full: the oldest goes
        free(SV.backlog[0]);
        memmove(SV.backlog, SV.backlog + 1, (SV_BACKLOG - 1) * sizeof SV.backlog[0]);
        SV.backlog_n--;
        SV.dropped++;
    }
    SV.backlog[SV.backlog_n++] = json;
}

static void sv_flush(void) {
    while (SV.backlog_n && closer_demo_wifi_connected()) {
        int status = closer_demo_post("/api/survey-data", SV.backlog[0], strlen(SV.backlog[0]));
        if (status == 200 || status == 400 || status == 413) {      // (a scan the server won't take is not retried)
            if (status != 200) SV.failed++; else SV.sent++;
            free(SV.backlog[0]);
            memmove(SV.backlog, SV.backlog + 1, (SV_BACKLOG - 1) * sizeof SV.backlog[0]);
            SV.backlog_n--;
        } else {
            SV.failed++;
            break;
        }
    }
}

// A running survey is also kept in flash, so it carries on after a restart (a power dip, a crash).
typedef struct { uint32_t session, end_unix, seq, interval_s; } sv_saved_t;

static void sv_save(void) {
    nvs_handle_t h;
    if (nvs_open("turtle", NVS_READWRITE, &h) != ESP_OK) return;
    if (SV.active) {
        sv_saved_t v = {SV.session, SV.end_unix, SV.seq, SV.interval_s};
        nvs_set_blob(h, "survey", &v, sizeof v);
    } else {
        nvs_erase_key(h, "survey");
    }
    nvs_commit(h);
    nvs_close(h);
}

// After a restart: picks the saved survey up again. False while the clock isn't right yet (try again later).
static bool sv_resume(void) {
    sv_saved_t v;
    size_t len = sizeof v;
    nvs_handle_t h;
    if (nvs_open("turtle", NVS_READONLY, &h) != ESP_OK) return true;
    esp_err_t err = nvs_get_blob(h, "survey", &v, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof v) return true;
    uint32_t now = now_unix();
    if (now < v.session) return false;                 // (no NTP answer yet: the clock is still the build time)
    if (now >= v.end_unix || v.end_unix - v.session > 1441 * 60) {
        SV.active = false;
        sv_save();
        return true;
    }
    SV.session = v.session;
    SV.end_unix = v.end_unix;
    SV.seq = v.seq;
    SV.interval_s = v.interval_s;
    SV.sent = SV.failed = SV.dropped = 0;
    SV.end_ms = esp_timer_get_time() / 1000 + (int64_t)(v.end_unix - now) * 1000;
    SV.restart = closer_demo_reset_reason();
    SV.active = true;
    ESP_LOGI(TAG, "Survey %lu resumed after a restart (%s): %lu s left", (unsigned long)SV.session, SV.restart,
             (unsigned long)(v.end_unix - now));
    return true;
}

static void sv_start(int minutes, int interval_s) {
    SV.session = now_unix();
    SV.end_unix = SV.session + minutes * 60;
    SV.seq = SV.sent = SV.failed = SV.dropped = 0;
    SV.interval_s = interval_s;
    SV.end_ms = esp_timer_get_time() / 1000 + minutes * 60000LL;
    SV.restart = NULL;
    portENTER_CRITICAL(&sv_mux);
    sv_ev_n = 0;
    portEXIT_CRITICAL(&sv_mux);
    SV.active = true;
    sv_save();
    ESP_LOGI(TAG, "Survey %lu: %d min, a scan every %d s", (unsigned long)SV.session, minutes, interval_s);
}

static int sv_json(char *out, size_t len) {
    int64_t left = SV.active ? (SV.end_ms - esp_timer_get_time() / 1000) / 1000 : 0;
    return snprintf(out, len, "{\"active\":%s,\"session\":%lu,\"interval\":%d,\"left_s\":%lld,\"scans\":%lu,\"sent\":%lu,"
                    "\"failed\":%lu,\"dropped\":%lu,\"waiting\":%d}", SV.active ? "true" : "false", (unsigned long)SV.session,
                    SV.interval_s, left > 0 ? left : 0, (unsigned long)SV.seq, (unsigned long)SV.sent, (unsigned long)SV.failed,
                    (unsigned long)SV.dropped, SV.backlog_n);
}

// GET: the survey's state. POST ?minutes=60&interval=15 starts one (minutes 1-1440, interval 10-600 s); ?stop=1 ends it.
static esp_err_t survey_handler(httpd_req_t *req) {
    touch(req);
    if (req->method == HTTP_POST) {
        char q[96], v[16];
        if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
            if (httpd_query_key_value(q, "stop", v, sizeof v) == ESP_OK) {
                SV.active = false;
                sv_save();
            } else {
                int minutes = 60, interval = 15;
                if (httpd_query_key_value(q, "minutes", v, sizeof v) == ESP_OK) minutes = atoi(v);
                if (httpd_query_key_value(q, "interval", v, sizeof v) == ESP_OK) interval = atoi(v);
                if (minutes < 1 || minutes > 1440 || interval < 10 || interval > 600) {
                    httpd_resp_set_status(req, "400 Bad Request");
                    return httpd_resp_sendstr(req, "{\"error\":\"minutes 1-1440, interval 10-600\"}");
                }
                if (now_unix() < 1700000000) {
                    httpd_resp_set_status(req, "503 Service Unavailable");
                    return httpd_resp_sendstr(req, "{\"error\":\"the board's clock isn't set yet\"}");
                }
                sv_start(minutes, interval);
            }
        }
    }
    char out[256];
    sv_json(out, sizeof out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

// Scans while someone looks at the access point list (the stream, /api/wifi/aps in the last 2 minutes): every
// SURVEY_GOOD_S on a good signal, SURVEY_WEAK_S on a weak one; and at the survey's interval while a survey runs.
static void survey_task(void *arg) {
    static wifi_ap_record_t recs[SURVEY_MAX];
    int64_t last = -SURVEY_GOOD_S * 1000LL;
    bool resume = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time() / 1000;
        if (resume && closer_demo_wifi_connected()) resume = !sv_resume() && now < 180000;   // (give up on the clock after 3 min)
        if (SV.active && now >= SV.end_ms) {
            SV.active = false;
            sv_save();
            ESP_LOGI(TAG, "Survey %lu done: %lu scans", (unsigned long)SV.session, (unsigned long)SV.seq);
        }
        if (SV.backlog_n) sv_flush();
        // A full scan delays traffic for a few seconds. A board that stands still with a good signal has little to
        // learn from it: every 5 minutes then, every minute while the signal is weak (a survey session: its own pace).
        wifi_ap_record_t ap;
        bool good = esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.rssi >= -65;
        int every = SV.active ? SV.interval_s : good ? SURVEY_GOOD_S : SURVEY_WEAK_S;
        // Only while someone looks at the list (each full scan delays traffic now and then for a few seconds)
        bool wanted = SV.active || stream_has_viewers() || (aps_asked_ms && now - aps_asked_ms < 120000);
        if (now - last < every * 1000LL || !wanted || !closer_demo_wifi_connected()) continue;
        uint16_t n = SURVEY_MAX;
        if (closer_demo_scan_all(recs, &n) != ESP_OK) { last = now - (every - 5) * 1000LL; continue; }   // busy: in 5 s
        last = now;                                   // (the interval runs from start to start: a full scan takes ~10 s)
        xSemaphoreTake(survey_lock, portMAX_DELAY);
        memcpy(survey, recs, n * sizeof recs[0]);
        survey_n = n;
        survey_ms = esp_timer_get_time() / 1000;
        survey_seq++;
        xSemaphoreGive(survey_lock);
        if (SV.active) {
            SV.seq++;
            sv_save();
            char *j = sv_build(recs, n);
            if (j) sv_queue(j);
            sv_flush();
        }
    }
}

// ------------------------------------------------------------------ live stream on /ws
// A client that sends "stream" (the lab demos server's live relay does) gets the live state twice a second.

#define MAX_STREAMS 3
static httpd_handle_t server;
static int stream_fds[MAX_STREAMS] = {-1, -1, -1};
static portMUX_TYPE stream_mux = portMUX_INITIALIZER_UNLOCKED;

static void stream_add(int fd) {
    portENTER_CRITICAL(&stream_mux);
    bool known = false;
    for (int i = 0; i < MAX_STREAMS; i++) known |= stream_fds[i] == fd;
    for (int i = 0; i < MAX_STREAMS && !known; i++) if (stream_fds[i] < 0) { stream_fds[i] = fd; known = true; }
    portEXIT_CRITICAL(&stream_mux);
}

static bool stream_has_viewers(void) {
    bool any = false;
    for (int i = 0; i < MAX_STREAMS; i++) any |= stream_fds[i] >= 0;
    return any;
}

static void stream_send_text(const char *buf, int n);
static void stream_send(void) {
    static uint32_t sent_seq;
    char buf[512];
    int n = wifi_json(buf, sizeof buf);
    stream_send_text(buf, n);
    if (survey_seq != sent_seq && stream_has_viewers()) {     // a new survey: once, to whoever is watching
        sent_seq = survey_seq;
        char *aps = malloc(SURVEY_MAX * 150 + 100);
        if (aps) { stream_send_text(aps, aps_json(aps, SURVEY_MAX * 150 + 100)); free(aps); }
    }
}

static void stream_send_text(const char *buf, int n) {
    int fds[MAX_STREAMS], count = 0;
    portENTER_CRITICAL(&stream_mux);
    for (int i = 0; i < MAX_STREAMS; i++) if (stream_fds[i] >= 0) fds[count++] = stream_fds[i];
    portEXIT_CRITICAL(&stream_mux);
    if (!count || !server) return;
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)buf, .len = n};
    for (int k = 0; k < count; k++) {
        bool alive = httpd_ws_get_fd_info(server, fds[k]) == HTTPD_WS_CLIENT_WEBSOCKET &&
                     httpd_ws_send_frame_async(server, fds[k], &frame) == ESP_OK;
        if (!alive) {
            portENTER_CRITICAL(&stream_mux);
            for (int i = 0; i < MAX_STREAMS; i++) if (stream_fds[i] == fds[k]) stream_fds[i] = -1;
            portEXIT_CRITICAL(&stream_mux);
        }
    }
}

static void wifi_task(void *arg) {
    int64_t last_sample = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_sample >= 1000) { last_sample = now; hist_record(); }
        sv_watch();
        stream_send();
    }
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
    snprintf(out, sizeof out, "{\"hex\":\"%02x%02x%02x\",\"brightness\":%d,\"mode\":\"%s\",\"fw\":\"%s\",\"host\":\"%s\"}",
             rgb[0], rgb[1], rgb[2], brightness, wifi_mode ? "wifi" : "fixed", closer_demo_version(), closer_demo_name());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// What Bluetooth pages see (and get told about at once): kept short, it must fit in one notification.
static void publish_state(void) {
    char out[64];
    snprintf(out, sizeof out, "{\"hex\":\"%02x%02x%02x\",\"brightness\":%d,\"mode\":\"%s\"}", rgb[0], rgb[1], rgb[2],
             brightness, wifi_mode ? "wifi" : "fixed");
    closer_ble_set_state(out);
}

// 0 to 100 (0: the LED is off); -1 if the text isn't a number in that range.
static int parse_brightness(const char *t) {
    char *end;
    long b = strtol(t, &end, 10);
    return end != t && *end == 0 && b >= 0 && b <= 100 ? (int)b : -1;
}

// "rrggbb" or "rrggbb,brightness" (a colour: back to the fixed colour), ",brightness" (brightness only),
// "mode=wifi" or "mode=fixed". From the WebSocket and from Bluetooth.
static void set_from_text(const char *t) {
    unsigned int c;
    int b;
    if (strncmp(t, "mode=", 5) == 0) {
        wifi_mode = strcmp(t + 5, "wifi") == 0;
        apply_led();
        save_soon();
        publish_state();
        return;
    }
    if (strlen(t) >= 6 && sscanf(t, "%6x", &c) == 1) { rgb[0] = c >> 16; rgb[1] = c >> 8; rgb[2] = c; wifi_mode = false; }
    if (t[0] == ',' && (b = parse_brightness(t + 1)) >= 0) brightness = b;
    if (strlen(t) > 7 && t[6] == ',' && (b = parse_brightness(t + 7)) >= 0) brightness = b;
    apply_led();
    led_changes++;
    save_soon();
    publish_state();
}

// WebSocket: the page sends "rrggbb,brightness" for every slider step; no reply, no new connection, no flash write.
// "ping:<anything>" comes back as "pong:<anything>" (the Bluetooth page compares round trips with Bluetooth's).
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) { touch(req); return ESP_OK; }      // the handshake
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
    char buf[48];
    frame.payload = (uint8_t *)buf;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof buf - 1);
    if (err != ESP_OK) return err;
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        buf[frame.len] = 0;
        touch_client(req);
        if (strcmp(buf, "stream") == 0) stream_add(httpd_req_to_sockfd(req));
        else if (strncmp(buf, "ping:", 5) == 0) {
            buf[1] = 'o';
            return httpd_ws_send_frame(req, &frame);
        } else set_from_text(buf);
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
            int b = parse_brightness(v);
            if (b >= 0) brightness = b;
        }
    }
    apply_led();
    save_soon();
    led_changes++;
    publish_state();
    return httpd_resp_sendstr(req, "ok");
}

static void web_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 16;       // (15 in use: 9 here, 3 for the Wi-Fi settings page, 3 for the UDP test)
    cfg.send_wait_timeout = 2;       // a stalled viewer must not hold the others up for long
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    server = srv;
    httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/led", .method = HTTP_GET, .handler = led_get},
        {.uri = "/api/led", .method = HTTP_POST, .handler = led_post},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get},
        {.uri = "/api/wifi/history", .method = HTTP_GET, .handler = history_get},
        {.uri = "/api/wifi/aps", .method = HTTP_GET, .handler = aps_get},
        {.uri = "/api/survey", .method = HTTP_GET, .handler = survey_handler},
        {.uri = "/api/survey", .method = HTTP_POST, .handler = survey_handler},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
    };
    for (int i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);
    closer_wifi_web_register(srv);   // since 1.14.0: /wifi, networks, band, access point, roaming, Bluetooth on/off
    closer_udp_register(srv);        // since 1.15.0: /udp, round trips to the CloSer server (and the board's own echo)
}

void app_main(void) {
    closer_demo_start(NULL);            // (named after its key: turtle, turtle-2, ...) also initialises NVS
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
    publish_state();
    closer_ble_start("turtle", set_from_text);     // since 1.13.0: the LED from a phone or laptop nearby, over Bluetooth
    xTaskCreate(led_task, "led", 4096, NULL, 4, NULL);
    survey_lock = xSemaphoreCreateMutex();
    xTaskCreate(wifi_task, "wifi-live", 4096, NULL, 3, NULL);
    xTaskCreate(survey_task, "survey", 8192, NULL, 2, NULL);      // (it also uploads: TLS)
    xTaskCreate(stats_task, "stats", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "Turtle running, firmware %s", closer_demo_version());
}
