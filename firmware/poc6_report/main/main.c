/*
 * MAXUS 9 EV - ESP32 Bridge - poc6_report
 * Battery inspection report for taladkrub (hub handoff H-0135, hypothesis H7).
 *
 * Built on poc4 (BT Classic -> ELM327, table-driven UDS poller, WiFi dashboard)
 * and adds:
 *   - ISO-TP reassembly in uds.c, so 10-byte multi-frame replies decode
 *     (the 12 BMS module groups, shown on a "Module" dashboard tab);
 *   - /report: pause the dashboard poller, read the report set fresh, bump an
 *     NVS report counter, return JSON (?demo=1 = canned bytes, not counted);
 *   - /report.js: the phone draws the report on a canvas and saves a JPEG.
 *
 * Read-only: Service 22 only, default session, no writes, no security access.
 * Board: classic ESP32 (WROOM-32 / ESP32-D0WD-V3) - needs BT Classic SPP.
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
#include "nvs.h"
#include "driver/gpio.h"
#include "uds.h"
#include "safety.h"

static const char *TAG = "poc6_rep";

#define FW_VERSION     "1.0.0"          /* official name for customer use (H-0138) */
#define REPORT_PROFILE "maxus9"

/* --- WiFi SoftAP config --- */
#define AP_SSID    "MaxusBridge"
#define AP_PASS    "maxus1234"      /* >= 8 chars for WPA2 */
#define AP_CHANNEL 1

/* --- ELM327 target (from Step 2) --- */
#define TARGET_NAME_HINT "OBD"
static const esp_bd_addr_t KNOWN_ELM327_BDA = {0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33};

/* ELM327 init sequence (ported from src/elm327.py:init). Same sequence that
 * produced the 2026-07-16 multi-frame capture: ATH1 + ATS0, CAF left on so the
 * adapter sends ISO-TP flow control itself. */
static const char *INIT_CMDS[] = {
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATAT2", "ATST96", "ATSP6",
};
#define INIT_COUNT (sizeof(INIT_CMDS) / sizeof(INIT_CMDS[0]))

/* Append to a fixed buffer without ever writing past it (snprintf returns the
 * would-be length, so a naive "o += snprintf" can run off the end). */
#define APPEND(buf, o, ...)                                                   \
    do {                                                                      \
        if ((o) < sizeof(buf)) {                                              \
            int _n = snprintf((buf) + (o), sizeof(buf) - (o), __VA_ARGS__);   \
            if (_n > 0) (o) += (size_t)_n;                                    \
            if ((o) > sizeof(buf) - 1) (o) = sizeof(buf) - 1;                 \
        }                                                                     \
    } while (0)

/* ============================ PID table ============================ */
/* Dashboard table - poc4's table unchanged, plus the 12 module groups. */

typedef enum { FMT_NUM, FMT_BMS_STATUS, FMT_GEAR, FMT_BOOL, FMT_MODULE } pid_fmt_t;

