// One controller at a time, with a code from the demo's display: see include/closer_access.h.
#include "closer_access.h"
#include "closer_demo.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "closer_access";

#define IDLE_US (10 * 60 * 1000000LL)      // control ends after this long without a control command
#define RETURN_US (60 * 1000000LL)         // a holder whose connection closed may come back with its token this long
#define CODE_US (60 * 1000000LL)           // a code is shown this long
#define CODE_TRIES 5
#define SHORT_CODE_TRIES 2                 // for codes of up to 3 digits
#define BURNT_WAIT_US (30 * 1000000LL)     // after a code's last wrong try: no new code for this long
#define CHECK_GAP_US (1000000LL)           // code checks at least this far apart (any client)

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int holder;                  // CLOSER_ACCESS_NONE: nobody, or the holder is away (then `held` still says so)
    bool held;                   // a lease exists (holder connected, or away for less than RETURN_US)
    bool by_grant;               // the holder got it with the server's grant (an admin, a server link)
    char token[17];
    int64_t used_us, gone_us;
    char code[9];
    int digits;
    int64_t code_until_us, no_code_until_us, next_check_us;
    int tries;
    char grant[65];
    char page[96];
    closer_access_change_fn on_change;
} A = {.holder = CLOSER_ACCESS_NONE, .digits = 6};

static void random_digits(char *out, int n) {
    for (int i = 0; i < n; i++) out[i] = '0' + esp_random() % 10;
    out[n] = 0;
}

static void random_hex(char *out, int n) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) out[i] = HEX[esp_random() & 15];
    out[n] = 0;
}

// The same time whatever matches.
static bool same(const char *a, const char *b) {
    size_t n = strlen(a), m = strlen(b);
    unsigned diff = n != m;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)a[i] ^ (unsigned char)(i < m ? b[i] : 0);
    return n && !diff;
}

// Under the lock: end a lease that ran out; returns the holder before, or -2 when nothing changed.
static int expire_locked(int64_t now) {
    if (A.code[0] && now > A.code_until_us) A.code[0] = 0;
    if (!A.held) return -2;
    bool idle = now - A.used_us > IDLE_US, away = A.holder == CLOSER_ACCESS_NONE && now - A.gone_us > RETURN_US;
    if (!idle && !away) return -2;
    int from = A.holder;
    A.held = false;
    A.holder = CLOSER_ACCESS_NONE;
    A.token[0] = 0;
    return from;
}

static void changed(int from, int to) {
    if (from == to) return;
    ESP_LOGI(TAG, "control: %d -> %d", from, to);
    if (A.on_change) A.on_change(from, to);
}

static void expire(void) {
    portENTER_CRITICAL(&mux);
    int from = expire_locked(esp_timer_get_time());
    portEXIT_CRITICAL(&mux);
    if (from != -2) changed(from, CLOSER_ACCESS_NONE);
}

// Under the lock: `client` holds control from now on, with a new token; returns the previous holder.
static int give_locked(int client, int64_t now, bool by_grant) {
    int from = A.holder;
    A.by_grant = by_grant;
    A.holder = client;
    A.held = true;
    A.used_us = now;
    random_hex(A.token, 16);
    A.code[0] = 0;               // (a used code is gone from the display)
    return from;
}

void closer_access_init(const char *page, closer_access_change_fn on_change) {
    strlcpy(A.page, page ? page : "", sizeof A.page);
    A.on_change = on_change;
}

void closer_access_set_digits(int digits) {
    portENTER_CRITICAL(&mux);
    A.digits = digits < 2 ? 2 : digits > 8 ? 8 : digits;
    A.code[0] = 0;
    portEXIT_CRITICAL(&mux);
}

void closer_access_set_grant(const char *grant) {
    portENTER_CRITICAL(&mux);
    strlcpy(A.grant, grant ? grant : "", sizeof A.grant);
    portEXIT_CRITICAL(&mux);
}

bool closer_access_has(int client) {
    expire();
    return client != CLOSER_ACCESS_NONE && A.held && A.holder == client;
}

int closer_access_holder(void) {
    expire();
    return A.holder;
}

bool closer_access_token_ok(const char *token) {
    expire();
    portENTER_CRITICAL(&mux);
    bool ok = A.held && token && same(A.token, token);
    if (ok) A.used_us = esp_timer_get_time();
    portEXIT_CRITICAL(&mux);
    return ok;
}

void closer_access_used(int client) {
    portENTER_CRITICAL(&mux);
    if (A.held && A.holder == client) A.used_us = esp_timer_get_time();
    portEXIT_CRITICAL(&mux);
}

void closer_access_gone(int client) {
    portENTER_CRITICAL(&mux);
    bool was = A.held && A.holder == client;
    if (was) { A.holder = CLOSER_ACCESS_NONE; A.gone_us = esp_timer_get_time(); }   // (the token stays valid a while)
    portEXIT_CRITICAL(&mux);
    if (was) changed(client, CLOSER_ACCESS_NONE);
}

void closer_access_take(int client) {
    portENTER_CRITICAL(&mux);
    int from = give_locked(client, esp_timer_get_time(), false);
    portEXIT_CRITICAL(&mux);
    changed(from, client);
}

bool closer_access_by_grant(int client) {
    return closer_access_has(client) && A.by_grant;
}

const char *closer_access_state(int client) {
    expire();
    return !A.held ? "none" : A.holder == client ? "you" : "other";
}

