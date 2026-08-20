/*
 * MAXUS 9 EV - ESP32 Bridge (Phase 2)
 * Step 4 POC (ESP-IDF) - full UDS decode. Carries the BT+WiFi coexistence base
 * from Step 3 (poc3) and replaces the 4 hard-coded PID reads with a table-driven
 * poller: every confirmed PID (+ MG-referenced testing candidates) is read,
 * decoded through the ported uds.c parser, and served over WiFi. Cross-checked
 * against src/webdash.py on the car.
 *
 * Decode stack ported from src/uds.py (parser) + src/pids.py CONFIRMED (formulas)
 * + docs/findings/07-mg-ev-reference.md (MG leads). Read-only (Service 22 only).
 * Board: classic ESP32 (WROOM-32 / ESP32-D0WD-V3).
 */

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "uds.h"

static const char *TAG = "poc4_dec";

/* --- WiFi SoftAP config --- */
#define AP_SSID    "MaxusBridge"
#define AP_PASS    "maxus1234"      /* >= 8 chars for WPA2 */
#define AP_CHANNEL 1

/* --- ELM327 target (from Step 2) --- */
#define TARGET_NAME_HINT "OBD"
static const esp_bd_addr_t KNOWN_ELM327_BDA = {0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33};

/* ELM327 init sequence (ported from src/elm327.py:init). No ATSH here - the
 * table poller sets the header per PID group. */
static const char *INIT_CMDS[] = {
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATAT2", "ATST96", "ATSP6",
};
#define INIT_COUNT (sizeof(INIT_CMDS) / sizeof(INIT_CMDS[0]))

/* ============================ PID table ============================ */
/* Ports src/pids.py CONFIRMED + the MG leads from note 07. INT16 = data[0]*256
 * + data[1]. Grouped by header so the poller only sends ATSH when it changes. */

/* Rendering form for non-plain-numeric values. */
typedef enum { FMT_NUM, FMT_BMS_STATUS, FMT_GEAR, FMT_BOOL } pid_fmt_t;

typedef struct {
    const char *name;
    const char *group;     /* dashboard tab: "BMS" / "Motor" / "Vehicle" */
    const char *header;    /* "7E5" (BMS) or "7E3" (VCU)                 */
    const char *cmd;       /* "22 B0 42"                                 */
    const char *did;       /* expected positive-response DID             */
    uint8_t     nbytes;    /* data bytes consumed, big-endian (1-4)      */
    float       scale;
    float       offset;    /* value = raw * scale + offset               */
    int         dec;       /* decimals for display                       */
    const char *unit;
    pid_fmt_t   fmt;
    bool        testing;   /* true = unverified candidate ("test" tag)   */
} pid_desc_t;

/* Ordered by header so the poller only sends ATSH when the header changes.
 * Confirmed formulas: docs/findings/01-confirmed-pids.md. Leads + all VCU
 * formulas: docs/findings/09-ovms-cross-reference.md (MG cross-reference -
 * every lead stays `testing` until read on our own car). */
