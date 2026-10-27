// The Wi-Fi settings page (see closer_wifi_web.h).
#include "closer_wifi_web.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "closer_ble.h"
#include "closer_demo.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "closer_wifi";

extern const char wifi_page_start[] asm("_binary_wifi_html_start");
extern const char wifi_page_end[] asm("_binary_wifi_html_end");

#define MAX_APS 40

// JSON string contents (quotes and backslashes escaped, control characters dropped).
static void esc(const char *in, char *out, size_t len) {
    size_t k = 0;
    for (; *in && k + 2 < len; in++) {
        if ((unsigned char)*in < 0x20) continue;
        if (*in == '"' || *in == '\\') out[k++] = '\\';
        out[k++] = *in;
    }
    out[k] = 0;
}

static const char *band_key(closer_band_t b) { return b == CLOSER_BAND_2G ? "2g" : b == CLOSER_BAND_5G ? "5g" : "both"; }

static esp_err_t page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, wifi_page_start, wifi_page_end - wifi_page_start - 1);
}

static esp_err_t config_get(httpd_req_t *req) {
    const size_t len = 9000;
    char *out = malloc(len);
    wifi_ap_record_t *aps = malloc(MAX_APS * sizeof *aps);
    closer_net_t *nets = malloc(CLOSER_MAX_NETWORKS * sizeof *nets);
    if (!out || !aps || !nets) {
        free(out); free(aps); free(nets);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    closer_demo_status_t st;
    closer_demo_get_status(&st);
    char e1[80], e2[80], note[112];
    esc(st.ssid, e1, sizeof e1);
    esc(st.wifi, e2, sizeof e2);
    int n = snprintf(out, len, "{\"fw\":\"%s\",\"name\":\"%s\",", closer_demo_version(), closer_demo_name());
    n += snprintf(out + n, len - n, "\"connected\":%s,\"ssid\":\"%s\",\"bssid\":\"%s\",\"channel\":%d,\"rssi\":%d,\"ip\":\"%s\",\"wifi\":\"%s\",",
                     st.connected ? "true" : "false", e1, st.bssid, st.channel, st.rssi, st.ip, e2);
    int k = closer_demo_saved_networks(nets, CLOSER_MAX_NETWORKS);
    n += snprintf(out + n, len - n, "\"saved\":[");
    for (int i = 0; i < k; i++) {
        esc(nets[i].ssid, e1, sizeof e1);
        n += snprintf(out + n, len - n, "%s{\"ssid\":\"%s\",\"open\":%s}", i ? "," : "", e1, nets[i].open ? "true" : "false");
    }
    closer_roam_t r;
    closer_demo_get_roam(&r);
    closer_demo_wifi_note(note, sizeof note);
    esc(note, e2, sizeof e2);
    n += snprintf(out + n, len - n,
                  "],\"pinned\":\"%s\",\"band\":\"%s\",\"roam\":{\"enabled\":%s,\"period_s\":%d,\"weak_rssi\":%d,\"margin_db\":%d},"
                  "\"switching\":%s,\"note\":\"%s\",\"bluetooth\":{\"available\":%s,\"enabled\":%s,\"running\":%s,\"clients\":%d},",
                  closer_demo_pinned_bssid(), band_key(closer_demo_get_band()), r.enabled ? "true" : "false", r.period_s,
                  r.weak_rssi, r.margin_db, closer_demo_wifi_switching() ? "true" : "false", e2,
#if CONFIG_BT_NIMBLE_ENABLED
                  "true",
#else
                  "false",
#endif
                  closer_ble_enabled() ? "true" : "false", closer_ble_running() ? "true" : "false", closer_ble_connections());
    int64_t age;
    k = closer_demo_last_scan(aps, MAX_APS, &age);
    n += snprintf(out + n, len - n, "\"scan\":{\"busy\":%s,\"age_s\":%d,\"aps\":[", closer_demo_scan_busy() ? "true" : "false",
                  age < 0 ? -1 : (int)(age / 1000));
    for (int i = 0; i < k && n < (int)len - 200; i++) {
        const wifi_ap_record_t *a = &aps[i];
        esc((const char *)a->ssid, e1, sizeof e1);
        n += snprintf(out + n, len - n, "%s{\"ssid\":\"%s\",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"ch\":%d,\"open\":%s}",
                      i ? "," : "", e1, a->bssid[0], a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5], a->rssi,
                      a->primary, a->authmode == WIFI_AUTH_OPEN ? "true" : "false");
    }
    n += snprintf(out + n, len - n, "]}}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, n);
    free(out); free(aps); free(nets);
    return err;
}

static void restart_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1500));          // (the reply gets out first)
    esp_restart();
}

