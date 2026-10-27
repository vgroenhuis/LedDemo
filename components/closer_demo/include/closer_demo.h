// Everything a CloSer lab demo needs besides the demo itself:
//   * joins Wi-Fi (network and password from flash, see tools/provision.py), keeps the time, reconnects
//   * reports to the lab demos page about once a minute (address, signal, memory, your own numbers)
//   * installs the firmware version the server asks for (over the air, with a safety net:
//     a new version that can't report to the server within two minutes is rolled back)
//
//   closer_demo_start();                         // once, early in app_main
//   closer_demo_set_stat("presses", n);          // up to 16 numbers of your own
//   closer_demo_set_clients(n);                  // people currently using your web page
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts Wi-Fi, time, reporting and updates in the background. hostname: e.g. "turtle" (reachable as turtle.local too);
// NULL: the demo id in the device key (so one firmware can serve several boards).
esp_err_t closer_demo_start(const char *hostname);
const char *closer_demo_name(void);
// Why the chip last started ("power_on", "brownout", "crash", "task_watchdog", "software", ...).
const char *closer_demo_reset_reason(void);                          // the hostname in use

void closer_demo_set_stat(const char *name, float value);   // name: [a-z][a-z0-9_]{0,23}
void closer_demo_set_clients(int n);
void closer_demo_set_message(const char *text);              // short note on the demo page (max 120 characters)

// Time-critical moments: no roaming scans while held (scans briefly interrupt traffic). Call with true before
// the critical part and false after it; a scan that was due then happens later.
void closer_demo_roaming_hold(bool hold);

// What Wi-Fi is doing, for a status light (closer_demo_status_t.phase).
typedef enum {
    CLOSER_WIFI_STARTING,
    CLOSER_WIFI_SEARCHING,      // looking for the network (not connected since start-up)
    CLOSER_WIFI_GETTING_IP,     // joined an access point, waiting for an address
    CLOSER_WIFI_CONNECTED,
    CLOSER_WIFI_SCANNING,       // connected, scanning for a stronger access point
    CLOSER_WIFI_ROAMING,        // moving to a stronger access point
    CLOSER_WIFI_RECONNECTING,   // the connection was lost, trying again
    CLOSER_WIFI_AUTH_FAILED,    // the access point turned us down: wrong password?
} closer_wifi_phase_t;

// For a status display.
typedef struct {
    bool connected;
    char ip[16];          // "" while not connected
    int rssi;             // dBm, 0 when unknown
    bool updating;        // installing a firmware update right now
    char note[121];       // the short message shown on the demo page ("update failed ...")
    char wifi[48];        // what Wi-Fi is doing, e.g. "Searching for iotroam", "Joining iotroam (ch 124)",
                          // "Getting an address", "Connected", "Wrong password? (try 3)", "Looking for a better AP"
    char ssid[33];        // the network
    int channel;          // of the access point, 0 when not associated
    int attempts;         // connection attempts since the last success
    closer_wifi_phase_t phase;
    int down_s;           // seconds without a connection (since start-up or since it was lost); 0 while connected
    char gateway[16];     // "" while not connected
    char bssid[18];       // the access point, "aa:bb:cc:dd:ee:ff"; "" while not associated
} closer_demo_status_t;
void closer_demo_get_status(closer_demo_status_t *out);

// A survey: scans all channels for every network (gently, like the roaming scans: traffic is delayed, not dropped)
// and fills up to *n records, strongest first; *n becomes the number filled. Only while connected.
// ESP_ERR_INVALID_STATE while not connected or while another scan runs.
esp_err_t closer_demo_scan_all(wifi_ap_record_t *out, uint16_t *n);

// POSTs JSON to the lab demos server (path after the base URL, e.g. "/api/survey-data"), authenticated with the
// device key. Returns the HTTP status, or -1 if it couldn't be sent (not connected, network error).
int closer_demo_post(const char *path, const char *body, size_t len);

bool closer_demo_wifi_connected(void);
const char *closer_demo_version(void);                       // the running firmware version

// ------------------------------------------------------------------ Wi-Fi settings (the /wifi page, closer_wifi_web.h)
// Saved networks (the provisioned one first), a chosen band or access point, roaming. A change of network, band or
// access point is tried first: kept (and stored in flash) once the board has an address, otherwise the previous
// settings come back within a minute. While the network in use can't be found, the board moves on to the next
// saved network now and then.

#define CLOSER_MAX_NETWORKS 8
typedef enum { CLOSER_BAND_BOTH, CLOSER_BAND_2G, CLOSER_BAND_5G } closer_band_t;
typedef struct { bool enabled; int period_s; int weak_rssi; int margin_db; } closer_roam_t;
typedef struct { char ssid[33]; bool open; } closer_net_t;

int closer_demo_saved_networks(closer_net_t *out, int max);             // returns how many
// Adds or updates a saved network. password NULL: keep the stored one (an error for a new network).
esp_err_t closer_demo_wifi_save(const char *ssid, const char *password);
esp_err_t closer_demo_wifi_forget(const char *ssid);                    // not the one in use
// To a saved network; bssid NULL or "": any of its access points (channel: a hint, 0 unknown).
esp_err_t closer_demo_wifi_connect(const char *ssid, const char *bssid, int channel);
esp_err_t closer_demo_set_band(closer_band_t band);
closer_band_t closer_demo_get_band(void);
// The access point the board sticks to ("" none: it picks and roams); pinned ssid is always the network in use.
const char *closer_demo_pinned_bssid(void);
esp_err_t closer_demo_unpin(void);
void closer_demo_get_roam(closer_roam_t *out);
esp_err_t closer_demo_set_roam(const closer_roam_t *roam);
// A change being tried right now, and how the last one ended ("Connected to x", "Could not connect to x ...").
bool closer_demo_wifi_switching(void);
void closer_demo_wifi_note(char *out, size_t len);
// The last full scan (closer_demo_scan_all), strongest first; *age_ms: how long ago (-1: none yet).
int closer_demo_last_scan(wifi_ap_record_t *out, int max, int64_t *age_ms);
esp_err_t closer_demo_request_scan(void);                               // a full scan in the background
bool closer_demo_scan_busy(void);

#ifdef __cplusplus
}
#endif
