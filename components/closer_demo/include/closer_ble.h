// Bluetooth LE control for a CloSer lab demo, beside Wi-Fi: a phone or laptop nearby talks to the board directly,
// without access point, internet or server in between (short, steady delays where the Wi-Fi is poor).
//
// One GATT service, the same for every lab demo, so one page or app can find any of them:
//   service  c1050001-f126-411b-829d-3570f3362a97
//   command  c1050002-...  write (with or without response): a text command, the same as the demo's WebSocket takes
//   state    c1050003-...  read + notify: a short text (JSON) the demo sets whenever it changes
//   echo     c1050004-...  write without response + notify: sent straight back, for measuring the round trip
//   info     c1050005-...  read: {"demo":"turtle-3","app":"turtle","fw":"1.13.0"} (which board, which control page)
// The board advertises the service and its name (the demo id). Anyone in range may connect (up to 3 at a time):
// fine for an LED; a demo that moves things should check a code in its commands (BesBot: closer_ble_set_conn_handlers
// to know which connection sent what, closer_ble_notify to answer that one alone).
//
//   closer_ble_start("turtle", on_command);   // after closer_demo_start(); on_command runs in the Bluetooth task
//   closer_ble_set_state("{\"hex\":\"00a0ff\"}");
//
// Needs CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED (sdkconfig.defaults); without them these do nothing.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*closer_ble_command_cb)(const char *text);

// app: which control page fits this firmware ("turtle", "besbot", ...). ESP_ERR_NOT_SUPPORTED without Bluetooth.
esp_err_t closer_ble_start(const char *app, closer_ble_command_cb on_command);
// Up to 160 characters; connected pages that listen get it at once. Keep it short: a notification carries what fits
// in one packet (MTU - 3: at least 20 bytes, usually 180+ with a phone or laptop).
void closer_ble_set_state(const char *text);
int closer_ble_connections(void);

// For demos that tell connections apart (who gave the code, who drives). Set before closer_ble_start(); on_command
// then gets the connection handle and replaces closer_ble_start's callback; on_disconnect runs when one goes.
typedef void (*closer_ble_conn_command_cb)(int conn, const char *text);
typedef void (*closer_ble_disconnect_cb)(int conn);
void closer_ble_set_conn_handlers(closer_ble_conn_command_cb on_command, closer_ble_disconnect_cb on_disconnect);
// A state notification to one connection only (a reply, or a state that differs per connection).
void closer_ble_notify(int conn, const char *text);
// The handles of the connections now open; returns how many (at most max).
int closer_ble_conn_handles(int *out, int max);
// Switched off on the Wi-Fi settings page (stored in flash; takes effect at the next start): closer_ble_start() then
// leaves Bluetooth off, so Wi-Fi has the radio to itself. closer_ble_running(): started and advertising.
bool closer_ble_enabled(void);
esp_err_t closer_ble_set_enabled(bool on);
bool closer_ble_running(void);

#ifdef __cplusplus
}
#endif