static const char *why(esp_err_t e) {
    switch (e) {
        case ESP_ERR_NOT_FOUND: return "That network isn't saved: give its password";
        case ESP_ERR_INVALID_STATE: return "Not now: another change is being tried, or it is the network in use";
        case ESP_ERR_INVALID_SIZE: return "A Wi-Fi password has 8 to 63 characters";
        case ESP_ERR_NO_MEM: return "Up to 8 saved networks: forget one first";
        case ESP_ERR_INVALID_ARG: return "Invalid value";
        default: return esp_err_to_name(e);
    }
}

static esp_err_t reply(httpd_req_t *req, esp_err_t e, const char *ok_text) {
    char out[200], m[160];
    esc(e == ESP_OK ? ok_text : why(e), m, sizeof m);
    snprintf(out, sizeof out, "{\"ok\":%s,\"message\":\"%s\"}", e == ESP_OK ? "true" : "false", m);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static const char *str(cJSON *j, const char *key) {
    cJSON *v = cJSON_GetObjectItem(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int num(cJSON *j, const char *key, int fallback) {
    cJSON *v = cJSON_GetObjectItem(j, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static esp_err_t config_post(httpd_req_t *req) {
    char body[640];
    if (req->content_len >= sizeof body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;
    cJSON *j = cJSON_Parse(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not JSON");
    const char *action = str(j, "action");
    const char *ssid = str(j, "ssid");
    esp_err_t e = ESP_ERR_INVALID_ARG;
    const char *ok = "Done";
    char msg[120];
    if (!action) {
    } else if (strcmp(action, "connect") == 0) {
        const char *pw = str(j, "password");
        e = ESP_OK;
        if (pw) e = closer_demo_wifi_save(ssid, pw);
        if (e == ESP_OK) e = closer_demo_wifi_connect(ssid, str(j, "bssid"), num(j, "channel", 0));
        snprintf(msg, sizeof msg, "Trying %s: if it doesn't work within a minute, the board goes back", ssid ? ssid : "");
        ok = msg;
    } else if (strcmp(action, "save") == 0) {
        e = closer_demo_wifi_save(ssid, str(j, "password") ? str(j, "password") : "");
        ok = "Saved";
    } else if (strcmp(action, "forget") == 0) {
        e = closer_demo_wifi_forget(ssid);
        ok = "Forgotten";
    } else if (strcmp(action, "band") == 0) {
        const char *b = str(j, "band");
        closer_band_t band = b && strcmp(b, "2g") == 0 ? CLOSER_BAND_2G : b && strcmp(b, "5g") == 0 ? CLOSER_BAND_5G : CLOSER_BAND_BOTH;
        e = closer_demo_set_band(band);
        ok = "Trying the new band: if it doesn't work within a minute, the board goes back";
    } else if (strcmp(action, "unpin") == 0) {
        e = closer_demo_unpin();
        ok = "Any access point again (roaming picks the best)";
    } else if (strcmp(action, "roam") == 0) {
        closer_roam_t r;
        closer_demo_get_roam(&r);
        cJSON *en = cJSON_GetObjectItem(j, "enabled");
        if (cJSON_IsBool(en)) r.enabled = cJSON_IsTrue(en);
        r.period_s = num(j, "period_s", r.period_s);
        r.weak_rssi = num(j, "weak_rssi", r.weak_rssi);
        r.margin_db = num(j, "margin_db", r.margin_db);
        e = closer_demo_set_roam(&r);
        ok = "Roaming settings saved";
    } else if (strcmp(action, "scan") == 0) {
        e = closer_demo_request_scan();
        ok = "Scanning (10 to 25 s)";
    } else if (strcmp(action, "bluetooth") == 0) {
        cJSON *en = cJSON_GetObjectItem(j, "enabled");
        e = cJSON_IsBool(en) ? closer_ble_set_enabled(cJSON_IsTrue(en)) : ESP_ERR_INVALID_ARG;
        if (e == ESP_OK) {
            ok = cJSON_IsTrue(en) ? "Bluetooth on: restarting" : "Bluetooth off: restarting";
            ESP_LOGI(TAG, "%s", ok);
            xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
        }
    }
    esp_err_t r = reply(req, e, ok);
    cJSON_Delete(j);
    return r;
}

esp_err_t closer_wifi_web_register(httpd_handle_t server) {
    const httpd_uri_t uris[] = {
        {.uri = "/wifi", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/wifi/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/wifi/config", .method = HTTP_POST, .handler = config_post},
    };
    for (int i = 0; i < 3; i++) {
        esp_err_t e = httpd_register_uri_handler(server, &uris[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
