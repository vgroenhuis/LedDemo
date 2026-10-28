#include "closer_demo.h"
#include "closer_ble.h"

#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"          // SHA-256 (mbedTLS 4 in ESP-IDF 6 has only the PSA API; 5.x has it too)
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "closer";

#define MAX_STATS 16
#define ROLLBACK_GRACE_S 120
#define WIFI_CONNECTED_BIT BIT0

typedef struct { char name[25]; float value; bool used; } stat_t;

static struct {
    char hostname[32];
    char ssid[33], password[65], base[128], key[96];
    char version[32];
    stat_t stats[MAX_STATS];
    int clients;
    char message[121];
    SemaphoreHandle_t lock;
    EventGroupHandle_t events;
    bool booted_reported;       // the "boot" event has reached the server
    bool pending_verify;        // running a freshly installed image that hasn't proved itself yet
    char failed_version[32];    // a version that was rolled back; not tried again
    int64_t retry_ota_us;       // after a failed download: not before this (uptime)
    int ota_tries;
    bool ota_busy;
    bool roam_pending;          // connecting to a chosen access point
    int roam_fail, roams;
    char wifi[48];              // what Wi-Fi is doing (closer_demo_status_t.wifi)
    int channel, attempts;
    closer_wifi_phase_t phase;
    bool ever_connected;
    int last_channel;       // of the last connection (a quick reconnect tries it first)
    int64_t down_since_us;      // when the connection was lost (or start-up); 0 while connected
    TaskHandle_t reporter;
    closer_band_t band;
    char pin_bssid[18];         // the chosen access point of the network in use ("" any)
    int pin_channel;
    bool switching;             // closer_demo_wifi_connect() & co are trying something
    bool unsaved;               // moved to another saved network by itself: store it once connected
    char switch_note[112];
    closer_roam_t roam;
} S = {.clients = -1, .wifi = "Starting Wi-Fi"};

// Saved networks (NVS namespace "closer_nets", blobs n0..n7); passwords never leave the board.
typedef struct { char ssid[33]; char pass[65]; } saved_net_t;
static saved_net_t nets[CLOSER_MAX_NETWORKS];
static int nets_n;

// ------------------------------------------------------------------ flash settings

static void get_setting(nvs_handle_t h, const char *key, char *out, size_t len, const char *fallback) {
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        strlcpy(out, fallback, len);
    }
}

static void load_nets(void) {
    nets_n = 0;
    nvs_handle_t h;
    if (nvs_open("closer_nets", NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < CLOSER_MAX_NETWORKS; i++) {
            char key[4] = {'n', '0' + i, 0};
            size_t n = sizeof nets[0];
            if (nvs_get_blob(h, key, &nets[nets_n], &n) == ESP_OK && n == sizeof nets[0] && nets[nets_n].ssid[0]) nets_n++;
        }
        nvs_close(h);
    }
}