static const pid_desc_t PIDS[] = {
    /* ---------- BMS 7E5 - confirmed ---------- */
    {"Bus Voltage",   "BMS", "7E5", "22 B0 41", "B041", 2, 0.25f,     0.0f, 1, "V", FMT_NUM, false},
    {"Pack Voltage",  "BMS", "7E5", "22 B0 42", "B042", 2, 0.25f,     0.0f, 1, "V", FMT_NUM, false},
    {"Pack Current",  "BMS", "7E5", "22 B0 43", "B043", 2, 0.025f, -1000.0f, 1, "A", FMT_NUM, false},
    {"Cell V Max",    "BMS", "7E5", "22 B0 58", "B058", 2, 0.001f,    0.0f, 3, "V", FMT_NUM, false},
    {"Cell V Min",    "BMS", "7E5", "22 B0 59", "B059", 2, 0.001f,    0.0f, 3, "V", FMT_NUM, false},
    {"Batt Temp Hi",  "BMS", "7E5", "22 B0 56", "B056", 1, 0.5f,    -40.0f, 1, "C", FMT_NUM, false},
    {"Batt Temp Lo",  "BMS", "7E5", "22 B0 57", "B057", 1, 0.5f,    -40.0f, 1, "C", FMT_NUM, false},
    {"Charge Status", "BMS", "7E5", "22 B0 48", "B048", 1, 0.0f,      0.0f, 0, "",  FMT_BMS_STATUS, false},
    /* ---------- BMS 7E5 - testing leads ---------- */
    {"SOC Raw (B046)","BMS", "7E5", "22 B0 46", "B046", 2, 0.1f,      0.0f, 1, "%", FMT_NUM, true},
    {"SOH (B061)",    "BMS", "7E5", "22 B0 61", "B061", 2, 0.01f,     0.0f, 1, "%", FMT_NUM, true},
    {"Coolant (B05C)","BMS", "7E5", "22 B0 5C", "B05C", 1, 0.5f,    -40.0f, 1, "C", FMT_NUM, true},
    {"SOC alt (005B)","BMS", "7E5", "22 00 5B", "005B", 1, 100.0f/255.0f, 0.0f, 1, "%", FMT_NUM, true},
    {"Ambient (0046)","BMS", "7E5", "22 00 46", "0046", 1, 1.0f,    -40.0f, 1, "C", FMT_NUM, true},
    {"Resistance",    "BMS", "7E5", "22 B0 45", "B045", 2, 0.5f,      0.0f, 1, "Ohm", FMT_NUM, true},
    {"Batt Error",    "BMS", "7E5", "22 B0 47", "B047", 1, 1.0f,      0.0f, 0, "",  FMT_NUM, true},
    {"BMS Range",     "BMS", "7E5", "22 B0 CE", "B0CE", 2, 1.0f,      0.0f, 0, "raw", FMT_NUM, true},
    {"Speed (000D)",  "BMS", "7E5", "22 00 0D", "000D", 1, 1.0f,      0.0f, 0, "kph", FMT_NUM, true},
    /* ---------- VCU 7E3 - motor group (testing) ---------- */
    {"Motor Temp",    "Motor", "7E3", "22 B4 05", "B405", 1, 1.0f,  -40.0f, 0, "C", FMT_NUM, true},
    {"VCU Coolant",   "Motor", "7E3", "22 B3 09", "B309", 1, 1.0f,  -40.0f, 0, "C", FMT_NUM, true},
    {"Motor Speed",   "Motor", "7E3", "22 B4 02", "B402", 2, 1.0f, -32767.0f, 0, "rpm", FMT_NUM, true},
    {"Motor Torque",  "Motor", "7E3", "22 B4 01", "B401", 2, 1.0f, -32767.0f, 0, "", FMT_NUM, true},
    /* ---------- VCU 7E3 - vehicle group ---------- */
    {"12V Battery",   "Vehicle", "7E3", "22 01 12", "0112", 1, 1.0f,  0.0f, 1, "V", FMT_NUM, false},
    {"Gear",          "Vehicle", "7E3", "22 B9 00", "B900", 1, 0.0f,  0.0f, 0, "",  FMT_GEAR, true},
    {"Ignition",      "Vehicle", "7E3", "22 B1 8C", "B18C", 1, 1.0f,  0.0f, 0, "",  FMT_NUM, true},
    {"HV Contactor",  "Vehicle", "7E3", "22 B7 03", "B703", 1, 0.0f,  0.0f, 0, "",  FMT_BOOL, true},
    {"Handbrake",     "Vehicle", "7E3", "22 BA 40", "BA40", 1, 0.0f,  0.0f, 0, "",  FMT_BOOL, true},
    {"Vehicle Speed", "Vehicle", "7E3", "22 BA 00", "BA00", 2, 0.01f, -200.0f, 1, "kph", FMT_NUM, true},
    {"Charge Rate",   "Vehicle", "7E3", "22 B7 12", "B712", 2, 0.1f,   0.0f, 1, "kW", FMT_NUM, true},
    {"Odometer",      "Vehicle", "7E3", "22 E1 01", "E101", 3, 1.0f,   0.0f, 0, "km", FMT_NUM, true},
};
#define N_PIDS (sizeof(PIDS) / sizeof(PIDS[0]))

