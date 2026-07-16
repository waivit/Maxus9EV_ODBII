/*
 * MAXUS 9 EV - ESP32 Bridge (Phase 2)
 * Step 3 POC (ESP-IDF) - COEXISTENCE: run BT Classic (polling the ELM327) and
 * WiFi SoftAP + HTTP server at the same time on the single 2.4 GHz radio. A
 * phone joins the ESP32's WiFi and watches Pack Voltage update live while BT
 * keeps polling. This is the #1 feasibility gate for the whole bridge.
 *
 * BT bring-up is carried over verbatim from Step 2 (firmware/poc2_idf_spp):
 * BR/EDR-only controller, legacy PIN 1234, MAC match, L2CAP Basic Mode.
 * New here: the SPP RX callback feeds a FreeRTOS stream buffer; a poll task
 * runs the AT init once then polls one PID on a loop, updating a mutex-guarded
 * snapshot (mirrors src/webdash.py poller_loop + _snapshot). HTTP handlers only
 * read the snapshot.
 *
 * Read-only: only Service 22 reads, never writes to the vehicle.
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

static const char *TAG = "poc3_coex";

/* --- WiFi SoftAP config --- */
#define AP_SSID    "MaxusBridge"
#define AP_PASS    "maxus1234"      /* >= 8 chars for WPA2 */
#define AP_CHANNEL 1

/* --- ELM327 target (from Step 2) --- */
#define TARGET_NAME_HINT "OBD"
static const esp_bd_addr_t KNOWN_ELM327_BDA = {0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33};

/* ELM327 init sequence (ported from src/elm327.py:init) - run once at connect.
 * ATSH 7E5 targets the BMS; all polled PIDs below live on 7E5. */
static const char *INIT_CMDS[] = {
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATAT2", "ATST96", "ATSP6", "ATSH 7E5",
};
#define INIT_COUNT (sizeof(INIT_CMDS) / sizeof(INIT_CMDS[0]))

/* Polled PIDs (all header 7E5). Confirmed: Pack V (B042), Batt Temp (B056).
 * TESTING candidates from docs/findings/07-mg-ev-reference.md: SOC (B046),
 * Coolant (B05C) - shown on the dashboard but NOT yet verified on the car. */

/* --- BT discovery/connect state --- */
static bool s_target_found = false;
static esp_bd_addr_t s_target_bda;
static volatile uint32_t s_spp_handle = 0;      /* 0 = not connected */
static StreamBufferHandle_t s_rx_stream;        /* SPP RX bytes -> poll task */
static TaskHandle_t s_poll_task = NULL;

/* --- Shared snapshot (mirrors webdash.py _snapshot under _lock) --- */
static SemaphoreHandle_t s_snap_mutex;
static struct {
    float    pack_v;    bool v_pack;   /* B042, confirmed */
    float    soc;       bool v_soc;    /* B046, TESTING   */
    float    batt_temp; bool v_temp;   /* B056, confirmed */
    float    coolant;   bool v_cool;   /* B05C, TESTING   */
    bool     bt_connected;
    uint32_t seq;
} g_snap;

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

/* Send one AT command, read the reply until the '>' prompt or timeout, and
 * return it sanitized to printable ASCII (mirrors elm327.py:_send_raw:
 * read-until-'>'-or-timeout, then .replace(">","").strip()). The ELM327 emits
 * non-UTF8 noise while it resets after ATZ - map non-printable -> space so the
 * log/JSON stay clean. Returns true if the prompt was seen. */
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
        if (c == '>') continue;                          /* drop prompt */
        clean_out[ci++] = (c >= 0x20 && c <= 0x7e) ? (char)c : ' ';
    }
    clean_out[ci] = '\0';
    char *s = clean_out;                                 /* trim */
    while (*s == ' ') s++;
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = '\0';
    if (s != clean_out) memmove(clean_out, s, strlen(s) + 1);
    return got_prompt;
}

/* Read one PID and decode. Reply (ATH1/ATS0) looks like "7ED0562B042vvvv":
 * find the positive-response marker (e.g. "62B042"), then take `nbytes` bytes
 * (2 or 4 hex chars) as an unsigned int and apply value*scale+offset. Minimal
 * inline parse; the full uds.py port is Step 4. Returns false on timeout / no
 * marker / short reply (e.g. NO DATA / SEARCHING). */
static bool read_pid(const char *cmd, const char *marker, int nbytes,
                     float scale, float offset, float *out)
{
    char buf[128];
    if (!elm_cmd(cmd, buf, sizeof(buf), 2000)) return false;
    const char *p = strstr(buf, marker);
    if (!p) return false;
    p += strlen(marker);
    int hexchars = nbytes * 2;
    if ((int)strlen(p) < hexchars) return false;
    char h[9];
    memcpy(h, p, hexchars);
    h[hexchars] = '\0';
    *out = (float)strtol(h, NULL, 16) * scale + offset;
    return true;
}

/* Poll task: init the adapter once, then poll all dashboard PIDs on a loop,
 * updating the shared snapshot each cycle (mirrors webdash.py:poller_loop). */