static void store_nets(void) {
    nvs_handle_t h;
    if (nvs_open("closer_nets", NVS_READWRITE, &h) != ESP_OK) return;
    for (int i = 0; i < CLOSER_MAX_NETWORKS; i++) {
        char key[4] = {'n', '0' + i, 0};
        if (i < nets_n) nvs_set_blob(h, key, &nets[i], sizeof nets[i]);
        else nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

static int find_net(const char *ssid) {
    for (int i = 0; i < nets_n; i++)
        if (strcmp(nets[i].ssid, ssid) == 0) return i;
    return -1;
}

static void load_settings(void) {
#if CONFIG_CLOSER_WIFI_BAND_5G
    S.band = CLOSER_BAND_5G;
#elif CONFIG_CLOSER_WIFI_BAND_2G
    S.band = CLOSER_BAND_2G;
#endif
#ifdef CONFIG_CLOSER_ROAMING
    S.roam.enabled = true;
#endif
    S.roam = (closer_roam_t){.enabled = S.roam.enabled, .period_s = CONFIG_CLOSER_ROAM_PERIODIC_S,
                             .weak_rssi = CONFIG_CLOSER_ROAM_RSSI, .margin_db = CONFIG_CLOSER_ROAM_MARGIN};
    nvs_handle_t h;
    if (nvs_open("closer", NVS_READONLY, &h) == ESP_OK) {
        get_setting(h, "wifi_ssid", S.ssid, sizeof S.ssid, CONFIG_CLOSER_DEFAULT_SSID);
        get_setting(h, "wifi_pass", S.password, sizeof S.password, CONFIG_CLOSER_DEFAULT_PASSWORD);
        get_setting(h, "base_url", S.base, sizeof S.base, CONFIG_CLOSER_REPORT_URL);
        get_setting(h, "key", S.key, sizeof S.key, CONFIG_CLOSER_DEFAULT_KEY);
        get_setting(h, "failed_ver", S.failed_version, sizeof S.failed_version, "");
        get_setting(h, "pin_bssid", S.pin_bssid, sizeof S.pin_bssid, "");
        int32_t v;
        if (nvs_get_i32(h, "pin_ch", &v) == ESP_OK) S.pin_channel = v;
        if (nvs_get_i32(h, "band", &v) == ESP_OK && v >= CLOSER_BAND_BOTH && v <= CLOSER_BAND_5G) S.band = v;
        if (nvs_get_i32(h, "roam_on", &v) == ESP_OK) S.roam.enabled = v;
        if (nvs_get_i32(h, "roam_per", &v) == ESP_OK) S.roam.period_s = v;
        if (nvs_get_i32(h, "roam_rssi", &v) == ESP_OK) S.roam.weak_rssi = v;
        if (nvs_get_i32(h, "roam_margin", &v) == ESP_OK) S.roam.margin_db = v;
        nvs_close(h);
    } else {
        strlcpy(S.ssid, CONFIG_CLOSER_DEFAULT_SSID, sizeof S.ssid);
        strlcpy(S.password, CONFIG_CLOSER_DEFAULT_PASSWORD, sizeof S.password);
        strlcpy(S.base, CONFIG_CLOSER_REPORT_URL, sizeof S.base);
        strlcpy(S.key, CONFIG_CLOSER_DEFAULT_KEY, sizeof S.key);
    }
}

static void remember_failed_version(const char *version) {
    nvs_handle_t h;
    if (nvs_open("closer", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "failed_ver", version);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ------------------------------------------------------------------ public setters

void closer_demo_set_stat(const char *name, float value) {
    if (!S.lock) return;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    int free_slot = -1;
    for (int i = 0; i < MAX_STATS; i++) {
        if (S.stats[i].used && strcmp(S.stats[i].name, name) == 0) { S.stats[i].value = value; free_slot = -2; break; }
        if (!S.stats[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        strlcpy(S.stats[free_slot].name, name, sizeof S.stats[free_slot].name);
        S.stats[free_slot].value = value;
        S.stats[free_slot].used = true;
    }
    xSemaphoreGive(S.lock);
}

void closer_demo_set_clients(int n) { S.clients = n; }

void closer_demo_set_message(const char *text) {
    if (!S.lock) return;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    strlcpy(S.message, text ? text : "", sizeof S.message);
    xSemaphoreGive(S.lock);
}

void closer_demo_get_status(closer_demo_status_t *out) {
    memset(out, 0, sizeof *out);
    out->connected = closer_demo_wifi_connected();
    out->updating = S.ota_busy;
    if (out->connected) {
        esp_netif_ip_info_t ip;
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
            snprintf(out->ip, sizeof out->ip, IPSTR, IP2STR(&ip.ip));
            snprintf(out->gateway, sizeof out->gateway, IPSTR, IP2STR(&ip.gw));
        }
    }
    wifi_ap_record_t ap;
    if (S.channel && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
        snprintf(out->bssid, sizeof out->bssid, "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
    out->phase = S.phase;
    int64_t since = S.down_since_us;
    out->down_s = since ? (int)((esp_timer_get_time() - since) / 1000000) : 0;
    if (S.lock) {
        xSemaphoreTake(S.lock, portMAX_DELAY);
        strlcpy(out->note, S.message, sizeof out->note);
        strlcpy(out->wifi, S.wifi, sizeof out->wifi);
        xSemaphoreGive(S.lock);
    }
    strlcpy(out->ssid, S.ssid, sizeof out->ssid);
    out->channel = S.channel;
    out->attempts = S.attempts;
}

bool closer_demo_wifi_connected(void) { return S.events && (xEventGroupGetBits(S.events) & WIFI_CONNECTED_BIT); }
const char *closer_demo_version(void) { return S.version; }
const char *closer_demo_name(void) { return S.hostname; }

// Why the chip last started: sent with the "boot" report.
const char *closer_demo_reset_reason(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power_on";      // power applied (plugged in, or the supply went away and back)
        case ESP_RST_EXT: return "reset_pin";
        case ESP_RST_SW: return "software";           // esp_restart(): an update, or asked to
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";     // the supply voltage dipped
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default: return "unknown";
    }
}

// ------------------------------------------------------------------ Wi-Fi

// Back to "any access point of this network" (after a roam has been completed or has failed).
static void clear_target(void) {
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.bssid_set) {
        wc.sta.bssid_set = false;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    S.roam_pending = false;
}

static void set_band(closer_band_t b) {
    wifi_band_mode_t m = b == CLOSER_BAND_2G ? WIFI_BAND_MODE_2G_ONLY : b == CLOSER_BAND_5G ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_AUTO;
    if (esp_wifi_set_band_mode(m) != ESP_OK) ESP_LOGW(TAG, "could not set the Wi-Fi band");
}

static bool parse_bssid(const char *t, uint8_t out[6]) {
    unsigned int b[6];
    if (!t || sscanf(t, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = b[i];
    return true;
}

// The station settings for S.ssid / S.password / the chosen access point.
static void make_config(wifi_config_t *wc) {
    memset(wc, 0, sizeof *wc);
    strlcpy((char *)wc->sta.ssid, S.ssid, sizeof wc->sta.ssid);
    strlcpy((char *)wc->sta.password, S.password, sizeof wc->sta.password);
    wc->sta.threshold.authmode = S.password[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wc->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (S.pin_bssid[0] && parse_bssid(S.pin_bssid, wc->sta.bssid)) {
        wc->sta.bssid_set = true;
        wc->sta.channel = S.pin_channel;
    }
}

// Joins S.ssid & co afresh (from the Wi-Fi event task or another task).
static void reconnect_now(void) {
    wifi_config_t wc;
    make_config(&wc);
    S.roam_pending = false;
    S.last_channel = 0;
    S.attempts = 0;
    bool was = closer_demo_wifi_connected();
    xEventGroupClearBits(S.events, WIFI_CONNECTED_BIT);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_disconnect();           // the event handler connects again, with these settings
    if (!was) esp_wifi_connect();    // (no "disconnected" event while it wasn't connected)
}

static void save_active(void) {
    nvs_handle_t h;
    if (nvs_open("closer", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "wifi_ssid", S.ssid);
    nvs_set_str(h, "wifi_pass", S.password);
    nvs_set_str(h, "pin_bssid", S.pin_bssid);
    nvs_set_i32(h, "pin_ch", S.pin_channel);
    nvs_set_i32(h, "band", S.band);
    nvs_commit(h);
    nvs_close(h);
    S.unsaved = false;
}

static void set_wifi_state(const char *fmt, ...) {
    char text[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (S.lock) xSemaphoreTake(S.lock, portMAX_DELAY);
    strlcpy(S.wifi, text, sizeof S.wifi);
    if (S.lock) xSemaphoreGive(S.lock);
}

// Why a connection failed, in words (wifi_err_reason_t).
static const char *reason_text(int r) {
    switch (r) {
        case WIFI_REASON_NO_AP_FOUND: return "Network not found";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE: return "Wrong password?";
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        case WIFI_REASON_AUTH_EXPIRE: return "Access point refused";
        case WIFI_REASON_BEACON_TIMEOUT: return "Signal lost";
        case WIFI_REASON_ASSOC_LEAVE: return "Disconnected";
        case WIFI_REASON_CONNECTION_FAIL: return "Connection failed";
        default: return "Connection lost";
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *e = data;
        S.channel = S.last_channel = e->channel;
        S.phase = CLOSER_WIFI_GETTING_IP;
        set_wifi_state("Getting an address");
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // The band can only be set once Wi-Fi is started, so it is done here, right before the first connect.
        if (S.band != CLOSER_BAND_BOTH) set_band(S.band);
        S.attempts = 1;
        S.phase = CLOSER_WIFI_SEARCHING;
        S.down_since_us = esp_timer_get_time();
        set_wifi_state("Searching for %s", S.ssid);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        xEventGroupClearBits(S.events, WIFI_CONNECTED_BIT);
        S.channel = 0;
        S.attempts++;
        if (!S.down_since_us) S.down_since_us = esp_timer_get_time();
        bool auth = reason_text(e->reason)[0] == 'W';          // "Wrong password?"
        S.phase = auth ? CLOSER_WIFI_AUTH_FAILED
                : S.roam_pending ? CLOSER_WIFI_ROAMING
                : S.ever_connected ? CLOSER_WIFI_RECONNECTING : CLOSER_WIFI_SEARCHING;
        set_wifi_state("%s (try %d)", reason_text(e->reason), S.attempts);
        ESP_LOGW(TAG, "Wi-Fi: %s (reason %d)", reason_text(e->reason), e->reason);
        ESP_LOGW(TAG, "Wi-Fi lost, reconnecting");
        if (S.roam_pending && ++S.roam_fail >= 3) {        // the chosen access point doesn't take us: any will do
            ESP_LOGW(TAG, "Roaming target unreachable");
            clear_target();
        }
        if (!S.roam_pending && !S.switching && S.attempts >= 4 && S.pin_bssid[0]) {
            // The chosen access point isn't there: any of the network will do (the choice comes back after a restart).
            ESP_LOGW(TAG, "Chosen access point %s not found, taking any", S.pin_bssid);
            S.pin_bssid[0] = 0;
            wifi_config_t wc;
            make_config(&wc);
            esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
        if (!S.roam_pending && !S.switching && nets_n > 1 && S.attempts % 6 == 0) {
            // The network in use can't be joined (out of range?): the next saved one, in turn.
            int i = find_net(S.ssid);
            const saved_net_t *n = &nets[(i + 1) % nets_n];
            ESP_LOGW(TAG, "Trying saved network %s instead of %s", n->ssid, S.ssid);
            strlcpy(S.ssid, n->ssid, sizeof S.ssid);
            strlcpy(S.password, n->pass, sizeof S.password);
            S.pin_bssid[0] = 0;
            S.last_channel = 0;
            S.unsaved = true;
            wifi_config_t wc;
            make_config(&wc);
            esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
        if (!S.roam_pending && !S.switching) {
            // A working connection was lost: try its channel first (well under a second) before scanning all
            // channels again (about 20 s with Bluetooth on, as it then shares the radio).
            wifi_config_t wc;
            if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
                bool quick = S.ever_connected && S.last_channel && S.attempts <= 2;
                wc.sta.channel = quick ? S.last_channel : 0;
                wc.sta.scan_method = quick ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
                esp_wifi_set_config(WIFI_IF_STA, &wc);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();      // (the "try N" note stays up while it searches again)
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Got address " IPSTR, IP2STR(&e->ip_info.ip));
        S.attempts = 0;
        S.phase = CLOSER_WIFI_CONNECTED;
        if (S.ever_connected && S.reporter) xTaskNotifyGive(S.reporter);     // back again: report now (new address?)
        S.ever_connected = true;
        S.down_since_us = 0;
        set_wifi_state("Connected");
        xEventGroupSetBits(S.events, WIFI_CONNECTED_BIT);
        if (S.roam_pending) clear_target();
        if (S.unsaved && !S.switching) save_active();      // (moved to another saved network by itself)
    }
}

static void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, S.hostname);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wc;
    make_config(&wc);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);   // powered over USB: answer quickly
}

// ------------------------------------------------------------------ roaming

// The chip joins the first (or strongest at that moment) access point and stays there. In a mesh of access points
// with one network name that can be a poor one, and a moving demo drifts away from it. So: look around shortly after
// connecting, when the signal is weak, and now and then; move to another access point of the same network only
// if it is clearly stronger. What was seen is reported (stats alt_rssi, roams) so the page shows how well it works.
// Listening time per channel. While Bluetooth runs (closer_ble) the Wi-Fi driver refuses scans with their own active
// times ("Should use default active scan time parameter ... when Bluetooth is enabled"), so they get the driver's
// defaults then. Otherwise short visits, and on the 5 GHz radar channels (52-144, where the board may only listen)
// 120 ms instead of the driver's 360 ms: just over one beacon interval (102 ms), holding traffic up a third as long.
static wifi_scan_time_t scan_time(void) {
    if (closer_ble_running()) return (wifi_scan_time_t){0};
    return (wifi_scan_time_t){.active = {.min = 20, .max = CONFIG_CLOSER_ROAM_SCAN_CHANNEL_MS}, .passive = 120};
}
static volatile bool roam_hold;
static int roam_scans;
void closer_demo_roaming_hold(bool hold) { roam_hold = hold; }

// Returns true if it started moving to another access point.
static bool roam_scan(const wifi_ap_record_t *cur) {
    wifi_scan_config_t sc = {.ssid = (uint8_t *)S.ssid, .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                             .scan_time = scan_time(), .home_chan_dwell_time = CONFIG_CLOSER_ROAM_SCAN_DWELL_MS};
    closer_demo_set_stat("roam_scans", ++roam_scans);
    S.phase = CLOSER_WIFI_SCANNING;
    set_wifi_state("Looking for a better AP");
    esp_err_t scanned = esp_wifi_scan_start(&sc, true);
    if (S.phase == CLOSER_WIFI_SCANNING) S.phase = CLOSER_WIFI_CONNECTED;     // (unless the connection dropped meanwhile)
    set_wifi_state("Connected");
    if (scanned != ESP_OK) return false;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) n = 24;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof *recs);
    if (!recs) return false;
    esp_wifi_scan_get_ap_records(&n, recs);
    int best = -1;
    bool moved = false;
    for (int i = 0; i < n; i++) {
        if (memcmp(recs[i].bssid, cur->bssid, 6) == 0) continue;
        if (best < 0 || recs[i].rssi > recs[best].rssi) best = i;
    }
    closer_demo_set_stat("alt_rssi", best >= 0 ? recs[best].rssi : -100);       // strongest other access point seen
    if (best >= 0 && recs[best].rssi >= cur->rssi + S.roam.margin_db) {
        ESP_LOGI(TAG, "Roaming: %d dBm -> %d dBm (channel %d -> %d)", cur->rssi, recs[best].rssi, cur->primary, recs[best].primary);
        wifi_config_t wc;
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
            wc.sta.bssid_set = true;
            memcpy(wc.sta.bssid, recs[best].bssid, 6);
            wc.sta.channel = recs[best].primary;
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            S.roam_pending = true;
            S.roam_fail = 0;
            S.roams++;
            closer_demo_set_stat("roams", S.roams);
            S.phase = CLOSER_WIFI_ROAMING;
            set_wifi_state("Moving to a stronger AP");
            esp_wifi_disconnect();                    // the Wi-Fi event handler reconnects, to the chosen one
            moved = true;
        }
    }
    free(recs);
    return moved;
}

static SemaphoreHandle_t scan_lock;      // one scan at a time (roaming or survey)
#define SCAN_CACHE 40
static wifi_ap_record_t scan_cache[SCAN_CACHE];         // the last full scan, for the Wi-Fi settings page
static int scan_cache_n;
static int64_t scan_cache_us;
static volatile bool scan_busy;

esp_err_t closer_demo_scan_all(wifi_ap_record_t *out, uint16_t *n) {
    if (!closer_demo_wifi_connected() || !scan_lock || xSemaphoreTake(scan_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                             .scan_time = scan_time(), .home_chan_dwell_time = CONFIG_CLOSER_ROAM_SCAN_DWELL_MS};
    S.phase = CLOSER_WIFI_SCANNING;
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (S.phase == CLOSER_WIFI_SCANNING) S.phase = CLOSER_WIFI_CONNECTED;
    if (e == ESP_OK) e = esp_wifi_scan_get_ap_records(n, out);       // (sorted by signal; frees the rest)
    else *n = 0;
    if (e == ESP_OK) {
        int k = *n < SCAN_CACHE ? *n : SCAN_CACHE;
        xSemaphoreTake(S.lock, portMAX_DELAY);
        memcpy(scan_cache, out, k * sizeof *out);
        scan_cache_n = k;
        scan_cache_us = esp_timer_get_time();
        xSemaphoreGive(S.lock);
    }
    xSemaphoreGive(scan_lock);
    return e;
}

static void roam_task(void *arg) {
    xEventGroupWaitBits(S.events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(10000));
    int64_t last_scan = CONFIG_CLOSER_ROAM_INITIAL_SCAN ? 0 : esp_timer_get_time() / 1000000;
    int weak_wait = 60;      // seconds between scans while the signal is weak; doubles when a scan finds nothing better
    bool first = CONFIG_CLOSER_ROAM_INITIAL_SCAN;
    for (;;) {
        wifi_ap_record_t cur;
        // (not while switched off, while an access point is chosen, or while another network is being tried; nor while
        // someone is connected over Bluetooth: with Bluetooth on, a scan holds the one radio for seconds, and the
        // Bluetooth connection, there because the Wi-Fi is poor, would stall)
        if (S.roam.enabled && !S.pin_bssid[0] && !S.switching && !roam_hold && closer_ble_connections() == 0 &&
            closer_demo_wifi_connected() &&
            esp_wifi_sta_get_ap_info(&cur) == ESP_OK) {
            int64_t now = esp_timer_get_time() / 1000000;
            bool periodic = S.roam.period_s > 0 && now - last_scan >= S.roam.period_s;
            bool weak = cur.rssi < S.roam.weak_rssi && now - last_scan >= weak_wait;
            if (first && cur.rssi >= S.roam.weak_rssi + 10) first = false;      // a strong signal: no need to look around
            if ((first || periodic || weak) && xSemaphoreTake(scan_lock, pdMS_TO_TICKS(20000)) == pdTRUE) {
                bool moved = roam_scan(&cur);
                xSemaphoreGive(scan_lock);
                first = false;
                last_scan = esp_timer_get_time() / 1000000;
                if (moved) weak_wait = 60;
                else if (weak && weak_wait < CONFIG_CLOSER_ROAM_BACKOFF_MAX_S) weak_wait *= 2;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CLOSER_ROAM_CHECK_S * 1000));
    }
}

// ------------------------------------------------------------------ time

// TLS checks certificate dates, so the clock must be roughly right before the first report.
static void time_sync(void) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.cloudflare.com"));
    esp_netif_sntp_init(&cfg);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) != ESP_OK) {
        struct timeval now;
        gettimeofday(&now, NULL);
        if (now.tv_sec < CLOSER_BUILD_EPOCH) {            // better than 1970 for certificate dates
            struct timeval t = {.tv_sec = CLOSER_BUILD_EPOCH};
            settimeofday(&t, NULL);
            ESP_LOGW(TAG, "No NTP answer: clock set to the firmware's build time");
        }
    } else {
        ESP_LOGI(TAG, "Clock synchronised");
    }
}

// ------------------------------------------------------------------ server requests

typedef struct { char *buf; int len, cap; } rx_t;

static esp_err_t on_http(esp_http_client_event_t *e) {
    rx_t *rx = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && rx && !esp_http_client_is_chunked_response(e->client)) {
        int n = e->data_len;
        if (rx->len + n < rx->cap) { memcpy(rx->buf + rx->len, e->data, n); rx->len += n; rx->buf[rx->len] = 0; }
    } else if (e->event_id == HTTP_EVENT_ON_DATA && rx) {
        int n = e->data_len;
        if (rx->len + n < rx->cap) { memcpy(rx->buf + rx->len, e->data, n); rx->len += n; rx->buf[rx->len] = 0; }
    }
    return ESP_OK;
}

static void set_auth(esp_http_client_handle_t c) {
    char auth[128];
    snprintf(auth, sizeof auth, "Bearer %s", S.key);
    esp_http_client_set_header(c, "Authorization", auth);
}

static char *build_report(const char *event) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "event", event);
    if (strcmp(event, "boot") == 0) cJSON_AddStringToObject(j, "reset", closer_demo_reset_reason());
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        char s[16];
        snprintf(s, sizeof s, IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(j, "ip", s);
    }
    cJSON_AddStringToObject(j, "hostname", S.hostname);
    cJSON_AddStringToObject(j, "fw", S.version);
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char b[18];
        snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        cJSON_AddNumberToObject(j, "rssi", ap.rssi);
        cJSON_AddStringToObject(j, "ssid", (const char *)ap.ssid);
        cJSON_AddStringToObject(j, "bssid", b);
        cJSON_AddNumberToObject(j, "channel", ap.primary);
    }
    // Internal RAM (what Wi-Fi, Bluetooth and the tasks need); PSRAM, where a board has it, as the stat psram_free.
    cJSON_AddNumberToObject(j, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) closer_demo_set_stat("psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j, "interval", CONFIG_CLOSER_INTERVAL_S);
    if (S.clients >= 0) cJSON_AddNumberToObject(j, "clients", S.clients);
    xSemaphoreTake(S.lock, portMAX_DELAY);
    if (S.message[0]) cJSON_AddStringToObject(j, "message", S.message);
    cJSON *stats = cJSON_AddObjectToObject(j, "stats");
    for (int i = 0; i < MAX_STATS; i++) {
        if (S.stats[i].used) cJSON_AddNumberToObject(stats, S.stats[i].name, S.stats[i].value);
    }
    xSemaphoreGive(S.lock);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

// ------------------------------------------------------------------ over-the-air update

static void hex(const uint8_t *in, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", in[i]);
}

// Downloads the image, checks its SHA-256 against the manifest, installs it in the other slot and restarts.
static void ota_task(void *arg) {
    cJSON *u = arg;
    const char *version = cJSON_GetObjectItem(u, "version")->valuestring;
    const char *path = cJSON_GetObjectItem(u, "path")->valuestring;
    const char *want_sha = cJSON_GetObjectItem(u, "sha256")->valuestring;
    int size = cJSON_GetObjectItem(u, "size") ? cJSON_GetObjectItem(u, "size")->valueint : 0;
    char url[256];
    snprintf(url, sizeof url, "%s%s", S.base, path);
    ESP_LOGI(TAG, "Updating %s -> %s", S.version, version);
    closer_demo_set_message("installing an update");

    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota = 0;
    esp_http_client_config_t cfg = {.url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .buffer_size = 2048};
    bool ok = false;
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    bool sha_ok = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&sha, PSA_ALG_SHA_256) == PSA_SUCCESS;
    char *buf = malloc(4096);
    if (slot && buf && sha_ok) {
        // Where the Wi-Fi is poor a download can stall: it then continues where it broke off (HTTP Range), up to
        // 8 connections, instead of starting over.
        int total = 0, got = 0;
        bool begun = false, broken = false;
        for (int attempt = 0; attempt < 8 && !broken && (!begun || got < total); attempt++) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(3000));
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (!c) break;
            set_auth(c);
            char range[32];
            if (got) {
                snprintf(range, sizeof range, "bytes=%d-", got);
                esp_http_client_set_header(c, "Range", range);
            }
            if (esp_http_client_open(c, 0) == ESP_OK) {
                int len = esp_http_client_fetch_headers(c);
                int status = esp_http_client_get_status_code(c);
                if (!begun) {
                    if (status == 200 && len > 0 && (size == 0 || len == size) && esp_ota_begin(slot, len, &ota) == ESP_OK) {
                        total = len;
                        begun = true;
                    } else {
                        ESP_LOGE(TAG, "Download refused: status %d, length %d", status, len);
                        broken = true;
                    }
                } else if (status != 206 || len != total - got) {
                    ESP_LOGE(TAG, "Can't resume the download: status %d, length %d", status, len);
                    broken = true;
                }
                int n;
                while (!broken && (n = esp_http_client_read(c, buf, 4096)) > 0) {
                    if (psa_hash_update(&sha, (uint8_t *)buf, n) != PSA_SUCCESS || esp_ota_write(ota, buf, n) != ESP_OK) broken = true;
                    else got += n;
                }
            }
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            if (begun && !broken && got < total) ESP_LOGW(TAG, "Download broke off at %d of %d bytes; resuming", got, total);
        }
        ok = begun && !broken && got == total;
        uint8_t digest[32];
        char dh[65];
        size_t dlen = 0;
        if (psa_hash_finish(&sha, digest, sizeof digest, &dlen) != PSA_SUCCESS || dlen != 32) ok = false;
        hex(digest, 32, dh);
        if (ok && strcasecmp(dh, want_sha) != 0) { ESP_LOGE(TAG, "SHA-256 mismatch"); ok = false; }
        if (ok && esp_ota_end(ota) != ESP_OK) ok = false;
        else if (!ok && begun) esp_ota_abort(ota);
        if (ok && esp_ota_set_boot_partition(slot) != ESP_OK) ok = false;
    }
    psa_hash_abort(&sha);
    free(buf);
    if (ok) {
        ESP_LOGI(TAG, "Update installed, restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    // A download that broke off (Wi-Fi hiccup) is no reason to give up on the version: again in 5, 10, 20 ...
    // minutes (up to an hour). Only a version that was installed and then rolled back is never tried again.
    S.ota_tries++;
    int wait_min = S.ota_tries >= 4 ? 60 : 5 << (S.ota_tries - 1);
    S.retry_ota_us = esp_timer_get_time() + wait_min * 60 * 1000000LL;
    ESP_LOGE(TAG, "Update to %s failed; staying on %s, trying again in %d min", version, S.version, wait_min);
    char m[64];
    snprintf(m, sizeof m, "update to %s failed (download), again in %d min", version, wait_min);
    closer_demo_set_message(m);
    cJSON_Delete(u);
    S.ota_busy = false;
    vTaskDelete(NULL);
}

static void maybe_update(cJSON *reply) {
    cJSON *u = cJSON_GetObjectItem(reply, "update");
    if (!cJSON_IsObject(u) || S.ota_busy || S.pending_verify) return;
    if (roam_hold) return;          // a time-critical moment (a robot being driven): the next report offers it again
    cJSON *v = cJSON_GetObjectItem(u, "version"), *p = cJSON_GetObjectItem(u, "path"), *h = cJSON_GetObjectItem(u, "sha256");
    if (!cJSON_IsString(v) || !cJSON_IsString(p) || !cJSON_IsString(h)) return;
    if (strcmp(v->valuestring, S.version) == 0 || strcmp(v->valuestring, S.failed_version) == 0) return;
    if (S.retry_ota_us && esp_timer_get_time() < S.retry_ota_us) return;
    cJSON *copy = cJSON_Duplicate(u, true);
    S.ota_busy = true;
    if (xTaskCreate(ota_task, "ota", 8192, copy, 5, NULL) != pdPASS) { cJSON_Delete(copy); S.ota_busy = false; }
}

// ------------------------------------------------------------------ reporting

static bool report(const char *event) {
    char *body = build_report(event);
    char url[192];
    snprintf(url, sizeof url, "%s/api/heartbeat", S.base);
    char resp[768] = {0};
    rx_t rx = {.buf = resp, .cap = sizeof resp};
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_POST, .event_handler = on_http, .user_data = &rx,
                                    .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c && body) {
        set_auth(c);
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, strlen(body));
        esp_err_t err = esp_http_client_perform(c);
        int status = esp_http_client_get_status_code(c);
        if (err == ESP_OK && status == 200) {
            ok = true;
            cJSON *reply = cJSON_Parse(resp);
            if (reply) { maybe_update(reply); cJSON_Delete(reply); }
        } else {
            ESP_LOGW(TAG, "Report failed: %s, HTTP %d", esp_err_to_name(err), status);
        }
    }
    if (c) esp_http_client_cleanup(c);
    free(body);
    return ok;
}

int closer_demo_post(const char *path, const char *body, size_t len) {
    if (!closer_demo_wifi_connected()) return -1;
    char url[192];
    snprintf(url, sizeof url, "%s%s", S.base, path);
    char resp[256] = {0};
    rx_t rx = {.buf = resp, .cap = sizeof resp};
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_POST, .event_handler = on_http, .user_data = &rx,
                                    .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    set_auth(c);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, len);
    int status = esp_http_client_perform(c) == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    return status;
}