typedef struct {
    const char *name;
    const char *group;     /* dashboard tab: "BMS" / "Module" / "Motor" / "Vehicle" */
    const char *header;    /* "7E5" (BMS) or "7E3" (IMCU)                */
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

/* Module group: 7 data bytes, multi-frame. Layout read as {V_a, V_b, T_a, T_b,
 * ?} with the MG formulas V = INT16/2000 + 1.0 and T = byte*0.5 - 40 - unverified
 * (H7), so these stay tagged "test" and never reach the report image. */
#define MODULE(nm, c, d) {nm, "Module", "7E5", c, d, 6, 0.0f, 0.0f, 3, "", FMT_MODULE, true}

/* Ordered by header so the poller only sends ATSH when the header changes. */
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
    /* ---------- BMS 7E5 - module groups (multi-frame, H7) ---------- */
    MODULE("Module 01 B001", "22 B0 01", "B001"),
    MODULE("Module 02 B009", "22 B0 09", "B009"),
    MODULE("Module 03 B011", "22 B0 11", "B011"),
    MODULE("Module 04 B019", "22 B0 19", "B019"),
    MODULE("Module 05 B021", "22 B0 21", "B021"),
    MODULE("Module 06 B029", "22 B0 29", "B029"),
    MODULE("Module 07 B0A3", "22 B0 A3", "B0A3"),
    MODULE("Module 08 B0AB", "22 B0 AB", "B0AB"),
    MODULE("Module 09 B0B3", "22 B0 B3", "B0B3"),
    MODULE("Module 10 B0BB", "22 B0 BB", "B0BB"),
    MODULE("Module 11 B0C3", "22 B0 C3", "B0C3"),
    MODULE("Module 12 B0CB", "22 B0 CB", "B0CB"),
    /* ---------- IMCU 7E3 - motor group ---------- */
    {"Motor Temp",    "Motor", "7E3", "22 B4 05", "B405", 1, 1.0f,  -40.0f, 0, "C", FMT_NUM, true},
    {"VCU Coolant",   "Motor", "7E3", "22 B3 09", "B309", 1, 1.0f,  -40.0f, 0, "C", FMT_NUM, true},
    {"Motor Speed",   "Motor", "7E3", "22 B4 02", "B402", 2, 1.0f, -32767.0f, 0, "rpm", FMT_NUM, true},
    {"Motor Torque",  "Motor", "7E3", "22 B4 01", "B401", 2, 1.0f, -32767.0f, 0, "", FMT_NUM, true},
    /* ---------- IMCU 7E3 - vehicle group ---------- */
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

/* ============================ Report set ============================ */
/* What the report image shows (Design, H-0135). Formulas match src/pids.py.
 * idx_byte: data byte carrying the cell / sensor index (notes 11-12), -1 none. */
typedef struct {
    const char *key;       /* JSON key read by report.js */
    const char *header;
    const char *cmd;
    const char *did;
    uint8_t     nbytes;    /* value bytes, big-endian */
    float       scale;
    float       offset;
    int8_t      idx_byte;
    const char *demo_raw;  /* canned bytes for ?demo=1 (real reads, different days) */
} rep_desc_t;

static const rep_desc_t REPORT[] = {
    {"soh",  "7E5", "22 B0 61", "B061", 2, 0.01f,  0.0f,   -1, "24D6"},
    {"soc",  "7E5", "22 B0 46", "B046", 2, 0.1f,   0.0f,   -1, "00FA"},
    {"cmax", "7E5", "22 B0 58", "B058", 2, 0.001f, 0.0f,    2, "0E1C1A"},
    {"cmin", "7E5", "22 B0 59", "B059", 2, 0.001f, 0.0f,    2, "0E0651"},
    {"thi",  "7E5", "22 B0 56", "B056", 1, 0.5f,  -40.0f,   1, "9406"},
    {"tlo",  "7E5", "22 B0 57", "B057", 1, 0.5f,  -40.0f,   1, "920B"},
    /* Page 2 "measurement conditions". B043 demo bytes are synthetic
     * (0x9C4A -> +0.25 A); the other demo bytes are real reads. */
    {"pv",   "7E5", "22 B0 42", "B042", 2, 0.25f,  0.0f,   -1, "0619"},
    {"cur",  "7E5", "22 B0 43", "B043", 2, 0.025f, -1000.0f, -1, "9C4A"},
    {"bms",  "7E5", "22 B0 48", "B048", 1, 1.0f,   0.0f,   -1, "03"},
    {"odo",  "7E3", "22 E1 01", "E101", 3, 1.0f,   0.0f,   -1, "00F1D7"},
};
#define N_REPORT (sizeof(REPORT) / sizeof(REPORT[0]))

/* Page 2 module groups (multi-frame, formula unverified - H7 b/d). Demo bytes
 * for the last six are real 2026-07-16 ELM327 reads; the first six have no
 * capture, so demo shows them as unread (exercises the missing-value path).
 * The demo modules (3.72 V) and demo cells (3.60 V) come from different days,
 * so they are not expected to agree. */
typedef struct { const char *cmd, *did, *demo_raw; } rep_mod_t;
static const rep_mod_t REPORT_MODS[] = {
    {"22 B0 01", "B001", NULL}, {"22 B0 09", "B009", NULL},
    {"22 B0 11", "B011", NULL}, {"22 B0 19", "B019", NULL},
    {"22 B0 21", "B021", NULL}, {"22 B0 29", "B029", NULL},
    {"22 B0 A3", "B0A3", "154715548C8CA0"}, {"22 B0 AB", "B0AB", "154C15548C8CA9"},
    {"22 B0 B3", "B0B3", "154F15548C8CA8"}, {"22 B0 BB", "B0BB", "154F15578C8C9F"},
    {"22 B0 C3", "B0C3", "154E15538C8CA4"}, {"22 B0 CB", "B0CB", "1545154D8C8CA6"},
};
#define N_REPORT_MODS (sizeof(REPORT_MODS) / sizeof(REPORT_MODS[0]))

typedef struct {
    bool  ok;
    float va, vb, ta, tb;
    char  raw[20];
} rep_mod_val_t;

typedef struct {
    bool  ok;
    float value;
    int   idx;        /* -1 when the DID carries no index or it was absent */
    int   nrc;        /* -1 when none */
    char  raw[40];
} rep_val_t;

/* --- Shared snapshot: one decoded value per PID (mirrors webdash _snapshot) --- */
typedef struct {
    bool valid;
    char raw[40];    /* data bytes as hex, for cross-check */
    char text[28];   /* formatted value, e.g. "400.5 V" or "Running" */
} pid_val_t;

static SemaphoreHandle_t s_snap_mutex;
static pid_val_t g_vals[N_PIDS];
static bool      g_bt_connected;
static uint32_t  g_seq;

/* ELM327 access: the dashboard poller and /report share one adapter. Whoever
 * holds s_elm_mutex owns the serial link and the cached ATSH header. */
static SemaphoreHandle_t s_elm_mutex;
static char              s_cur_hdr[8];
static volatile bool     s_elm_ready;   /* init sequence done on this link */

/* ============================ Status LED ============================ */
/* Onboard LED (DevKit "D2" = GPIO2). GPIO2 is a strapping pin, but it is only
 * driven after boot, so download mode is unaffected. Lets the user read the
 * device state at the car without opening the phone. */
#define LED_GPIO GPIO_NUM_2

typedef enum {
    LED_SEARCHING,    /* no ELM327 link yet          : slow blink, 1 Hz       */
    LED_NO_ANSWER,    /* ELM327 up, car answers none : double blip every 1 s  */
    LED_OK,           /* polling with live values    : solid on               */
    LED_REPORTING,    /* /report reading the car     : fast blink, 5 Hz       */
} led_state_t;

static volatile led_state_t s_led_state = LED_SEARCHING;
static volatile bool        s_led_done;   /* report issued: 3 quick blinks */

static void led_task(void *arg)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    uint32_t t = 0;                                   /* 50 ms ticks */
    for (;;) {
        if (s_led_done) {
            s_led_done = false;
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
            for (int i = 0; i < 3; i++) {
                gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(150));
                gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(150));
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            t = 0;
            continue;
        }
        int on, p = (int)(t % 20);
        switch (s_led_state) {
        case LED_SEARCHING: on = p < 10;                          break;
        case LED_NO_ANSWER: on = p < 2 || (p >= 4 && p < 6);      break;
        case LED_REPORTING: on = (t % 4) < 2;                     break;
        default:            on = 1;                               break;
        }
        gpio_set_level(LED_GPIO, on);
        vTaskDelay(pdMS_TO_TICKS(50));
        t++;
    }
}

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
 * (printable ASCII; non-printable -> space, drop '>', trim). CR between frames
 * becomes a space, which is the frame separator uds_parse() expects. Ported
 * from src/elm327.py:_send_raw + the poc2/poc3 sanitizer. Caller holds
 * s_elm_mutex. */
