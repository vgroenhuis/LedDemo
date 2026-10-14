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

#ifdef __cplusplus
extern "C" {
#endif

// Starts Wi-Fi, time, reporting and updates in the background. hostname: e.g. "turtle" (reachable as turtle.local too).
esp_err_t closer_demo_start(const char *hostname);

void closer_demo_set_stat(const char *name, float value);   // name: [a-z][a-z0-9_]{0,23}
void closer_demo_set_clients(int n);
void closer_demo_set_message(const char *text);              // short note on the demo page (max 120 characters)

// Time-critical moments: no roaming scans while held (scans briefly interrupt traffic). Call with true before
// the critical part and false after it; a scan that was due then happens later.
void closer_demo_roaming_hold(bool hold);

bool closer_demo_wifi_connected(void);
const char *closer_demo_version(void);                       // the running firmware version

#ifdef __cplusplus
}
#endif