static void report_task(void *arg) {
    xEventGroupWaitBits(S.events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    time_sync();
    for (;;) {
        if (closer_demo_wifi_connected()) {
            bool ok = report(S.booted_reported ? "beat" : "boot");
            if (ok) S.booted_reported = true;
            if (ok && S.pending_verify) {                  // the new version works: keep it
                esp_ota_mark_app_valid_cancel_rollback();
                S.pending_verify = false;
                ESP_LOGI(TAG, "Firmware %s confirmed", S.version);
                if (S.failed_version[0]) {                 // an older failure is history now
                    remember_failed_version("");
                    S.failed_version[0] = 0;
                    closer_demo_set_message("");
                }
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(S.pending_verify ? 15000 : CONFIG_CLOSER_INTERVAL_S * 1000));
    }
}

static void watchdog_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(ROLLBACK_GRACE_S * 1000));
    if (S.pending_verify) {
        ESP_LOGE(TAG, "New firmware %s could not report; going back to the previous one", S.version);
        remember_failed_version(S.version);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------ start

esp_err_t closer_demo_start(const char *hostname) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    S.lock = xSemaphoreCreateMutex();
    scan_lock = xSemaphoreCreateMutex();
    S.events = xEventGroupCreate();
    strlcpy(S.version, esp_app_get_description()->version, sizeof S.version);
    load_settings();
    load_nets();
    if (S.ssid[0] && find_net(S.ssid) < 0 && nets_n < CLOSER_MAX_NETWORKS) {    // the provisioned network: saved too
        memmove(&nets[1], &nets[0], nets_n * sizeof nets[0]);
        strlcpy(nets[0].ssid, S.ssid, sizeof nets[0].ssid);
        strlcpy(nets[0].pass, S.password, sizeof nets[0].pass);
        nets_n++;
        store_nets();
    }
    // The name: given, or the demo id at the front of the device key ("turtle-2.<secret>"), so one firmware serves
    // several boards.
    if (hostname && hostname[0]) strlcpy(S.hostname, hostname, sizeof S.hostname);
    else {
        size_t n = strcspn(S.key, ".");
        if (n == 0 || n >= sizeof S.hostname) strlcpy(S.hostname, "closer-demo", sizeof S.hostname);
        else { memcpy(S.hostname, S.key, n); S.hostname[n] = 0; }
    }
    if (!S.ssid[0] || !S.key[0]) ESP_LOGE(TAG, "No Wi-Fi network or device key in flash: see tools/provision.py");

    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        S.pending_verify = true;
        xTaskCreate(watchdog_task, "rollback", 3072, NULL, 3, NULL);
    }
    if (S.failed_version[0] && strcmp(S.failed_version, S.version) != 0) {
        char m[64];
        snprintf(m, sizeof m, "update to %s failed, rolled back", S.failed_version);
        closer_demo_set_message(m);
    }
    ESP_LOGI(TAG, "Firmware %s%s", S.version, S.pending_verify ? " (new, unconfirmed)" : "");
    wifi_start();
    xTaskCreate(report_task, "report", 8192, NULL, 4, &S.reporter);
    xTaskCreate(roam_task, "roam", 4096, NULL, 3, NULL);      // (does nothing while roaming is switched off)
    return ESP_OK;
}