/* Gear enum (note 09, MG VCU): 8=Park, 0=Reverse, anything else = Drive. */
static const char *gear_name(uint32_t v)
{
    switch (v) {
    case 8:  return "Park";
    case 0:  return "Reverse";
    default: return "Drive";
    }
}

/* B048 BMS-status enum, verbatim from note 07 (MG firmware). */
static const char *b048_enum(int v)
{
    switch (v) {
    case 0x00: return "ConnNotCharging";
    case 0x01: return "Idle";
    case 0x03: return "Running";
    case 0x06: return "Charging(AC)";
    case 0x07: return "Charging(DC)";
    case 0x08: return "AboutToSleep";
    case 0x0a: return "Connected";
    case 0x0c: return "StartingCharge";
    default:   return "?";
    }
}

/* --- Shared snapshot: one decoded value per PID (mirrors webdash _snapshot) --- */
typedef struct {
    bool valid;
    char raw[40];    /* data bytes as hex, for cross-check / SOC hunt */
    char text[28];   /* formatted value, e.g. "400.5 V" or "Running" */
} pid_val_t;

static SemaphoreHandle_t s_snap_mutex;
static pid_val_t g_vals[N_PIDS];
static bool      g_bt_connected;
static uint32_t  g_seq;

/* --- BT discovery/connect state --- */
static bool s_target_found = false;
static esp_bd_addr_t s_target_bda;
static volatile uint32_t s_spp_handle = 0;
static StreamBufferHandle_t s_rx_stream;
static TaskHandle_t s_poll_task = NULL;

/* ============================ ELM327 over SPP ============================ */

static bool name_looks_like_elm(const char *name)
{
    if (!name) return false;
    char up[64];
    size_t i;
    for (i = 0; i < sizeof(up) - 1 && name[i]; i++)
        up[i] = (char)toupper((unsigned char)name[i]);
    up[i] = '\0';
    return strstr(up, TARGET_NAME_HINT) != NULL;
}

/* Send one AT/OBD command, read until '>' or timeout, return sanitized reply
 * (printable ASCII; non-printable -> space, drop '>', trim). Ported from
 * src/elm327.py:_send_raw + the poc2/poc3 sanitizer. */
static bool elm_cmd(const char *cmd, char *clean_out, size_t clean_sz, uint32_t timeout_ms)
{
    if (!s_spp_handle) return false;
    xStreamBufferReset(s_rx_stream);

    char out[48];
    int n = snprintf(out, sizeof(out), "%s\r", cmd);
    esp_spp_write((uint32_t)s_spp_handle, n, (uint8_t *)out);

    char raw[256];
    size_t rlen = 0;
    bool got_prompt = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        uint8_t tmp[64];
        size_t got = xStreamBufferReceive(s_rx_stream, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
        for (size_t i = 0; i < got && rlen < sizeof(raw) - 1; i++) raw[rlen++] = (char)tmp[i];
        if (got && memchr(tmp, '>', got)) { got_prompt = true; break; }
    }

    size_t ci = 0;
    for (size_t i = 0; i < rlen && ci < clean_sz - 1; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '>') continue;
        clean_out[ci++] = (c >= 0x20 && c <= 0x7e) ? (char)c : ' ';
    }
    clean_out[ci] = '\0';
    char *s = clean_out;
    while (*s == ' ') s++;
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = '\0';
    if (s != clean_out) memmove(clean_out, s, strlen(s) + 1);
    return got_prompt;
}

