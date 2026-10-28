// UDP round-trip tests (see closer_udp.h).
#include "closer_udp.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "cJSON.h"
#include "closer_ble.h"
#include "closer_demo.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "closer_udp";

extern const char udp_page_start[] asm("_binary_udp_html_start");
extern const char udp_page_end[] asm("_binary_udp_html_end");

#define MAX_COUNT 5000
#define NONE 0xFFFF            // (round trips are kept in units of 0.1 ms, up to 6.5 s)
#define HEAD 24                // the test packet header, as in labdemos/udp_echo.py
#define ECHO_RATE 400          // packets per second the board's own echo answers

typedef enum { IDLE, RUNNING, DONE, FAILED } run_state_t;
static const char *STATE[] = {"idle", "running", "done", "error"};

static struct {
    SemaphoreHandle_t lock;
    run_state_t state;
    bool stop;
    char message[96];
    bool ntp;
    char host[64], ip[16];
    int port, count, interval_ms, size;
    uint16_t session;
    int sent, received, dup, reorder, send_err, max_seq;
    int64_t peer_rx;           // packets the echo received of this test (-1: unknown, NTP)
    int64_t t_start, t_end;
    int rssi_start, channel_start;
    bool bt;
    uint16_t *rtt;
} T = {.port = CLOSER_UDP_PORT, .count = 200, .interval_ms = 50, .size = 64, .peer_rx = -1, .max_seq = -1};

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void fail(const char *m) {
    xSemaphoreTake(T.lock, portMAX_DELAY);
    T.state = FAILED;
    snprintf(T.message, sizeof T.message, "%s", m);
    T.t_end = esp_timer_get_time();
    xSemaphoreGive(T.lock);
}

// One reply: its round trip goes in the list (under the lock).
static void got_reply(const uint8_t *b, int n, int64_t now) {
    uint32_t seq, rtt_us;
    int64_t rx = -1;
    if (T.ntp) {
        if (n < 48 || (b[0] & 7) != 4) return;
        seq = get32be(b + 24);                      // (our transmit time field comes back as the origin time)
        rtt_us = (uint32_t)now - get32be(b + 28);
    } else {
        uint16_t session;
        if (n < HEAD || memcmp(b, "CUDP", 4) || b[4] != 1 || b[5] != 2) return;
        memcpy(&session, b + 6, 2);
        if (session != T.session) return;
        seq = get32(b + 8);
        int64_t t;
        memcpy(&t, b + 12, 8);
        rtt_us = (uint32_t)(now - t);
        rx = get32(b + 20);
    }
    if (seq >= (uint32_t)T.sent) return;
    if (T.rtt[seq] != NONE) {
        T.dup++;
        return;
    }
    T.rtt[seq] = MIN(rtt_us / 100, NONE - 1);
    T.received++;
    if ((int)seq < T.max_seq) T.reorder++;
    else T.max_seq = seq;
    if (rx > T.peer_rx) T.peer_rx = rx;
}

static void run_task(void *arg) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    char port[8];
    snprintf(port, sizeof port, "%d", T.port);
    if (getaddrinfo(T.host, port, &hints, &res) != 0 || !res) {
        fail("Can't find that host name (DNS)");
        vTaskDelete(NULL);
    }
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0 || connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        if (s >= 0) close(s);
        freeaddrinfo(res);
        fail("No socket for UDP");
        vTaskDelete(NULL);
    }
    xSemaphoreTake(T.lock, portMAX_DELAY);
    inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, T.ip, sizeof T.ip);
    xSemaphoreGive(T.lock);
    freeaddrinfo(res);
    fcntl(s, F_SETFL, O_NONBLOCK);
    ESP_LOGI(TAG, "test to %s (%s) port %d: %d x %d bytes every %d ms", T.host, T.ip, T.port, T.count, T.size, T.interval_ms);

    uint8_t *buf = calloc(1, 1500);
    int64_t next = esp_timer_get_time(), last_send = next, refused = 0;
    for (int seq = 0;; seq++) {
        bool sending = seq < T.count && !T.stop;
        int64_t until = sending ? next : last_send + 1500000;       // (late replies: 1.5 s after the last packet)
        if (sending) {
            int len = T.size;
            memset(buf, 0, len);
            int64_t now = esp_timer_get_time();
            if (T.ntp) {
                buf[0] = 0x23;                                         // NTP version 4, client
                put32be(buf + 40, seq);
                put32be(buf + 44, (uint32_t)now);
            } else {
                memcpy(buf, "CUDP", 4);
                buf[4] = 1;
                buf[5] = 1;
                memcpy(buf + 6, &T.session, 2);
                put32(buf + 8, seq);
                memcpy(buf + 12, &now, 8);
            }
            xSemaphoreTake(T.lock, portMAX_DELAY);
            T.sent = seq + 1;                                          // (before sending: a reply may come at once)
            xSemaphoreGive(T.lock);
            if (send(s, buf, len, 0) != len) {
                if (errno == ECONNREFUSED) refused++;                  // (an ICMP "port unreachable" came back)
                xSemaphoreTake(T.lock, portMAX_DELAY);
                T.send_err++;
                xSemaphoreGive(T.lock);
            }
            last_send = now;
            next += T.interval_ms * 1000LL;
            if (now - next > 1000000) next = now;                     // (more than a second behind: don't burst)
        }
        for (;;) {
            int64_t now = esp_timer_get_time();
            if (now >= until) break;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            struct timeval tv = {.tv_sec = (until - now) / 1000000, .tv_usec = (until - now) % 1000000};
            if (select(s + 1, &rd, NULL, NULL, &tv) <= 0) continue;
            int n;
            while ((n = recv(s, buf, 1500, 0)) > 0) {
                int64_t t = esp_timer_get_time();
                xSemaphoreTake(T.lock, portMAX_DELAY);
                got_reply(buf, n, t);
                xSemaphoreGive(T.lock);
            }
            if (n < 0 && errno == ECONNREFUSED) refused++;
        }
        if (!sending) break;
    }
    close(s);
    free(buf);
    xSemaphoreTake(T.lock, portMAX_DELAY);
    T.state = DONE;
    T.t_end = esp_timer_get_time();
    if (T.received == 0)
        snprintf(T.message, sizeof T.message, "%s", refused ? "No replies: the server refused the port (nothing listens there)"
                                                            : "No replies: the packets or the replies don't get through");
    else snprintf(T.message, sizeof T.message, "%s", T.stop ? "Stopped" : "Done");
    ESP_LOGI(TAG, "test done: %d sent, %d back", T.sent, T.received);
    xSemaphoreGive(T.lock);
    vTaskDelete(NULL);
}