// ------------------------------------------------------------------ Wi-Fi settings (closer_demo.h)

#define SWITCH_WAIT_S 60        // a scan of all channels takes ~20 s with Bluetooth on; then joining, an address

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (S.lock) xSemaphoreTake(S.lock, portMAX_DELAY);
    vsnprintf(S.switch_note, sizeof S.switch_note, fmt, ap);
    if (S.lock) xSemaphoreGive(S.lock);
    va_end(ap);
    ESP_LOGI(TAG, "%s", S.switch_note);
}

typedef struct { char ssid[33], pass[65], bssid[18]; int channel; closer_band_t band; } choice_t;
static choice_t switch_to;

static void current_choice(choice_t *c) {
    strlcpy(c->ssid, S.ssid, sizeof c->ssid);
    strlcpy(c->pass, S.password, sizeof c->pass);
    strlcpy(c->bssid, S.pin_bssid, sizeof c->bssid);
    c->channel = S.pin_channel;
    c->band = S.band;
}

static void apply_choice(const choice_t *c) {
    strlcpy(S.ssid, c->ssid, sizeof S.ssid);
    strlcpy(S.password, c->pass, sizeof S.password);
    strlcpy(S.pin_bssid, c->bssid, sizeof S.pin_bssid);
    S.pin_channel = c->bssid[0] ? c->channel : 0;
    S.band = c->band;
    set_band(c->band);
    reconnect_now();
}