static bool elm_cmd(const char *cmd, char *clean_out, size_t clean_sz, uint32_t timeout_ms)
{
    clean_out[0] = '\0';
    /* The single gate to the car: nothing outside the read-only allowlist
     * is ever written to the adapter (docs/inspection-safety.md). */
    if (!safety_cmd_allowed(cmd)) {
        ESP_LOGE(TAG, "BLOCKED command (not on the read-only allowlist): \"%s\"", cmd);
        return false;
    }
    if (!s_spp_handle) return false;
    xStreamBufferReset(s_rx_stream);

    char out[48];
    int n = snprintf(out, sizeof(out), "%s\r", cmd);
    esp_spp_write((uint32_t)s_spp_handle, n, (uint8_t *)out);

    char raw[320];
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

/* Send a Service 22 request to `header`, switching ATSH only when needed.
 * Caller holds s_elm_mutex. */
static uds_result_t elm_read(const char *header, const char *cmd)
{
    char buf[256];
    if (strcmp(s_cur_hdr, header) != 0) {
        char h[16];
        snprintf(h, sizeof(h), "ATSH %s", header);
        elm_cmd(h, buf, sizeof(buf), 2000);
        strncpy(s_cur_hdr, header, sizeof(s_cur_hdr) - 1);
    }
    elm_cmd(cmd, buf, sizeof(buf), 2000);
    uds_result_t r = uds_parse(buf);
    /* No VIN may be stored or shown, even from a DID we did not expect to
     * carry one (ECU 718 B019 does, note 11). */
    int masked = safety_scrub_vin(r.data, r.n);
    if (masked) ESP_LOGW(TAG, "VIN-like data masked in reply to %s %s", header, cmd);
    return r;
}

/* Format the decoded data bytes of a result as hex, into out. */
static void data_hex(const uds_result_t *r, char *out, size_t sz)
{
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < r->n && o + 2 < sz; i++)
        o += snprintf(out + o, sz - o, "%02X", r->data[i]);
}

