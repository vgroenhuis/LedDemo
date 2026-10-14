# Turtle: RGB LED demo (ESP32-C5-DevKitC-1, ESP-IDF 5.5)

> This is the firmware side of the CloSer lab demos (https://closer.ram.eemcs.utwente.nl/labdemos/). The server side
> (demo list, heartbeat API, firmware releases) runs on the CloSer server and is not part of this repository: the board
> POSTs a JSON report to `<base_url>/api/heartbeat` once a minute and, when the reply offers another firmware version,
> downloads it from `<base_url>/api/firmware/<id>/<version>.bin`. The `labdemos.cli` commands below are run on that server.
> Secrets (Wi-Fi password, device key) are never in this repository: they live in `local/` (ignored) and in the board's flash.


A web page sets the colour of the on-board WS2812 LED (GPIO27, see `idf.py menuconfig` > Turtle demo).
`components/closer_demo/` is the reusable part for every demo: Wi-Fi, clock, reports to the lab demos page,
over-the-air updates with automatic rollback. A new demo is a new `main/` that calls `closer_demo_start("name")`.

## Build (on the server)
    . ~/esp/idf-env.sh            # ESP-IDF 5.5.5 (+ the Python 3.12 it needs) in ~/esp
    cd ~/closer-deploy/labdemos/firmware/turtle
    idf.py build

The version is in `VERSION` (reported by the device and compared with the release).

## First flash (USB, once)
    python tools/provision.py     # local/turtle-factory.bin: firmware + Wi-Fi settings + device key
Copy `local/turtle-factory.bin` to the computer the board is plugged into and flash it at 0x0:

    python -m esptool --chip esp32c5 -p <port> write_flash 0x0 turtle-factory.bin

`local/` holds the Wi-Fi password and the device key; it is not in git. Wi-Fi settings and key live in their own
flash partition (NVS, 0x9000): OTA updates never touch them. Change them with `local/turtle-settings.bin` at 0x9000.

## Update over the air
    # edit code, bump VERSION
    idf.py build
    cd ../.. && .venv/bin/python -m labdemos.cli publish turtle firmware/turtle/build/turtle.bin
The device reports every minute; when the server answers with a version other than the one it runs, it downloads
`/labdemos/api/firmware/turtle/<version>.bin` (HTTPS, authenticated with its key), checks the SHA-256 from the manifest,
writes the other app slot and restarts. The new image must then send a report successfully within 2 minutes
(`esp_ota_mark_app_valid_cancel_rollback`), otherwise the bootloader returns to the previous slot and the version
is remembered as failed (shown on the demo page: "update to X failed, rolled back") and not offered again until a
different version is published. A crash loop before confirmation also rolls back.
Roll back by publishing an older build. `unpublish turtle` stops offering updates.

## Not done yet
* Firmware signing: the integrity check is the SHA-256 from the server over HTTPS, so whoever controls the server controls the devices.
  Next step: signed app images (CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT) with the key kept off the web path.
* Remote testing of a release before it goes out (run it on a spare board, or a simulator, and publish when it passes).

## Roaming (components/closer_demo, settings under "CloSer demo reporting" in menuconfig)
The chip joins one access point and stays; with many access points sharing one network name (iotroam) that may
be a poor one. The firmware looks around (a scan for the same SSID) and moves only to an access point that is at
least `CLOSER_ROAM_MARGIN` (10 dB) stronger.
* Once ~10 s after connecting (`CLOSER_ROAM_INITIAL_SCAN`).
* While the signal is below `CLOSER_ROAM_RSSI` (-67 dBm): after 60 s, and if that finds nothing better, after 120, 240 ... up to
  `CLOSER_ROAM_BACKOFF_MAX_S` (600 s). Moving resets the wait.
* Every `CLOSER_ROAM_PERIODIC_S` seconds regardless (default 0 = never).
A scan listens `CLOSER_ROAM_SCAN_CHANNEL_MS` per channel and returns to the network's channel for `CLOSER_ROAM_SCAN_DWELL_MS`
between channels, so traffic is delayed by tens of milliseconds at a time over a few seconds, not blocked.
For time-critical parts call `closer_demo_roaming_hold(true)` ... `closer_demo_roaming_hold(false)`; or turn roaming off
(`CLOSER_ROAMING`). The page shows `roam_scans`, `roams` and `alt_rssi` (strongest other access point at the last scan).

## LED page
`/ws` (WebSocket) takes "rrggbb,brightness" text frames; the page uses it for every slider step and falls back to
`POST /api/led` if the socket is down. The colour is saved to flash 1.5 s after the last change (a flash write stalls the web server).