static const char *band_name(closer_band_t b) { return b == CLOSER_BAND_2G ? "2.4 GHz" : b == CLOSER_BAND_5G ? "5 GHz" : "2.4 + 5 GHz"; }

static void describe(const choice_t *c, char *out, size_t len) {
    snprintf(out, len, "%s%s%s%s%s", c->ssid, c->bssid[0] ? " via " : "", c->bssid, c->band != CLOSER_BAND_BOTH ? ", " : "",
             c->band != CLOSER_BAND_BOTH ? band_name(c->band) : "");
}

// Tries switch_to; keeps it once connected (stored in flash), else back to what worked before.
static void switch_task(void *arg) {
    choice_t prev, want = switch_to;
    current_choice(&prev);
    char what[96], before[96];
    describe(&want, what, sizeof what);
    describe(&prev, before, sizeof before);
    note("Trying %s ...", what);
    apply_choice(&want);
    vTaskDelay(pdMS_TO_TICKS(500));      // (the old connection's bit is cleared; wait for a new address)
    EventBits_t bits = xEventGroupWaitBits(S.events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(SWITCH_WAIT_S * 1000));
    if (bits & WIFI_CONNECTED_BIT) {
        save_active();
        note("Connected to %s", what);
    } else {
        char why[48];
        xSemaphoreTake(S.lock, portMAX_DELAY);
        strlcpy(why, S.wifi, sizeof why);
        xSemaphoreGive(S.lock);
        apply_choice(&prev);
        note("Could not connect to %s (%s); back on %s", what, why, before);
    }
    S.switching = false;
    vTaskDelete(NULL);
}