static esp_err_t start(cJSON *j, char *msg, size_t len) {
    cJSON *v;
    const char *mode = cJSON_IsString(v = cJSON_GetObjectItem(j, "mode")) ? v->valuestring : "echo";
    const char *host = cJSON_IsString(v = cJSON_GetObjectItem(j, "host")) ? v->valuestring : "";
    bool ntp = strcmp(mode, "ntp") == 0;
    int port = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "port")) ? v->valueint : ntp ? 123 : CLOSER_UDP_PORT;
    int count = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "count")) ? v->valueint : 100;
    int interval = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "interval_ms")) ? v->valueint : 50;
    int size = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "size")) ? v->valueint : 64;
    bool name_ok = host[0] && strlen(host) < sizeof T.host;
    for (const char *c = host; *c; c++)
        if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '-')) name_ok = false;
    if (!name_ok || port < 1 || port > 65535) {
        snprintf(msg, len, "Give a host name and a port");
        return ESP_ERR_INVALID_ARG;
    }
    if (ntp) {                 // NTP servers stop answering clients that ask too often
        count = MIN(MAX(count, 1), 10);
        interval = MAX(interval, 8000);
        size = 48;
    }
    count = MIN(MAX(count, 1), MAX_COUNT);
    interval = MIN(MAX(interval, 10), 10000);
    size = MIN(MAX(size, HEAD), 1400);
    uint16_t *rtt = malloc(count * sizeof *rtt);
    if (!rtt) {
        snprintf(msg, len, "Not enough memory for %d packets", count);
        return ESP_ERR_NO_MEM;
    }
    memset(rtt, 0xFF, count * sizeof *rtt);
    closer_demo_status_t st;
    closer_demo_get_status(&st);
    if (!st.connected) {
        free(rtt);
        snprintf(msg, len, "Not connected to Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(T.lock, portMAX_DELAY);
    if (T.state == RUNNING) {
        xSemaphoreGive(T.lock);
        free(rtt);
        snprintf(msg, len, "A test is running");
        return ESP_ERR_INVALID_STATE;
    }
    free(T.rtt);
    T.rtt = rtt;
    T.ntp = ntp;
    snprintf(T.host, sizeof T.host, "%s", host);
    T.ip[0] = 0;
    T.port = port;
    T.count = count;
    T.interval_ms = interval;
    T.size = size;
    T.session = esp_random();
    T.sent = T.received = T.dup = T.reorder = T.send_err = 0;
    T.max_seq = -1;
    T.peer_rx = ntp ? -1 : 0;
    T.stop = false;
    T.t_start = esp_timer_get_time();
    T.t_end = 0;
    T.rssi_start = st.rssi;
    T.channel_start = st.channel;
    T.bt = closer_ble_running();
    T.state = RUNNING;
    snprintf(T.message, sizeof T.message, "Running");
    xSemaphoreGive(T.lock);
    if (xTaskCreate(run_task, "udp-test", 4096, NULL, 6, NULL) != pdPASS) {
        fail("Can't start the test");
        snprintf(msg, len, "Can't start the test");
        return ESP_ERR_NO_MEM;
    }
    snprintf(msg, len, "Started: %d packets of %d bytes every %d ms", count, size, interval);
    return ESP_OK;
}

// The board's own echo: the lab demos server (or anything else) can measure round trips to the board.
static void echo_task(void *arg) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(CLOSER_UDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s < 0 || bind(s, (struct sockaddr *)&me, sizeof me) != 0) {
        ESP_LOGW(TAG, "no UDP echo on port %d", CLOSER_UDP_PORT);
        if (s >= 0) close(s);
        vTaskDelete(NULL);
    }
    uint8_t *buf = malloc(1472);
    struct { uint32_t addr; uint16_t port, session; uint32_t rx; } last = {0};
    double tokens = ECHO_RATE;
    int64_t t_last = esp_timer_get_time();
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, 1472, 0, (struct sockaddr *)&from, &fl);
        if (n < HEAD || memcmp(buf, "CUDP", 4) || buf[4] != 1 || buf[5] != 1) continue;
        int64_t now = esp_timer_get_time();
        tokens = MIN(ECHO_RATE, tokens + (now - t_last) * ECHO_RATE / 1e6);
        t_last = now;
        if (tokens < 1) continue;
        tokens -= 1;
        uint16_t session;
        memcpy(&session, buf + 6, 2);
        if (from.sin_addr.s_addr != last.addr || from.sin_port != last.port || session != last.session) {
            last.addr = from.sin_addr.s_addr;
            last.port = from.sin_port;
            last.session = session;
            last.rx = 0;
        }
        buf[5] = 2;
        put32(buf + 20, ++last.rx);
        sendto(s, buf, n, 0, (struct sockaddr *)&from, fl);       // (as long as the request: no amplification)
    }
}

