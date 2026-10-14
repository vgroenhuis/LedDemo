#include "closer_demo.h"

#include <ctype.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
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
#include "mbedtls/sha256.h"
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
    bool ota_busy;
    bool roam_pending;          // connecting to a chosen access point
    int roam_fail, roams;
} S = {.clients = -1};

// ------------------------------------------------------------------ flash settings

static void get_setting(nvs_handle_t h, const char *key, char *out, size_t len, const char *fallback) {
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        strlcpy(out, fallback, len);
    }
}

static void load_settings(void) {
    nvs_handle_t h;
    if (nvs_open("closer", NVS_READONLY, &h) == ESP_OK) {
        get_setting(h, "wifi_ssid", S.ssid, sizeof S.ssid, CONFIG_CLOSER_DEFAULT_SSID);
        get_setting(h, "wifi_pass", S.password, sizeof S.password, CONFIG_CLOSER_DEFAULT_PASSWORD);
        get_setting(h, "base_url", S.base, sizeof S.base, CONFIG_CLOSER_REPORT_URL);
        get_setting(h, "key", S.key, sizeof S.key, CONFIG_CLOSER_DEFAULT_KEY);
        get_setting(h, "failed_ver", S.failed_version, sizeof S.failed_version, "");
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

bool closer_demo_wifi_connected(void) { return S.events && (xEventGroupGetBits(S.events) & WIFI_CONNECTED_BIT); }
const char *closer_demo_version(void) { return S.version; }

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

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(S.events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi lost, reconnecting");
        if (S.roam_pending && ++S.roam_fail >= 3) {        // the chosen access point doesn't take us: any will do
            ESP_LOGW(TAG, "Roaming target unreachable");
            clear_target();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Got address " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(S.events, WIFI_CONNECTED_BIT);
        if (S.roam_pending) clear_target();
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
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, S.ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, S.password, sizeof wc.sta.password);
    wc.sta.threshold.authmode = S.password[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
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
static volatile bool roam_hold;
static int roam_scans;
void closer_demo_roaming_hold(bool hold) { roam_hold = hold; }

// Returns true if it started moving to another access point.
static bool roam_scan(const wifi_ap_record_t *cur) {
    wifi_scan_config_t sc = {.ssid = (uint8_t *)S.ssid, .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                             .scan_time = {.active = {.min = 20, .max = CONFIG_CLOSER_ROAM_SCAN_CHANNEL_MS}},
                             .home_chan_dwell_time = CONFIG_CLOSER_ROAM_SCAN_DWELL_MS};
    closer_demo_set_stat("roam_scans", ++roam_scans);
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
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
    if (best >= 0 && recs[best].rssi >= cur->rssi + CONFIG_CLOSER_ROAM_MARGIN) {
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
            esp_wifi_disconnect();                    // the Wi-Fi event handler reconnects, to the chosen one
            moved = true;
        }
    }
    free(recs);
    return moved;
}

static void roam_task(void *arg) {
    xEventGroupWaitBits(S.events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(10000));
    int64_t last_scan = CONFIG_CLOSER_ROAM_INITIAL_SCAN ? 0 : esp_timer_get_time() / 1000000;
    int weak_wait = 60;      // seconds between scans while the signal is weak; doubles when a scan finds nothing better
    bool first = CONFIG_CLOSER_ROAM_INITIAL_SCAN;
    for (;;) {
        wifi_ap_record_t cur;
        if (!roam_hold && closer_demo_wifi_connected() && esp_wifi_sta_get_ap_info(&cur) == ESP_OK) {
            int64_t now = esp_timer_get_time() / 1000000;
            bool periodic = CONFIG_CLOSER_ROAM_PERIODIC_S > 0 && now - last_scan >= CONFIG_CLOSER_ROAM_PERIODIC_S;
            bool weak = cur.rssi < CONFIG_CLOSER_ROAM_RSSI && now - last_scan >= weak_wait;
            if (first || periodic || weak) {
                bool moved = roam_scan(&cur);
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
    cJSON_AddNumberToObject(j, "heap_free", esp_get_free_heap_size());
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
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    char *buf = malloc(4096);
    if (c && slot && buf) {
        set_auth(c);
        if (esp_http_client_open(c, 0) == ESP_OK) {
            int total = esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            if (status == 200 && total > 0 && (size == 0 || total == size) && esp_ota_begin(slot, total, &ota) == ESP_OK) {
                int got = 0, n;
                ok = true;
                while ((n = esp_http_client_read(c, buf, 4096)) > 0) {
                    mbedtls_sha256_update(&sha, (uint8_t *)buf, n);
                    if (esp_ota_write(ota, buf, n) != ESP_OK) { ok = false; break; }
                    got += n;
                }
                ok = ok && got == total;
                uint8_t digest[32];
                char dh[65];
                mbedtls_sha256_finish(&sha, digest);
                hex(digest, 32, dh);
                if (ok && strcasecmp(dh, want_sha) != 0) { ESP_LOGE(TAG, "SHA-256 mismatch"); ok = false; }
                if (ok && esp_ota_end(ota) != ESP_OK) ok = false;
                else if (!ok) esp_ota_abort(ota);
                if (ok && esp_ota_set_boot_partition(slot) != ESP_OK) ok = false;
            } else {
                ESP_LOGE(TAG, "Download refused: status %d, length %d", status, total);
            }
        }
    }
    mbedtls_sha256_free(&sha);
    free(buf);
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    if (ok) {
        ESP_LOGI(TAG, "Update installed, restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "Update to %s failed; staying on %s", version, S.version);
    remember_failed_version(version);               // not retried until a different version is released
    strlcpy(S.failed_version, version, sizeof S.failed_version);
    closer_demo_set_message("update failed");
    cJSON_Delete(u);
    S.ota_busy = false;
    vTaskDelete(NULL);
}

static void maybe_update(cJSON *reply) {
    cJSON *u = cJSON_GetObjectItem(reply, "update");
    if (!cJSON_IsObject(u) || S.ota_busy || S.pending_verify) return;
    cJSON *v = cJSON_GetObjectItem(u, "version"), *p = cJSON_GetObjectItem(u, "path"), *h = cJSON_GetObjectItem(u, "sha256");
    if (!cJSON_IsString(v) || !cJSON_IsString(p) || !cJSON_IsString(h)) return;
    if (strcmp(v->valuestring, S.version) == 0 || strcmp(v->valuestring, S.failed_version) == 0) return;
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
            }
        }
        vTaskDelay(pdMS_TO_TICKS(S.pending_verify ? 15000 : CONFIG_CLOSER_INTERVAL_S * 1000));
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
    S.events = xEventGroupCreate();
    strlcpy(S.hostname, hostname, sizeof S.hostname);
    strlcpy(S.version, esp_app_get_description()->version, sizeof S.version);
    load_settings();
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
    xTaskCreate(report_task, "report", 8192, NULL, 4, NULL);
#if CONFIG_CLOSER_ROAMING
    xTaskCreate(roam_task, "roam", 4096, NULL, 3, NULL);
#endif
    return ESP_OK;
}