static esp_err_t start_switch(const choice_t *c) {
    if (S.switching) return ESP_ERR_INVALID_STATE;
    S.switching = true;
    switch_to = *c;
    if (xTaskCreate(switch_task, "wifi_switch", 4096, NULL, 4, NULL) != pdPASS) {
        S.switching = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int closer_demo_saved_networks(closer_net_t *out, int max) {
    int n = 0;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    for (int i = 0; i < nets_n && n < max; i++, n++) {
        strlcpy(out[n].ssid, nets[i].ssid, sizeof out[n].ssid);
        out[n].open = !nets[i].pass[0];
    }
    xSemaphoreGive(S.lock);
    return n;
}

esp_err_t closer_demo_wifi_save(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 64)) return ESP_ERR_INVALID_ARG;
    if (password && password[0] && strlen(password) < 8) return ESP_ERR_INVALID_SIZE;      // WPA2: 8 to 63 characters
    esp_err_t err = ESP_OK;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    int i = find_net(ssid);
    if (i < 0 && !password) err = ESP_ERR_NOT_FOUND;
    else if (i < 0 && nets_n >= CLOSER_MAX_NETWORKS) err = ESP_ERR_NO_MEM;
    else {
        if (i < 0) {
            i = nets_n++;
            strlcpy(nets[i].ssid, ssid, sizeof nets[i].ssid);
        }
        if (password) strlcpy(nets[i].pass, password, sizeof nets[i].pass);
        store_nets();
    }
    xSemaphoreGive(S.lock);
    return err;
}