static int be_int16(const uint8_t *d) { return d[0] * 256 + d[1]; }

/* Render one dashboard PID value into text. */
static void format_pid(const pid_desc_t *p, const uds_result_t *r, char *text, size_t sz)
{
    uint32_t raw = 0;
    for (int b = 0; b < p->nbytes && b < 4; b++) raw = (raw << 8) | r->data[b];

    switch (p->fmt) {
    case FMT_BMS_STATUS:
        snprintf(text, sz, "%s (%02X)", b048_enum((int)raw), (unsigned)raw);
        break;
    case FMT_GEAR:
        snprintf(text, sz, "%s", gear_name(raw));
        break;
    case FMT_BOOL:
        snprintf(text, sz, "%s", (raw & 1u) ? "Yes" : "No");
        break;
    case FMT_MODULE:
        snprintf(text, sz, "%.3f/%.3f V %.1f/%.1f C",
                 be_int16(r->data) / 2000.0f + 1.0f, be_int16(r->data + 2) / 2000.0f + 1.0f,
                 r->data[4] * 0.5f - 40.0f, r->data[5] * 0.5f - 40.0f);
        break;
    default:
        snprintf(text, sz, "%.*f %s", p->dec, raw * p->scale + p->offset, p->unit);
        break;
    }
}