static void poll_task(void *arg)
{
    char buf[128];
    for (size_t i = 0; i < INIT_COUNT; i++) {
        uint32_t to = (strcmp(INIT_CMDS[i], "ATZ") == 0) ? 5000 : 2000;
        bool ok = elm_cmd(INIT_CMDS[i], buf, sizeof(buf), to);
        ESP_LOGI(TAG, "init %-8s -> %s%s", INIT_CMDS[i], buf, ok ? "" : "  [timeout]");
    }
    ESP_LOGI(TAG, "Init done. Polling Pack V / SOC / Batt Temp / Coolant every 1.5 s...");

    while (s_spp_handle) {
        float pv = 0, soc = 0, tb = 0, tc = 0;
        bool v_pv = read_pid("22 B0 42", "62B042", 2, 0.25f,  0.0f, &pv);   /* V   */
        bool v_soc = read_pid("22 B0 46", "62B046", 2, 0.1f,   0.0f, &soc);  /* %   */
        bool v_tb = read_pid("22 B0 56", "62B056", 1, 0.5f, -40.0f, &tb);   /* C   */
        bool v_tc = read_pid("22 B0 5C", "62B05C", 1, 0.5f, -40.0f, &tc);   /* C   */

        xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
        g_snap.pack_v = pv;     g_snap.v_pack = v_pv;
        g_snap.soc = soc;       g_snap.v_soc = v_soc;
        g_snap.batt_temp = tb;  g_snap.v_temp = v_tb;
        g_snap.coolant = tc;    g_snap.v_cool = v_tc;
        g_snap.bt_connected = true;
        g_snap.seq++;
        uint32_t seq = g_snap.seq;
        xSemaphoreGive(s_snap_mutex);

        ESP_LOGI(TAG, "[poll #%lu] Vpack=%.1f(%d) SOC=%.1f(%d) Tbat=%.1f(%d) Tcool=%.1f(%d) heap=%lu",
                 (unsigned long)seq, pv, v_pv, soc, v_soc, tb, v_tb, tc, v_tc,
                 (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    g_snap.bt_connected = false;
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
            /* AUTHENTICATE|ENCRYPT forces the legacy PIN pairing flow; the
             * adapter is Basic-Mode only (ERTM disabled at init). */
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
        s_spp_handle = 0;                 /* poll_task loop sees this and exits */
        break;
    case ESP_SPP_DATA_IND_EVT:
        /* Thin producer: hand raw bytes to the poll task via the stream buffer. */
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
    char json[320];
    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    snprintf(json, sizeof(json),
             "{\"pack_v\":%.1f,\"v_pack\":%s,"
             "\"soc\":%.1f,\"v_soc\":%s,"
             "\"batt_temp\":%.1f,\"v_temp\":%s,"
             "\"coolant\":%.1f,\"v_cool\":%s,"
             "\"bt\":%s,\"seq\":%lu}",
             g_snap.pack_v, g_snap.v_pack ? "true" : "false",
             g_snap.soc, g_snap.v_soc ? "true" : "false",
             g_snap.batt_temp, g_snap.v_temp ? "true" : "false",
             g_snap.coolant, g_snap.v_cool ? "true" : "false",
             g_snap.bt_connected ? "true" : "false", (unsigned long)g_snap.seq);
    xSemaphoreGive(s_snap_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static const char HTML_PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>MAXUS Bridge</title><style>"
    "body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:1.2rem}"
    "h2{font-weight:600;margin:.3rem 0 1rem}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:.8rem;max-width:520px;margin:0 auto}"
    ".tile{background:#1b1b1f;border-radius:14px;padding:1rem .6rem}"
    ".lbl{color:#9a9aa2;font-size:.85rem;margin-bottom:.4rem}"
    ".val{font-size:2.1rem;font-weight:700;color:#4ea1ff}"
    ".u{font-size:1rem;color:#9a9aa2;font-weight:400}"
    ".tag{background:#3a2f00;color:#f0c000;font-size:.6rem;padding:1px 5px;border-radius:6px;vertical-align:middle}"
    ".s{color:#8a8a8a;margin:1rem .3rem .2rem}"
    ".dot{display:inline-block;width:11px;height:11px;border-radius:50%;vertical-align:middle}"
    "</style></head><body>"
    "<h2>MAXUS 9 EV</h2>"
    "<div class=grid>"
    "<div class=tile><div class=lbl>Pack Voltage</div><div class=val id=pv>--</div></div>"
    "<div class=tile><div class=lbl>SOC <span class=tag>test</span></div><div class=val id=soc>--</div></div>"
    "<div class=tile><div class=lbl>Batt Temp</div><div class=val id=tb>--</div></div>"
    "<div class=tile><div class=lbl>Coolant <span class=tag>test</span></div><div class=val id=tc>--</div></div>"
    "</div>"
    "<div class=s>BT <span class=dot id=dot></span> &middot; poll #<span id=seq>0</span></div>"
    "<script>"
    "function set(id,ok,v,u){document.getElementById(id).innerHTML="
    "ok?v.toFixed(1)+' <span class=u>'+u+'</span>':'--';}"
    "async function u(){try{let d=await(await fetch('/data')).json();"
    "set('pv',d.v_pack,d.pack_v,'V');"
    "set('soc',d.v_soc,d.soc,'%');"
    "set('tb',d.v_temp,d.batt_temp,'\\u00b0C');"
    "set('tc',d.v_cool,d.coolant,'\\u00b0C');"
    "document.getElementById('seq').textContent=d.seq;"
    "document.getElementById('dot').style.background=d.bt?'#3c3':'#c33';"
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

    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, " POC3 (ESP-IDF): BT poll + WiFi serve (coexistence)");
    ESP_LOGI(TAG, "==============================");

    wifi_ap_init();   /* WiFi first, then BT - coexistence layer arbitrates */
    http_start();
    bt_init();        /* scan/connect kicked off from ESP_SPP_INIT_EVT */
}