int closer_access_code(char *code, size_t code_len, char *link, size_t link_len) {
    expire();
    char c[9];
    int64_t left;
    portENTER_CRITICAL(&mux);
    strlcpy(c, A.code, sizeof c);
    left = A.code_until_us - esp_timer_get_time();
    portEXIT_CRITICAL(&mux);
    if (!c[0]) return 0;
    if (code) strlcpy(code, c, code_len);
    // The code goes after "#": browsers don't send it to the server (not in its logs); the page reads it.
    if (link) snprintf(link, link_len, "%s/%s#%s", closer_demo_base_url(), A.page, c);
    return (int)((left + 999999) / 1000000);
}

static int reply_granted(char *reply, size_t len) {
    portENTER_CRITICAL(&mux);
    char t[17];
    strlcpy(t, A.token, sizeof t);
    portEXIT_CRITICAL(&mux);
    return snprintf(reply, len, "{\"access\":\"granted\",\"token\":\"%s\"}", t);
}

bool closer_access_message(int client, const char *text, char *reply, size_t len) {
    if (!strstr(text, "\"access\"")) return false;
    cJSON *j = cJSON_Parse(text);
    cJSON *a = j ? cJSON_GetObjectItem(j, "access") : NULL;
    if (!cJSON_IsString(a)) { cJSON_Delete(j); return false; }
    expire();
    const char *what = a->valuestring;
    cJSON *arg;
    int64_t now = esp_timer_get_time();
    int from = -2;
    reply[0] = 0;

    if (strcmp(what, "request") == 0) {
        char shown[9] = "";
        portENTER_CRITICAL(&mux);
        int64_t wait = A.no_code_until_us - now;
        if (!A.code[0] && wait <= 0) {
            random_digits(A.code, A.digits);
            A.code_until_us = now + CODE_US;
            A.tries = 0;
            strlcpy(shown, A.code, sizeof shown);
        }
        int64_t left = A.code_until_us - now;
        bool have = A.code[0];
        portEXIT_CRITICAL(&mux);
        if (shown[0]) ESP_LOGI(TAG, "control code %s (client %d asked)", shown, client);
        if (have) snprintf(reply, len, "{\"access\":\"shown\",\"for\":%d}", (int)(left / 1000000));
        else snprintf(reply, len, "{\"access\":\"wait\",\"for\":%d}", (int)((wait + 999999) / 1000000));

    } else if (strcmp(what, "code") == 0 && cJSON_IsString(arg = cJSON_GetObjectItem(j, "code"))) {
        const char *answer;
        portENTER_CRITICAL(&mux);
        if (now < A.next_check_us) answer = "wait";
        else if (!A.code[0]) answer = "expired";
        else {
            A.next_check_us = now + CHECK_GAP_US;
            if (same(A.code, arg->valuestring)) { from = give_locked(client, now, false); answer = NULL; }
            else if (++A.tries >= (A.digits <= 3 ? SHORT_CODE_TRIES : CODE_TRIES)) { A.code[0] = 0; A.no_code_until_us = now + BURNT_WAIT_US; answer = "expired"; }
            else answer = "wrong";
        }
        portEXIT_CRITICAL(&mux);
        if (answer) {
            ESP_LOGI(TAG, "client %d: code %s", client, answer);
            if (strcmp(answer, "wait") == 0) snprintf(reply, len, "{\"access\":\"wait\",\"for\":1}");
            else snprintf(reply, len, "{\"access\":\"%s\"}", answer);
        }

    } else if (strcmp(what, "token") == 0 && cJSON_IsString(arg = cJSON_GetObjectItem(j, "token"))) {
        portENTER_CRITICAL(&mux);
        bool ok = A.held && same(A.token, arg->valuestring);
        if (ok && A.holder != client) { from = A.holder; A.holder = client; }
        if (ok) A.used_us = now;
        portEXIT_CRITICAL(&mux);
        if (ok) reply_granted(reply, len);
        else snprintf(reply, len, "{\"access\":\"expired\"}");

    } else if (strcmp(what, "grant") == 0 && cJSON_IsString(arg = cJSON_GetObjectItem(j, "grant"))) {
        bool if_free = cJSON_IsTrue(cJSON_GetObjectItem(j, "if_free")) ||
                       (cJSON_IsNumber(cJSON_GetObjectItem(j, "if_free")) && cJSON_GetObjectItem(j, "if_free")->valueint);
        const char *answer = NULL;
        portENTER_CRITICAL(&mux);
        if (!same(A.grant, arg->valuestring)) answer = "denied";
        else if (A.held && A.holder == client) A.used_us = now;
        else if (if_free && A.held) answer = "busy";
        else from = give_locked(client, now, true);
        portEXIT_CRITICAL(&mux);
        if (answer) snprintf(reply, len, "{\"access\":\"%s\"}", answer);

    } else if (strcmp(what, "release") == 0) {
        portENTER_CRITICAL(&mux);
        if (A.held && A.holder == client) { from = client; A.held = false; A.holder = CLOSER_ACCESS_NONE; A.token[0] = 0; }
        portEXIT_CRITICAL(&mux);
        snprintf(reply, len, "{\"access\":\"released\"}");

    } else {
        snprintf(reply, len, "{\"access\":\"unknown\"}");
    }
    cJSON_Delete(j);

    if (from != -2) {
        int to = A.held ? A.holder : CLOSER_ACCESS_NONE;
        if (to == client && !reply[0]) reply_granted(reply, len);
        changed(from, to);
    }
    return true;
}