/* Poll task: init once, then loop the PID table, decoding each into g_vals. */
static void poll_task(void *arg)
{
    char buf[128];
    xSemaphoreTake(s_elm_mutex, portMAX_DELAY);
    s_cur_hdr[0] = '\0';                      /* new link: no header set yet */
    for (size_t i = 0; i < INIT_COUNT; i++) {
        uint32_t to = (strcmp(INIT_CMDS[i], "ATZ") == 0) ? 5000 : 2000;
        bool ok = elm_cmd(INIT_CMDS[i], buf, sizeof(buf), to);
        ESP_LOGI(TAG, "init %-8s -> %s%s", INIT_CMDS[i], buf, ok ? "" : "  [timeout]");
    }
    s_elm_ready = true;
    xSemaphoreGive(s_elm_mutex);
    ESP_LOGI(TAG, "Init done. Polling %d PIDs...", (int)N_PIDS);

    static pid_val_t local[N_PIDS];           /* static: keeps the task stack small */
    while (s_spp_handle) {
        for (size_t i = 0; i < N_PIDS; i++) {
            const pid_desc_t *p = &PIDS[i];
            /* Lock per PID, so a /report request waits at most one read. */
            xSemaphoreTake(s_elm_mutex, portMAX_DELAY);
            uds_result_t r = elm_read(p->header, p->cmd);
            xSemaphoreGive(s_elm_mutex);

            /* Need a positive response for the DID we asked for, with enough
             * data bytes for this descriptor. */
            bool ok = r.positive && strcmp(r.did, p->did) == 0 && r.n >= p->nbytes;
            local[i].valid = ok;
            data_hex(&r, local[i].raw, sizeof(local[i].raw));
            if (ok) format_pid(p, &r, local[i].text, sizeof(local[i].text));
            else    snprintf(local[i].text, sizeof(local[i].text), "--");
        }

        xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
        memcpy(g_vals, local, sizeof(g_vals));
        g_bt_connected = true;
        g_seq++;
        uint32_t seq = g_seq;
        xSemaphoreGive(s_snap_mutex);

        int ok_count = 0;
        for (size_t i = 0; i < N_PIDS; i++) if (local[i].valid) ok_count++;
        s_led_state = ok_count ? LED_OK : LED_NO_ANSWER;
        ESP_LOGI(TAG, "[poll #%lu] %d/%d ok | PackV=%s SOCraw=%s | heap=%lu",
                 (unsigned long)seq, ok_count, (int)N_PIDS,
                 local[1].text, local[8].text,
                 (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_elm_ready = false;
    s_led_state = LED_SEARCHING;
    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    g_bt_connected = false;
    xSemaphoreGive(s_snap_mutex);
    ESP_LOGW(TAG, "poll_task exiting (BT link closed)");
    s_poll_task = NULL;
    vTaskDelete(NULL);
}

/* ============================ Report ============================ */

/* Decode one report value from a parsed reply. */
static void report_decode(const rep_desc_t *d, const uds_result_t *r, rep_val_t *v)
{
    memset(v, 0, sizeof(*v));
    v->idx = -1;
    v->nrc = r->nrc_code;
    data_hex(r, v->raw, sizeof(v->raw));
    if (!(r->positive && strcmp(r->did, d->did) == 0 && r->n >= d->nbytes)) return;

    uint32_t raw = 0;
    for (int b = 0; b < d->nbytes; b++) raw = (raw << 8) | r->data[b];
    v->ok = true;
    v->value = raw * d->scale + d->offset;
    if (d->idx_byte >= 0 && r->n > d->idx_byte) v->idx = r->data[d->idx_byte];
}

/* Turn canned hex into a parsed-looking result (demo mode). NULL = no reply. */
static uds_result_t demo_result(const char *did, const char *hex)
{
    uds_result_t r = {0};
    r.nrc_code = -1;
    r.nrc_name = "";
    if (!hex) return r;
    r.positive = true;
    memcpy(r.did, did, 4);
    for (const char *p = hex; p[0] && p[1] && r.n < UDS_MAX_DATA; p += 2) {
        char h[3] = {p[0], p[1], 0};
        r.data[r.n++] = (uint8_t)strtol(h, NULL, 16);
    }
    return r;
}

/* Decode one module group: {V_a, V_b, T_a, T_b, ?}, MG formulas (unverified). */
static void report_decode_mod(const rep_mod_t *m, const uds_result_t *r, rep_mod_val_t *v)
{
    memset(v, 0, sizeof(*v));
    data_hex(r, v->raw, sizeof(v->raw));
    if (!(r->positive && strcmp(r->did, m->did) == 0 && r->n >= 6)) return;
    v->ok = true;
    v->va = be_int16(r->data) / 2000.0f + 1.0f;
    v->vb = be_int16(r->data + 2) / 2000.0f + 1.0f;
    v->ta = r->data[4] * 0.5f - 40.0f;
    v->tb = r->data[5] * 0.5f - 40.0f;
}

/* Next report number from NVS. Survives power cycles and never resets, so a
 * number is never issued twice by this device. Returns 0 on NVS failure. */
static uint32_t report_counter_next(void)
{
    nvs_handle_t h;
    uint32_t seq = 0;
    if (nvs_open("poc6", NVS_READWRITE, &h) != ESP_OK) return 0;
    nvs_get_u32(h, "rep_seq", &seq);          /* absent on first use -> 0 */
    seq++;
    if (nvs_set_u32(h, "rep_seq", seq) != ESP_OK || nvs_commit(h) != ESP_OK) seq = 0;
    nvs_close(h);
    return seq;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t report_get_handler(httpd_req_t *req)
{
    bool demo = false;
    char q[32], v[4];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "demo", v, sizeof(v)) == ESP_OK && v[0] == '1')
        demo = true;

    static rep_val_t     vals[N_REPORT];      /* static: httpd stack stays small */
    static rep_mod_val_t mods[N_REPORT_MODS];
    int ok_count = 0;

    if (demo) {
        for (size_t i = 0; i < N_REPORT; i++) {
            uds_result_t r = demo_result(REPORT[i].did, REPORT[i].demo_raw);
            report_decode(&REPORT[i], &r, &vals[i]);
        }
        for (size_t i = 0; i < N_REPORT_MODS; i++) {
            uds_result_t r = demo_result(REPORT_MODS[i].did, REPORT_MODS[i].demo_raw);
            report_decode_mod(&REPORT_MODS[i], &r, &mods[i]);
        }
        ok_count = (int)N_REPORT;
    } else {
        if (!s_elm_ready) return send_json(req, "{\"ok\":false,\"err\":\"bt\"}");
        /* Take the adapter for the whole report so all values come from one
         * moment; the dashboard poller waits (at most one read in flight). */
        if (xSemaphoreTake(s_elm_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
            return send_json(req, "{\"ok\":false,\"err\":\"busy\"}");
        led_state_t led_before = s_led_state;
        s_led_state = LED_REPORTING;
        for (size_t i = 0; i < N_REPORT; i++) {
            for (int attempt = 0; attempt < 2; attempt++) {   /* one retry */
                uds_result_t r = elm_read(REPORT[i].header, REPORT[i].cmd);
                report_decode(&REPORT[i], &r, &vals[i]);
                if (vals[i].ok) break;
            }
            ESP_LOGI(TAG, "report %-4s %s raw=%s", REPORT[i].key,
                     vals[i].ok ? "ok  " : "FAIL", vals[i].raw);
            if (vals[i].ok) ok_count++;
        }
        for (size_t i = 0; i < N_REPORT_MODS; i++) {
            for (int attempt = 0; attempt < 2; attempt++) {   /* one retry */
                uds_result_t r = elm_read("7E5", REPORT_MODS[i].cmd);
                report_decode_mod(&REPORT_MODS[i], &r, &mods[i]);
                if (mods[i].ok) break;
            }
            ESP_LOGI(TAG, "report mod %s %s raw=%s", REPORT_MODS[i].did,
                     mods[i].ok ? "ok  " : "FAIL", mods[i].raw);
        }
        s_led_state = led_before;
        xSemaphoreGive(s_elm_mutex);
        if (ok_count == 0) return send_json(req, "{\"ok\":false,\"err\":\"novalues\"}");
    }

    /* Only a real report with at least one value consumes a number. */
    uint32_t seq = demo ? 0 : report_counter_next();
    if (!demo && seq == 0) return send_json(req, "{\"ok\":false,\"err\":\"nvs\"}");
    if (!demo) s_led_done = true;            /* 3 quick blinks: report issued */

    static char json[4096];
    size_t o = 0;
    APPEND(json, o, "{\"ok\":true,\"demo\":%s,\"seq\":%lu,\"fw\":\"%s\",\"profile\":\"%s\",\"v\":{",
           demo ? "true" : "false", (unsigned long)seq, FW_VERSION, REPORT_PROFILE);
    for (size_t i = 0; i < N_REPORT; i++) {
        const rep_val_t *x = &vals[i];
        APPEND(json, o, "%s\"%s\":{\"ok\":%s,\"v\":%.3f,\"i\":%d,\"nrc\":%d,\"raw\":\"%s\"}",
               i ? "," : "", REPORT[i].key, x->ok ? "true" : "false",
               x->ok ? x->value : 0.0f, x->idx, x->nrc, x->raw);
    }
    APPEND(json, o, "},\"mod\":[");
    for (size_t i = 0; i < N_REPORT_MODS; i++) {
        const rep_mod_val_t *m = &mods[i];
        APPEND(json, o, "%s{\"did\":\"%s\",\"ok\":%s,\"va\":%.4f,\"vb\":%.4f,\"ta\":%.1f,"
               "\"tb\":%.1f,\"raw\":\"%s\"}",
               i ? "," : "", REPORT_MODS[i].did, m->ok ? "true" : "false",
               m->va, m->vb, m->ta, m->tb, m->raw);
    }
    APPEND(json, o, "]}");
    ESP_LOGI(TAG, "report %s seq=%lu: %d/%d values", demo ? "DEMO" : "real",
             (unsigned long)seq, ok_count, (int)N_REPORT);
    return send_json(req, json);
}

/* ============================== BT callbacks ============================== */

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
            xTaskCreate(poll_task, "elm_poll", 6144, NULL, 5, &s_poll_task);
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGW(TAG, "BT link closed.");
        s_elm_ready = false;
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
    static char json[8192];                  /* ~41 PIDs x ~170 chars */
    size_t o = 0;
    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    APPEND(json, o, "{\"bt\":%s,\"seq\":%lu,\"pids\":[",
           g_bt_connected ? "true" : "false", (unsigned long)g_seq);
    for (size_t i = 0; i < N_PIDS; i++) {
        APPEND(json, o,
               "%s{\"name\":\"%s\",\"group\":\"%s\",\"text\":\"%s\",\"raw\":\"%s\","
               "\"valid\":%s,\"test\":%s}",
               i ? "," : "", PIDS[i].name, PIDS[i].group,
               g_vals[i].text, g_vals[i].raw,
               g_vals[i].valid ? "true" : "false", PIDS[i].testing ? "true" : "false");
    }
    xSemaphoreGive(s_snap_mutex);
    APPEND(json, o, "]}");
    return send_json(req, json);
}

/* Dashboard page: poc4's tabbed table plus a "รายงาน" (report) tab. The report
 * drawing lives in report.js (embedded file) to keep this string small. */
static const char HTML_PAGE[] =
    "<!doctype html><html lang=th><head><meta charset=utf-8>"
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
    "#rep{max-width:560px;margin:0 auto;text-align:center}"
    ".btn{display:inline-block;margin:.4rem;padding:.8rem 1.4rem;border:0;border-radius:10px;"
    "background:#4ea1ff;color:#fff;font-size:1rem;font-weight:700;text-decoration:none}"
    ".btn:disabled{opacity:.5}"
    ".lnk{background:none;border:0;color:#8ab4ff;text-decoration:underline;font-size:.9rem}"
    "#rout img{width:100%;margin:.6rem 0 1.4rem;border-radius:6px}"
    /* bottom tab bar */
    "#tabs{position:fixed;left:0;right:0;bottom:0;display:flex;background:#18181d;"
    "border-top:1px solid #2a2a33}"
    "#tabs button{flex:1;padding:.9rem .2rem;background:none;border:0;color:#8a8a92;"
    "font-size:.85rem;font-weight:600}"
    "#tabs button.on{color:#4ea1ff;box-shadow:inset 0 2px 0 #4ea1ff}"
    "</style></head><body>"
    "<h2>MAXUS 9 EV</h2>"
    "<table id=t></table>"
    "<div id=rep style=display:none>"
    "<p class=s>ให้รถอยู่ในสถานะ READY จอดนิ่ง และปิดแอร์ก่อนกดสร้างรายงาน</p>"
    "<button id=rgo class=btn>สร้างรายงาน</button><br>"
    "<button id=rdemo class=lnk>ตัวอย่าง (ไม่ต้องต่อรถ)</button>"
    "<p id=rst class=s></p>"
    "<div id=rout></div>"
    "</div>"
    "<div class=s>BT <span class=dot id=dot></span> &middot; poll #<span id=seq>0</span></div>"
    "<div id=tabs></div>"
    "<script>"
    "var REP='รายงาน',cur=null,last=[];"
    "function draw(){"
    "var rep=cur===REP;"
    "document.getElementById('t').style.display=rep?'none':'';"
    "document.getElementById('rep').style.display=rep?'block':'none';"
    "if(rep)return;"
    "var rows=last.filter(function(p){return p.group===cur;});"
    "document.getElementById('t').innerHTML=rows.map(function(p){"
    "return '<tr><td class=n>'+p.name+(p.test?' <span class=tag>test</span>':'')"
    "+'</td><td class=v>'+(p.valid?p.text:'--')+'</td>'"
    "+'<td class=r>'+(p.raw||'')+'</td></tr>';}).join('');}"
    "function tabs(){"
    "var gs=[];last.forEach(function(p){if(gs.indexOf(p.group)<0)gs.push(p.group);});"
    "gs.push(REP);"
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
    "</script><script src=/report.js></script></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, HTML_PAGE);
}

/* report.js is embedded by CMake (EMBED_TXTFILES, NUL-terminated). */
extern const char report_js_start[] asm("_binary_report_js_start");

static esp_err_t reportjs_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    return httpd_resp_sendstr(req, report_js_start);
}

static void http_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;               /* /report reads the ELM327 inline */
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t root  = {.uri = "/",          .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t data  = {.uri = "/data",      .method = HTTP_GET, .handler = data_get_handler};
    httpd_uri_t rep   = {.uri = "/report",    .method = HTTP_GET, .handler = report_get_handler};
    httpd_uri_t repjs = {.uri = "/report.js", .method = HTTP_GET, .handler = reportjs_get_handler};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &data);
    httpd_register_uri_handler(server, &rep);
    httpd_register_uri_handler(server, &repjs);
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

    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, " %s (%d PIDs, %d report values)", FW_VERSION, (int)N_PIDS, (int)N_REPORT);
    ESP_LOGI(TAG, "==============================");
    int uds_fails = uds_selftest();
    int safety_fails = safety_selftest();
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);

    s_snap_mutex = xSemaphoreCreateMutex();
    s_elm_mutex = xSemaphoreCreateMutex();
    s_rx_stream = xStreamBufferCreate(1024, 1);
    for (size_t i = 0; i < N_PIDS; i++) snprintf(g_vals[i].text, sizeof(g_vals[i].text), "--");

    wifi_ap_init();
    http_start();
    /* Fail-closed: if the safety gate or the parser does not pass its own
     * self-test, never connect to the car. WiFi stays up so the state is
     * visible (BT dot stays red, LED keeps the slow "searching" blink). */
    if (uds_fails || safety_fails) {
        ESP_LOGE(TAG, "self-test failed (uds %d, safety %d): NOT connecting to the ELM327",
                 uds_fails, safety_fails);
        return;
    }
    bt_init();
}