esp_err_t closer_demo_wifi_forget(const char *ssid) {
    if (!ssid || strcmp(ssid, S.ssid) == 0) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    int i = find_net(ssid);
    if (i >= 0) {
        memmove(&nets[i], &nets[i + 1], (nets_n - i - 1) * sizeof nets[0]);
        nets_n--;
        store_nets();
    }
    xSemaphoreGive(S.lock);
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t closer_demo_wifi_connect(const char *ssid, const char *bssid, int channel) {
    choice_t c;
    current_choice(&c);
    xSemaphoreTake(S.lock, portMAX_DELAY);
    int i = find_net(ssid ? ssid : "");
    if (i >= 0) {
        strlcpy(c.ssid, nets[i].ssid, sizeof c.ssid);
        strlcpy(c.pass, nets[i].pass, sizeof c.pass);
    }
    xSemaphoreGive(S.lock);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    uint8_t b[6];
    if (bssid && bssid[0] && !parse_bssid(bssid, b)) return ESP_ERR_INVALID_ARG;
    strlcpy(c.bssid, bssid ? bssid : "", sizeof c.bssid);
    c.channel = channel;
    return start_switch(&c);
}

esp_err_t closer_demo_set_band(closer_band_t band) {
    if (band < CLOSER_BAND_BOTH || band > CLOSER_BAND_5G) return ESP_ERR_INVALID_ARG;
    choice_t c;
    current_choice(&c);
    c.band = band;
    if (band != CLOSER_BAND_BOTH && c.channel) {      // a chosen access point on the other band can't stay chosen
        bool five = c.channel > 14;
        if (five != (band == CLOSER_BAND_5G)) { c.bssid[0] = 0; c.channel = 0; }
    }
    return start_switch(&c);
}

closer_band_t closer_demo_get_band(void) { return S.band; }
const char *closer_demo_pinned_bssid(void) { return S.pin_bssid; }

esp_err_t closer_demo_unpin(void) {
    if (S.switching) return ESP_ERR_INVALID_STATE;
    S.pin_bssid[0] = 0;
    S.pin_channel = 0;
    wifi_config_t wc;
    make_config(&wc);
    esp_wifi_set_config(WIFI_IF_STA, &wc);       // (stays on the access point it is on; roaming may move it later)
    save_active();
    note("Any access point of %s", S.ssid);
    return ESP_OK;
}

void closer_demo_get_roam(closer_roam_t *out) { *out = S.roam; }

esp_err_t closer_demo_set_roam(const closer_roam_t *r) {
    if (r->period_s < 0 || r->period_s > 86400 || r->weak_rssi < -95 || r->weak_rssi > -30 || r->margin_db < 1 || r->margin_db > 40)
        return ESP_ERR_INVALID_ARG;
    S.roam = *r;
    nvs_handle_t h;
    if (nvs_open("closer", NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    nvs_set_i32(h, "roam_on", r->enabled);
    nvs_set_i32(h, "roam_per", r->period_s);
    nvs_set_i32(h, "roam_rssi", r->weak_rssi);
    nvs_set_i32(h, "roam_margin", r->margin_db);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

bool closer_demo_wifi_switching(void) { return S.switching; }

void closer_demo_wifi_note(char *out, size_t len) {
    xSemaphoreTake(S.lock, portMAX_DELAY);
    strlcpy(out, S.switch_note, len);
    xSemaphoreGive(S.lock);
}

int closer_demo_last_scan(wifi_ap_record_t *out, int max, int64_t *age_ms) {
    xSemaphoreTake(S.lock, portMAX_DELAY);
    int n = scan_cache_n < max ? scan_cache_n : max;
    memcpy(out, scan_cache, n * sizeof *out);
    *age_ms = scan_cache_us ? (esp_timer_get_time() - scan_cache_us) / 1000 : -1;
    xSemaphoreGive(S.lock);
    return n;
}

static void scan_task(void *arg) {
    wifi_ap_record_t *recs = malloc(SCAN_CACHE * sizeof *recs);
    uint16_t n = SCAN_CACHE;
    if (recs) closer_demo_scan_all(recs, &n);
    free(recs);
    scan_busy = false;
    vTaskDelete(NULL);
}

esp_err_t closer_demo_request_scan(void) {
    if (!closer_demo_wifi_connected()) return ESP_ERR_INVALID_STATE;
    if (scan_busy) return ESP_OK;
    scan_busy = true;
    if (xTaskCreate(scan_task, "wifi_scan", 3072, NULL, 2, NULL) != pdPASS) { scan_busy = false; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

bool closer_demo_scan_busy(void) { return scan_busy; }