/* Format the decoded data bytes of a result as hex, into out. */
static void data_hex(const uds_result_t *r, char *out, size_t sz)
{
    size_t o = 0;
    for (int i = 0; i < r->n && o + 2 < sz; i++)
        o += snprintf(out + o, sz - o, "%02X", r->data[i]);
    if (o == 0 && sz) out[0] = '\0';
}

/* Poll task: init once, then loop the PID table, decoding each into g_vals. */
static void poll_task(void *arg)
{
    char buf[128];
    for (size_t i = 0; i < INIT_COUNT; i++) {
        uint32_t to = (strcmp(INIT_CMDS[i], "ATZ") == 0) ? 5000 : 2000;
        bool ok = elm_cmd(INIT_CMDS[i], buf, sizeof(buf), to);
        ESP_LOGI(TAG, "init %-8s -> %s%s", INIT_CMDS[i], buf, ok ? "" : "  [timeout]");
    }
    ESP_LOGI(TAG, "Init done. Polling %d PIDs every 2 s...", (int)N_PIDS);

    while (s_spp_handle) {
        pid_val_t local[N_PIDS];
        const char *cur_hdr = NULL;

        for (size_t i = 0; i < N_PIDS; i++) {
            const pid_desc_t *p = &PIDS[i];
            if (!cur_hdr || strcmp(cur_hdr, p->header) != 0) {
                char h[16];
                snprintf(h, sizeof(h), "ATSH %s", p->header);
                elm_cmd(h, buf, sizeof(buf), 2000);
                cur_hdr = p->header;
            }
            elm_cmd(p->cmd, buf, sizeof(buf), 2000);
            uds_result_t r = uds_parse(buf);
            /* Need a positive response for the DID we asked for, with enough
             * data bytes for this descriptor. */
            bool ok = r.positive && strcmp(r.did, p->did) == 0 && r.n >= p->nbytes;

            local[i].valid = ok;
            data_hex(&r, local[i].raw, sizeof(local[i].raw));
            if (!ok) {
                snprintf(local[i].text, sizeof(local[i].text), "--");
            } else {
                /* Big-endian unsigned over nbytes, then value = raw*scale+offset. */
                uint32_t raw = 0;
                for (int b = 0; b < p->nbytes; b++) raw = (raw << 8) | r.data[b];

                switch (p->fmt) {
                case FMT_BMS_STATUS:
                    snprintf(local[i].text, sizeof(local[i].text), "%s (%02X)",
                             b048_enum((int)raw), (unsigned)raw);
                    break;
                case FMT_GEAR:
                    snprintf(local[i].text, sizeof(local[i].text), "%s", gear_name(raw));
                    break;
                case FMT_BOOL:
                    snprintf(local[i].text, sizeof(local[i].text), "%s",
                             (raw & 1u) ? "Yes" : "No");
                    break;
                default:
                    snprintf(local[i].text, sizeof(local[i].text), "%.*f %s",
                             p->dec, raw * p->scale + p->offset, p->unit);
                    break;
                }
            }
        }

        xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
        memcpy(g_vals, local, sizeof(g_vals));
        g_bt_connected = true;
        g_seq++;
        uint32_t seq = g_seq;
        xSemaphoreGive(s_snap_mutex);

        int ok_count = 0;
        for (size_t i = 0; i < N_PIDS; i++) if (local[i].valid) ok_count++;
        ESP_LOGI(TAG, "[poll #%lu] %d/%d ok | PackV=%s SOCraw=%s | heap=%lu",
                 (unsigned long)seq, ok_count, (int)N_PIDS,
                 g_vals[1].text, g_vals[8].text,
                 (unsigned long)esp_get_free_heap_size());
        /* ~29 PIDs/cycle already paces the loop (a few seconds); keep the extra
         * delay short so the dashboard stays responsive. */
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    g_bt_connected = false;
    xSemaphoreGive(s_snap_mutex);
    ESP_LOGW(TAG, "poll_task exiting (BT link closed)");
    s_poll_task = NULL;
    vTaskDelete(NULL);
}

static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        const char *name = NULL;
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME)
                name = (const char *)param->disc_res.prop[i].val;
        }
        bool mac_match = memcmp(param->disc_res.bda, KNOWN_ELM327_BDA, ESP_BD_ADDR_LEN) == 0;
        if (!s_target_found && (name_looks_like_elm(name) || mac_match)) {
            memcpy(s_target_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
            s_target_found = true;
            ESP_LOGI(TAG, "ELM327 found (%s) - will connect", mac_match ? "known MAC" : "name");
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (s_target_found) {
                ESP_LOGI(TAG, "Discovery done. SPP SDP lookup...");
                esp_spp_start_discovery(s_target_bda);
            } else {
                ESP_LOGW(TAG, "No ELM327 found - rescanning in 3 s...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
            ESP_LOGI(TAG, "Legacy PIN auth SUCCESS with %s", param->auth_cmpl.device_name);
        else
            ESP_LOGE(TAG, "Legacy PIN auth FAILED, status=%d", param->auth_cmpl.stat);
        break;
    default:
        break;
    }
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(TAG, "SPP init. Scanning ~10 s for the ELM327...");
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
            ESP_LOGI(TAG, "SDP SPP channel %d - connecting (authenticated)...",
                     param->disc_comp.scn[0]);
            esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT,
                            ESP_SPP_ROLE_MASTER, param->disc_comp.scn[0], s_target_bda);
        } else {
            ESP_LOGE(TAG, "No SPP service (status=%d)", param->disc_comp.status);
        }
        break;
    case ESP_SPP_OPEN_EVT:
        s_spp_handle = param->open.handle;
        ESP_LOGI(TAG, "BT connected (handle=%lu). Starting poll task...",
                 (unsigned long)s_spp_handle);
        if (!s_poll_task)
            xTaskCreate(poll_task, "elm_poll", 4096, NULL, 5, &s_poll_task);
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGW(TAG, "BT link closed.");
        s_spp_handle = 0;
        break;
    case ESP_SPP_DATA_IND_EVT:
        xStreamBufferSend(s_rx_stream, param->data_ind.data, param->data_ind.len, 0);
        break;
    default:
        break;
    }
}

