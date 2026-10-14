// "Turtle": the on-board RGB LED of an ESP32-C5-DevKitC-1, set from a web page.
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
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "turtle";
extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");

static led_strip_handle_t strip;
static uint8_t rgb[3] = {0, 160, 255};
static int brightness = CONFIG_TURTLE_BRIGHTNESS;
static uint32_t led_changes, requests;

#define MAX_CLIENTS 16
static struct { uint32_t ip; int64_t last_us; } seen[MAX_CLIENTS];

// The page gives sRGB values (what a screen shows); the LED's light is proportional to the value it is given.
// A screen value of 48 is only ~3 % of full light, so convert (gamma 2.2) before scaling by the brightness.
static float gamma_lut[256];
static void init_gamma(void) {
    for (int i = 0; i < 256; i++) gamma_lut[i] = powf(i / 255.0f, 2.2f) * 255.0f;
}
static uint8_t to_led(uint8_t screen) { return (uint8_t)(gamma_lut[screen] * brightness / 100.0f + 0.5f); }

static void apply_led(void) {
    led_strip_set_pixel(strip, 0, to_led(rgb[0]), to_led(rgb[1]), to_led(rgb[2]));
    led_strip_refresh(strip);
}

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

static void stats_task(void *arg) {
    for (;;) {
        int64_t now = esp_timer_get_time();
        int n = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) n += seen[i].ip && now - seen[i].last_us < 120 * 1000000LL;
        closer_demo_set_clients(n);
        closer_demo_set_stat("led_changes", led_changes);
        closer_demo_set_stat("requests", requests);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static esp_err_t page_get(httpd_req_t *req) {
    touch(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

static esp_err_t led_get(httpd_req_t *req) {
    char out[160];
    snprintf(out, sizeof out, "{\"hex\":\"%02x%02x%02x\",\"brightness\":%d,\"fw\":\"%s\",\"host\":\"turtle\"}", rgb[0], rgb[1], rgb[2], brightness, closer_demo_version());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// "rrggbb" or "rrggbb,brightness"
static void set_from_text(const char *t) {
    unsigned int c;
    int b;
    if (strlen(t) >= 6 && sscanf(t, "%6x", &c) == 1) { rgb[0] = c >> 16; rgb[1] = c >> 8; rgb[2] = c; }
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
            if (sscanf(v, "%x", &c) == 1) { rgb[0] = c >> 16; rgb[1] = c >> 8; rgb[2] = c; }
        }
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
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
    };
    for (int i = 0; i < 4; i++) httpd_register_uri_handler(srv, &uris[i]);
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
    apply_led();
    web_start();
    xTaskCreate(stats_task, "stats", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "Turtle running, firmware %s", closer_demo_version());
}