static esp_err_t page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, udp_page_start, udp_page_end - udp_page_start - 1);
}

// Sent in pieces from the stack (no big buffer on the heap: a busy page mustn't break the heap up, updates need
// large blocks for TLS).
static esp_err_t status_get(httpd_req_t *req) {
    int from = 0;
    char q[32], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "from", v, sizeof v) == ESP_OK)
        from = MAX(atoi(v), 0);
    char out[720];
    xSemaphoreTake(T.lock, portMAX_DELAY);
    int64_t end = T.t_end ? T.t_end : esp_timer_get_time();
    int sent = T.sent;
    snprintf(out, sizeof out,
             "{\"fw\":\"%s\",\"name\":\"%s\",\"state\":\"%s\",\"message\":\"%s\",\"mode\":\"%s\",\"host\":\"%s\",\"ip\":\"%s\","
             "\"port\":%d,\"count\":%d,\"interval_ms\":%d,\"size\":%d,\"sent\":%d,\"received\":%d,\"dup\":%d,\"reorder\":%d,"
             "\"send_err\":%d,\"peer_rx\":%lld,\"elapsed_ms\":%lld,\"wifi\":{\"rssi\":%d,\"channel\":%d,\"bluetooth\":%s},"
             "\"from\":%d,\"rtt\":[",
             closer_demo_version(), closer_demo_name(), STATE[T.state], T.message, T.ntp ? "ntp" : "echo", T.host, T.ip,
             T.port, T.count, T.interval_ms, T.size, sent, T.received, T.dup, T.reorder, T.send_err,
             (long long)T.peer_rx, T.state == IDLE ? 0LL : (long long)((end - T.t_start) / 1000), T.rssi_start,
             T.channel_start, T.bt ? "true" : "false", from);
    xSemaphoreGive(T.lock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, out, HTTPD_RESP_USE_STRLEN);
    for (int i = from; err == ESP_OK && i < sent;) {
        int n = 0;
        xSemaphoreTake(T.lock, portMAX_DELAY);          // (not held while sending: the test goes on meanwhile)
        for (; T.rtt && i < sent && i < T.sent && n < (int)sizeof out - 16; i++)   // (a new test may have begun)
            n += snprintf(out + n, sizeof out - n, "%s%d", i > from ? "," : "", T.rtt[i] == NONE ? -1 : T.rtt[i]);
        xSemaphoreGive(T.lock);
        if (!n) break;
        err = httpd_resp_send_chunk(req, out, n);
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "]}", 2);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t action_post(httpd_req_t *req) {
    char body[400];
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
    cJSON *a = cJSON_GetObjectItem(j, "action");
    char msg[100] = "Unknown action";
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(a) && strcmp(a->valuestring, "start") == 0) {
        e = start(j, msg, sizeof msg);
    } else if (cJSON_IsString(a) && strcmp(a->valuestring, "stop") == 0) {
        T.stop = true;
        e = ESP_OK;
        snprintf(msg, sizeof msg, "Stopping");
    }
    cJSON_Delete(j);
    char out[160];
    snprintf(out, sizeof out, "{\"ok\":%s,\"message\":\"%s\"}", e == ESP_OK ? "true" : "false", msg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

esp_err_t closer_udp_register(httpd_handle_t server) {
    T.lock = xSemaphoreCreateMutex();
    xTaskCreate(echo_task, "udp-echo", 3072, NULL, 5, NULL);
    const httpd_uri_t uris[] = {
        {.uri = "/udp", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/udp", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/udp", .method = HTTP_POST, .handler = action_post},
    };
    for (int i = 0; i < 3; i++) {
        esp_err_t e = httpd_register_uri_handler(server, &uris[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
