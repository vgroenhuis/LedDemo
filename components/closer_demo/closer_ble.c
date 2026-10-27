// Bluetooth LE control (see closer_ble.h), with the NimBLE host.
#include "closer_ble.h"
#include "sdkconfig.h"

#if !CONFIG_BT_NIMBLE_ENABLED

esp_err_t closer_ble_start(const char *app, closer_ble_command_cb on_command) { return ESP_ERR_NOT_SUPPORTED; }
void closer_ble_set_state(const char *text) {}
int closer_ble_connections(void) { return 0; }
bool closer_ble_enabled(void) { return false; }
esp_err_t closer_ble_set_enabled(bool on) { return ESP_ERR_NOT_SUPPORTED; }
bool closer_ble_running(void) { return false; }

#else

#include <stdio.h>
#include <string.h>
#include "closer_demo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "closer_ble";

// c105000N-f126-411b-829d-3570f3362a97, bytes in little-endian order as NimBLE wants them
#define CLOSER_UUID(n) BLE_UUID128_INIT(0x97, 0x2a, 0x36, 0xf3, 0x70, 0x35, 0x9d, 0x82, 0x1b, 0x41, 0x26, 0xf1, n, 0x00, 0x05, 0xc1)
static const ble_uuid128_t svc_uuid = CLOSER_UUID(0x01);
static const ble_uuid128_t cmd_uuid = CLOSER_UUID(0x02);
static const ble_uuid128_t state_uuid = CLOSER_UUID(0x03);
static const ble_uuid128_t echo_uuid = CLOSER_UUID(0x04);
static const ble_uuid128_t info_uuid = CLOSER_UUID(0x05);

#define MAX_CMD 64

static closer_ble_command_cb command_cb;
static char app_name[24];
static char state[161];
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t state_handle, echo_handle;
static uint8_t own_addr_type;
static int conns;
static uint32_t commands;
static bool running;

static void advertise(void);

static int read_text(struct ble_gatt_access_ctxt *ctxt, const char *text) {
    return os_mbuf_append(ctxt->om, text, strlen(text)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const ble_uuid_t *uuid = ctxt->chr->uuid;
    if (ble_uuid_cmp(uuid, &state_uuid.u) == 0) {
        char copy[sizeof state];
        taskENTER_CRITICAL(&state_lock);
        strcpy(copy, state);
        taskEXIT_CRITICAL(&state_lock);
        return read_text(ctxt, copy);
    }
    if (ble_uuid_cmp(uuid, &info_uuid.u) == 0) {
        char info[128];
        snprintf(info, sizeof info, "{\"demo\":\"%s\",\"app\":\"%s\",\"fw\":\"%s\"}", closer_demo_name(), app_name,
                 closer_demo_version());
        return read_text(ctxt, info);
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (ble_uuid_cmp(uuid, &echo_uuid.u) == 0) {
        // Straight back to whoever sent it: the page times the round trip.
        struct os_mbuf *om = ble_hs_mbuf_from_flat(NULL, 0);
        if (om && os_mbuf_appendfrom(om, ctxt->om, 0, OS_MBUF_PKTLEN(ctxt->om)) == 0) ble_gatts_notify_custom(conn, echo_handle, om);
        else if (om) os_mbuf_free_chain(om);
        return 0;
    }
    // command
    char buf[MAX_CMD + 1];
    uint16_t len = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) > MAX_CMD) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, MAX_CMD, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    buf[len] = 0;
    commands++;
    if (command_cb) command_cb(buf);
    return 0;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &cmd_uuid.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
            {.uuid = &state_uuid.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &state_handle},
            {.uuid = &echo_uuid.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &echo_handle},
            {.uuid = &info_uuid.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {0},
};

static void count_changed(void) {
    closer_demo_set_stat("ble_clients", conns);
    ESP_LOGI(TAG, "%d connected", conns);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conns++;
            count_changed();
            // Ask for short connection intervals (7.5 to 15 ms): commands then wait at most that long for their turn.
            // The phone or laptop decides; many settle on 15 to 30 ms.
            struct ble_gap_upd_params p = {.itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400,
                                           .min_ce_len = 0, .max_ce_len = 0};
            ble_gap_update_params(event->connect.conn_handle, &p);
        }
        advertise();                    // for the next one (up to the maximum)
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (conns > 0) conns--;
        count_changed();
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0)
            ESP_LOGI(TAG, "connection interval %.2f ms", d.conn_itvl * 1.25);
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void advertise(void) {
    if (conns >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS || ble_gap_adv_active()) return;
    // The service in the advertisement (pages look for it), the name in the scan response (no room for both).
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields r = {0};
    const char *name = ble_svc_gap_device_name();
    r.name = (uint8_t *)name;
    r.name_len = strlen(name);
    r.name_is_complete = 1;
    if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&r);
    // Every 400-500 ms, not the stack's default 30-60 ms: the C5 has one radio, and each advertisement takes it away
    // from Wi-Fi (on 5 GHz: a band switch). Fast advertising made the access point drop the board and connection
    // attempts fail. A phone still finds the board within a second or so.
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = BLE_GAP_ADV_ITVL_MS(400), .itvl_max = BLE_GAP_ADV_ITVL_MS(500)};
    if (rc == 0) rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "advertising failed (%d)", rc);
}

static void on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no Bluetooth address");
        return;
    }
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "Bluetooth host reset (%d)", reason); }

static void host_task(void *arg) {
    nimble_port_run();                  // returns only after nimble_port_stop()
    nimble_port_freertos_deinit();
}

bool closer_ble_enabled(void) {
    nvs_handle_t h;
    uint8_t on = 1;
    if (nvs_open("closer", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "ble", &on);
        nvs_close(h);
    }
    return on;
}

esp_err_t closer_ble_set_enabled(bool on) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("closer", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "ble", on);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool closer_ble_running(void) { return running; }

esp_err_t closer_ble_start(const char *app, closer_ble_command_cb on_command) {
    if (!closer_ble_enabled()) {
        ESP_LOGI(TAG, "Bluetooth is switched off (Wi-Fi settings page)");
        return ESP_OK;
    }
    command_cb = on_command;
    snprintf(app_name, sizeof app_name, "%s", app ? app : "");
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(services);
    if (rc == 0) rc = ble_gatts_add_svcs(services);
    if (rc == 0) rc = ble_svc_gap_device_name_set(closer_demo_name());
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed (%d)", rc);
        return ESP_FAIL;
    }
    closer_demo_set_stat("ble_clients", 0);
    nimble_port_freertos_init(host_task);
    running = true;
    ESP_LOGI(TAG, "advertising as %s", closer_demo_name());
    return ESP_OK;
}

void closer_ble_set_state(const char *text) {
    taskENTER_CRITICAL(&state_lock);
    snprintf(state, sizeof state, "%s", text);
    taskEXIT_CRITICAL(&state_lock);
    if (state_handle && conns) ble_gatts_chr_updated(state_handle);
}

int closer_ble_connections(void) { return conns; }

#endif