static void bt_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(esp_bt_gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(esp_spp_cb));
    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = false,       /* adapter is Basic Mode only */
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&spp_cfg));

    esp_bt_gap_set_device_name("MaxusBridge-BT");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);
}

/* ============================== WiFi + HTTP ============================== */

static esp_err_t data_get_handler(httpd_req_t *req)
{
    static char json[4096];
    size_t o = 0;
    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    o += snprintf(json + o, sizeof(json) - o, "{\"bt\":%s,\"seq\":%lu,\"pids\":[",
                  g_bt_connected ? "true" : "false", (unsigned long)g_seq);
    for (size_t i = 0; i < N_PIDS; i++) {
        o += snprintf(json + o, sizeof(json) - o,
                      "%s{\"name\":\"%s\",\"group\":\"%s\",\"text\":\"%s\",\"raw\":\"%s\","
                      "\"valid\":%s,\"test\":%s}",
                      i ? "," : "", PIDS[i].name, PIDS[i].group,
                      g_vals[i].text, g_vals[i].raw,
                      g_vals[i].valid ? "true" : "false", PIDS[i].testing ? "true" : "false");
    }
    xSemaphoreGive(s_snap_mutex);
    o += snprintf(json + o, sizeof(json) - o, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static const char HTML_PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>MAXUS Bridge</title><style>"
    "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:1rem;"
    "padding-bottom:4.5rem}"                        /* room for the fixed tab bar */
    "h2{font-weight:600;text-align:center;font-size:1.1rem}"
    "table{width:100%;max-width:560px;margin:0 auto;border-collapse:collapse}"
    "td{padding:.5rem .4rem;border-bottom:1px solid #262630}"
    ".n{color:#c8c8d0}.v{text-align:right;font-weight:700;color:#4ea1ff;white-space:nowrap}"
    ".r{color:#666;font-size:.75rem;font-family:monospace;text-align:right}"
    ".tag{background:#3a2f00;color:#f0c000;font-size:.6rem;padding:1px 5px;border-radius:6px}"
    ".s{color:#8a8a8a;text-align:center;margin:.8rem}"
    ".dot{display:inline-block;width:11px;height:11px;border-radius:50%;vertical-align:middle}"
    /* bottom tab bar */
    "#tabs{position:fixed;left:0;right:0;bottom:0;display:flex;background:#18181d;"
    "border-top:1px solid #2a2a33}"
    "#tabs button{flex:1;padding:.9rem .2rem;background:none;border:0;color:#8a8a92;"
    "font-size:.85rem;font-weight:600}"
    "#tabs button.on{color:#4ea1ff;box-shadow:inset 0 2px 0 #4ea1ff}"
    "</style></head><body>"
    "<h2>MAXUS 9 EV</h2>"
    "<table id=t></table>"
    "<div class=s>BT <span class=dot id=dot></span> &middot; poll #<span id=seq>0</span></div>"
    "<div id=tabs></div>"
    "<script>"
    "var cur=null,last=[];"
    "function draw(){"
    "var rows=last.filter(function(p){return p.group===cur;});"
    "document.getElementById('t').innerHTML=rows.map(function(p){"
    "return '<tr><td class=n>'+p.name+(p.test?' <span class=tag>test</span>':'')"
    "+'</td><td class=v>'+(p.valid?p.text:'--')+'</td>'"
    "+'<td class=r>'+(p.raw||'')+'</td></tr>';}).join('');}"
    "function tabs(){"
    "var gs=[];last.forEach(function(p){if(gs.indexOf(p.group)<0)gs.push(p.group);});"
    "if(!cur||gs.indexOf(cur)<0)cur=gs[0];"
    "var el=document.getElementById('tabs');"
    "el.innerHTML=gs.map(function(g){"
    "return '<button data-g=\"'+g+'\"'+(g===cur?' class=on':'')+'>'+g+'</button>';}).join('');"
    "Array.prototype.forEach.call(el.children,function(b){"
    "b.onclick=function(){cur=b.getAttribute('data-g');tabs();draw();};});}"
    "async function u(){try{let d=await(await fetch('/data')).json();"
    "document.getElementById('seq').textContent=d.seq;"
    "document.getElementById('dot').style.background=d.bt?'#3c3':'#c33';"
    "last=d.pids;tabs();draw();"
    "}catch(e){}}"
    "setInterval(u,1000);u();"
    "</script></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, HTML_PAGE);
    return ESP_OK;
}

static void http_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t data = {.uri = "/data", .method = HTTP_GET, .handler = data_get_handler};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &data);
    ESP_LOGI(TAG, "HTTP server up: http://192.168.4.1/");
}

static void wifi_ap_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wc = {0};
    strncpy((char *)wc.ap.ssid, AP_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)wc.ap.password, AP_PASS, sizeof(wc.ap.password));
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP up: SSID=%s pass=%s ch=%d", AP_SSID, AP_PASS, AP_CHANNEL);
}

/* ================================ main ================================== */

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    s_snap_mutex = xSemaphoreCreateMutex();
    s_rx_stream = xStreamBufferCreate(512, 1);
    for (size_t i = 0; i < N_PIDS; i++) snprintf(g_vals[i].text, sizeof(g_vals[i].text), "--");

    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, " POC4 (ESP-IDF): full UDS decode (%d PIDs)", (int)N_PIDS);
    ESP_LOGI(TAG, "==============================");

    wifi_ap_init();
    http_start();
    bt_init();
}
