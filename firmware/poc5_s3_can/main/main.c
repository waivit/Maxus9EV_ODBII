/*
 * MAXUS 9 EV - Phase 3 Step 1 (ESP-IDF) - native CAN board bring-up.
 * Board: Waveshare ESP32-S3-RS485-CAN (ESP32-S3, TWAI on GPIO15/16).
 *
 * Goal of this POC: prove the board actually receives vehicle CAN traffic -
 * correct GPIOs, working transceiver, correct bit timing - BEFORE anything is
 * ever transmitted onto the bus. Nothing is decoded here; UDS/ISO-TP is Step 2/3.
 *
 * SAFETY: the default build runs TWAI_MODE_LISTEN_ONLY, which cannot ACK or
 * transmit, so the vehicle bus physically cannot be disturbed. The self-test
 * build (SELFTEST_MODE 1) DOES transmit and is for the desk only - it is a
 * compile-time switch precisely so it cannot be entered by accident in the car.
 *
 * Output is read over USB serial (idf.py monitor); the WiFi dashboard is Step 5.
 * See docs/phase3-native-can-roadmap.md.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mdns.h"
#include "wifi_creds.h"

static const char *TAG = "poc5_can";

/* ======================== Build-time configuration ======================== */

/* Board wiring (Waveshare ESP32-S3-RS485-CAN). */
#define CAN_TX_GPIO   GPIO_NUM_15
#define CAN_RX_GPIO   GPIO_NUM_16

/* Run mode, chosen at compile time so a transmitting mode can never be entered
 * by accident on a vehicle. */
#define MODE_LISTEN    0   /* LISTEN_ONLY sniffer - cannot transmit or ACK.     */
#define MODE_SELFTEST  1   /* Desk loopback: NO_ACK + self-reception. Transmits! */
                           /* Desk only; needs the 120 ohm jumper ON so the      */
                           /* transceiver echoes the driven bus back.            */
#define MODE_ACTIVE    2   /* NORMAL mode: sends Service 22 requests and reads   */
                           /* the reply. Transmits on the vehicle bus (read-only */
                           /* UDS - exactly what the ELM327 path has always done)*/
#define MODE_SCAN      3   /* Discover responding ECUs, then sweep DID ranges.   */
#define MODE_DASH      4   /* Poll the PID table and serve a web dashboard over  */
                           /* WiFi (APSTA: joins the router AND runs its own AP).*/

#define RUN_MODE MODE_DASH

/* Active-mode request target. Same addressing the ELM327 path uses: BMS request
 * on 7E5, response on 7ED (docs/findings/01-confirmed-pids.md). B042 = Pack
 * Voltage, INT16 * 0.25 - a confirmed PID with a known plausible value (~400 V),
 * which makes it a good end-to-end proof of the native-CAN transport. */
#define UDS_REQ_ID    0x7E5
#define UDS_RESP_ID   0x7ED
#define UDS_PAD       0x00   /* ELM327 pads with 0x00 and this car accepts it   */

#define MAX_IDS        128    /* unique CAN ids tracked                        */
#define RAW_DUMP_N      20    /* first N frames logged verbatim as evidence    */
#define REPORT_MS     5000    /* report block interval                         */
#define PROBE_MS      5000    /* per-bitrate listen window during auto-probe   */
#define RX_QUEUE_LEN    64    /* default 5 is too small for a busy vehicle bus */

/* Bitrate auto-probe order. 500k first - MAXUS OBD is ISO 15765-4 11-bit 500k
 * (that is what ATSP6 selected on the ELM327 path). Probing is safe because it
 * happens in listen-only mode. */
typedef struct {
    const char          *name;
    twai_timing_config_t timing;
} bitrate_opt_t;

static const bitrate_opt_t BITRATES[] = {
    { "500k", TWAI_TIMING_CONFIG_500KBITS() },
    { "250k", TWAI_TIMING_CONFIG_250KBITS() },
    { "125k", TWAI_TIMING_CONFIG_125KBITS() },
    { "1M",   TWAI_TIMING_CONFIG_1MBITS()   },
};
#define N_BITRATES (sizeof(BITRATES) / sizeof(BITRATES[0]))

/* ============================ Shared ID table ============================= */
/* rx_task writes, report_task reads - guarded by a mutex, mirroring the
 * snapshot pattern in firmware/poc4_idf_decode/main/main.c. */

typedef struct {
    uint32_t id;            /* CAN identifier                                 */
    uint32_t count;         /* frames seen since boot                         */
    uint32_t prev_count;    /* count at the previous report (for the rate)    */
    uint8_t  dlc;           /* data length of the most recent frame           */
    uint8_t  data[8];       /* payload of the most recent frame               */
    bool     extd;          /* 29-bit identifier                              */
} can_id_stat_t;

static SemaphoreHandle_t s_tbl_mutex;
static can_id_stat_t     s_tbl[MAX_IDS];
static size_t            s_tbl_n;
static uint32_t          s_total_rx;
static uint32_t          s_dropped_ids;   /* frames whose id did not fit MAX_IDS */
static const char       *s_locked_rate = "?";

/* Record one frame into the table. Caller must hold the mutex. */
static void table_add(const twai_message_t *m)
{
    s_total_rx++;
    for (size_t i = 0; i < s_tbl_n; i++) {
        if (s_tbl[i].id == m->identifier && s_tbl[i].extd == (bool)m->extd) {
            s_tbl[i].count++;
            s_tbl[i].dlc = m->data_length_code;
            memcpy(s_tbl[i].data, m->data, 8);
            return;
        }
    }
    if (s_tbl_n >= MAX_IDS) { s_dropped_ids++; return; }
    can_id_stat_t *e = &s_tbl[s_tbl_n++];
    e->id         = m->identifier;
    e->count      = 1;
    e->prev_count = 0;
    e->dlc        = m->data_length_code;
    e->extd       = m->extd;
    memcpy(e->data, m->data, 8);
}

/* Format up to 8 payload bytes as hex, into out. */
static void frame_hex(const uint8_t *data, uint8_t dlc, char *out, size_t sz)
{
    size_t o = 0;
    uint8_t n = (dlc > 8) ? 8 : dlc;
    for (uint8_t i = 0; i < n && o + 3 < sz; i++)
        o += snprintf(out + o, sz - o, "%02X ", data[i]);
    if (o == 0 && sz) out[0] = '\0';
}

/* ============================== TWAI helpers ============================== */

static esp_err_t twai_bring_up(twai_mode_t mode, const twai_timing_config_t *timing)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, mode);
    g.rx_queue_len    = RX_QUEUE_LEN;
    g.alerts_enabled  = TWAI_ALERT_BUS_ERROR | TWAI_ALERT_ERR_PASS |
                        TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_BUS_OFF;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g, timing, &f);
    if (err != ESP_OK) return err;
    err = twai_start();
    if (err != ESP_OK) twai_driver_uninstall();
    return err;
}

static void twai_tear_down(void)
{
    twai_stop();
    twai_driver_uninstall();
}

/* ============================ Desk self-test ============================== */
#if RUN_MODE == MODE_SELFTEST

/* Transmit one frame as a self-reception request and expect it straight back.
 * Proves the TWAI driver, the GPIO15/16 mapping and the transceiver without a
 * car and without a second CAN node. */
static void run_selftest(void)
{
    ESP_LOGW(TAG, "*** SELF-TEST BUILD (NO_ACK mode - transmits!). Desk use only. ***");
    ESP_LOGI(TAG, "Termination jumper must be ON for the transceiver to echo.");

    twai_timing_config_t t = BITRATES[0].timing;   /* 500k */
    esp_err_t err = twai_bring_up(TWAI_MODE_NO_ACK, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SELF-TEST FAIL: driver bring-up: %s", esp_err_to_name(err));
        return;
    }

    twai_message_t tx = {
        .identifier       = 0x555,
        .data_length_code = 4,
        .self             = 1,
        .data             = { 0xDE, 0xAD, 0xBE, 0xEF },
    };
    err = twai_transmit(&tx, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SELF-TEST FAIL: transmit: %s", esp_err_to_name(err));
        twai_tear_down();
        return;
    }

    twai_message_t rx;
    err = twai_receive(&rx, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SELF-TEST FAIL: no self-reception (%s). Check the 120 ohm "
                      "jumper, GPIO15/16 and that the transceiver is powered.",
                 esp_err_to_name(err));
    } else if (rx.identifier == tx.identifier &&
               rx.data_length_code == tx.data_length_code &&
               memcmp(rx.data, tx.data, tx.data_length_code) == 0) {
        char hex[32];
        frame_hex(rx.data, rx.data_length_code, hex, sizeof(hex));
        ESP_LOGI(TAG, "SELF-TEST PASS (self-reception ok): id=0x%03X dlc=%u data=%s",
                 (unsigned)rx.identifier, rx.data_length_code, hex);
    } else {
        ESP_LOGE(TAG, "SELF-TEST FAIL: frame mismatch (got id=0x%03X dlc=%u)",
                 (unsigned)rx.identifier, rx.data_length_code);
    }
    twai_tear_down();
}

#elif RUN_MODE == MODE_ACTIVE || RUN_MODE == MODE_SCAN || RUN_MODE == MODE_DASH

/* Requests to fire each cycle. Mixes confirmed PIDs (a known-good baseline) with
 * the two multi-frame values the ELM327 path could never reach: F194 is the BMS
 * software version the MAXUS Display-SOC transform is keyed on (H1), F190 is the
 * VIN. Addressing per docs/findings/01-confirmed-pids.md. */
typedef struct {
    const char *name;
    uint32_t    req_id;
    uint32_t    resp_id;
    uint8_t     did_hi, did_lo;
} uds_probe_t;

static const uds_probe_t PROBES[] = {
    { "Pack Voltage B042", 0x7E5, 0x7ED, 0xB0, 0x42 },  /* confirmed baseline   */
    { "SOC raw      B046", 0x7E5, 0x7ED, 0xB0, 0x46 },  /* compare to dash now  */
    { "SOH          B061", 0x7E5, 0x7ED, 0xB0, 0x61 },
    { "BMS version  F194", 0x7E5, 0x7ED, 0xF1, 0x94 },  /* multi-frame, H1 key  */
    { "VIN          F190", 0x7E3, 0x7EB, 0xF1, 0x90 },  /* multi-frame          */
    { "Cell V max   B058", 0x7E5, 0x7ED, 0xB0, 0x58 },
};
#define N_PROBES (sizeof(PROBES) / sizeof(PROBES[0]))

#define UDS_BUF 256

/* ---- Scan definitions, shared by MODE_SCAN (serial) and MODE_DASH (WiFi) ---- */

/* Diagnostic request range. Hard limit - see the guard in uds_xfer(). */
#define SCAN_REQ_LO   0x700
#define SCAN_REQ_HI   0x7FF
#define SCAN_MAX_ECUS   48

/* DID blocks worth sweeping: every PID confirmed so far lives in the B0/B3/B4/
 * B7/B9/BA pages, F1 is the standard identification page, 00 the generic OBD
 * one (docs/findings/09-ovms-cross-reference.md). */
static const struct { uint16_t lo, hi; } DID_RANGES[] = {
    { 0xB000, 0xB0FF }, { 0xB300, 0xB3FF }, { 0xB400, 0xB4FF },
    { 0xB700, 0xB7FF }, { 0xB900, 0xB9FF }, { 0xBA00, 0xBAFF },
    { 0xF100, 0xF1FF }, { 0x0000, 0x00FF },
};
#define N_DID_RANGES (sizeof(DID_RANGES) / sizeof(DID_RANGES[0]))

/* Names for the ECUs identified from their own ISO 14229 identification DIDs
 * (docs/findings/12-ecu-identification-and-bms-structure.md). */
static const char *ecu_name(uint32_t req_id)
{
    switch (req_id) {
    case 0x704: return "BDU";
    case 0x747: return "AVM";
    case 0x7E3: return "IMCU";
    case 0x7E4: return "CCU";
    case 0x7E5: return "BMS";
    default:    return "UNKNOWN";
    }
}

/* Send one Service 22 request and assemble the reply, handling ISO-TP framing:
 * single frame, or first frame -> flow control -> consecutive frames. Returns
 * the assembled payload length (starting at the 0x62 / 0x7F byte), or -1.
 *
 * Multi-frame is the capability the ELM327 port never had; it is what unlocks
 * F194/F190 (see docs/findings/09-ovms-cross-reference.md). */
static int uds_xfer(uint32_t req_id, uint32_t resp_id, uint8_t did_hi, uint8_t did_lo,
                    uint8_t *out, size_t out_sz, uint32_t *from_id,
                    uint32_t timeout_ms, bool verbose)
{
    /* Only the ISO 15765-4 diagnostic range may ever be transmitted to. Lower
     * ids are the vehicle's own broadcast/control traffic - a frame sent there
     * could be taken as a real command, so it is refused outright. */
    if (req_id < 0x700 || req_id > 0x7FF) {
        ESP_LOGE(TAG, "REFUSED: 0x%03X is outside the diagnostic range 0x700-0x7FF",
                 (unsigned)req_id);
        return -1;
    }

    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) { }      /* drop anything stale */

    twai_message_t tx = {
        .identifier       = req_id,
        .data_length_code = 8,
        .data = { 0x03, 0x22, did_hi, did_lo, UDS_PAD, UDS_PAD, UDS_PAD, UDS_PAD },
    };
    if (twai_transmit(&tx, pdMS_TO_TICKS(500)) != ESP_OK) {
        if (verbose) ESP_LOGE(TAG, "    transmit failed (nothing ACKed the frame)");
        return -1;
    }

    int     total = -1, got = 0;
    uint8_t next_sn = 1;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        uint32_t wait = (timeout_ms < 100) ? timeout_ms : 100;
        if (twai_receive(&m, pdMS_TO_TICKS(wait)) != ESP_OK) continue;

        char hex[32];
        frame_hex(m.data, m.data_length_code, hex, sizeof(hex));

        /* resp_id 0 means "accept whatever answers" - that is how the ECU scan
         * discovers the request->response mapping instead of assuming +8. */
        if (resp_id != 0 && m.identifier != resp_id) {
            /* Anything else on the bus is itself interesting - the OBD port is
             * otherwise completely silent on this vehicle. */
            if (verbose)
                ESP_LOGI(TAG, "    (unexpected id 0x%03X: %s)", (unsigned)m.identifier, hex);
            continue;
        }
        if (from_id) *from_id = m.identifier;
        if (verbose) ESP_LOGI(TAG, "    rx 0x%03X: %s", (unsigned)m.identifier, hex);

        switch (m.data[0] >> 4) {
        case 0x0: {                                     /* single frame */
            int len = m.data[0] & 0x0F;
            if (len > 7) len = 7;
            for (int i = 0; i < len && (size_t)i < out_sz; i++) out[i] = m.data[1 + i];
            return len;
        }
        case 0x1:                                       /* first frame */
            total = ((m.data[0] & 0x0F) << 8) | m.data[1];
            if (verbose)
                ESP_LOGI(TAG, "    MULTI-FRAME: %d bytes total -> sending flow control",
                         total);
            for (int i = 0; i < 6 && (size_t)got < out_sz; i++) out[got++] = m.data[2 + i];
            {
                twai_message_t fc = {                   /* FC: CTS, BS=0, ST=0 */
                    .identifier       = req_id,
                    .data_length_code = 8,
                    .data = { 0x30, 0x00, 0x00, UDS_PAD, UDS_PAD, UDS_PAD, UDS_PAD, UDS_PAD },
                };
                if (twai_transmit(&fc, pdMS_TO_TICKS(200)) != ESP_OK)
                    ESP_LOGW(TAG, "    flow-control transmit failed");
            }
            /* A multi-frame reply needs longer than a scan's short timeout. */
            deadline = esp_timer_get_time() + 1000 * 1000;
            next_sn = 1;
            break;
        case 0x2: {                                     /* consecutive frame */
            uint8_t sn = m.data[0] & 0x0F;
            if (sn != next_sn && verbose)
                ESP_LOGW(TAG, "    CF out of order: got %u expected %u", sn, next_sn);
            next_sn = (next_sn + 1) & 0x0F;
            for (int i = 0; i < 7 && (size_t)got < out_sz; i++) out[got++] = m.data[1 + i];
            if (total > 0 && got >= total) return total;
            break;
        }
        default:
            if (verbose) ESP_LOGW(TAG, "    unhandled PCI 0x%X", m.data[0] >> 4);
            break;
        }
    }

    if (got > 0) {
        if (verbose) ESP_LOGW(TAG, "    timed out with %d/%d bytes", got, total);
        return got;
    }
    return -1;
}

/* Thin wrapper for the fixed probe list. */
static int uds_request(const uds_probe_t *p, uint8_t *out, size_t out_sz)
{
    return uds_xfer(p->req_id, p->resp_id, p->did_hi, p->did_lo,
                    out, out_sz, NULL, 2000, true);
}

/* Log an assembled payload as hex plus ASCII (version strings and the VIN are
 * text, so the ASCII column is what makes them readable). */
static void log_payload(const uint8_t *d, int n)
{
    char hex[3 * 32 + 1], asc[32 + 1];
    int  shown = (n > 32) ? 32 : n;
    int  o = 0;
    for (int i = 0; i < shown; i++) {
        o += snprintf(hex + o, sizeof(hex) - o, "%02X ", d[i]);
        asc[i] = (d[i] >= 0x20 && d[i] < 0x7F) ? (char)d[i] : '.';
    }
    asc[shown] = '\0';
    ESP_LOGI(TAG, "    => %d bytes: %s | \"%s\"%s", n, hex, asc,
             (n > 32) ? " (truncated)" : "");
}

/* ============================== Web dashboard ============================= */
#if RUN_MODE == MODE_DASH

/* Rendering form for values that are not a plain scaled number. */
typedef enum { FMT_NUM, FMT_BMS_STATUS, FMT_CELLGROUP, FMT_REGEN } pid_fmt_t;

typedef struct {
    const char *name;
    const char *group;      /* dashboard tab                                  */
    uint16_t    req_id, resp_id;
    uint8_t     did_hi, did_lo;
    uint8_t     nbytes;     /* data bytes consumed, big-endian (1-4)          */
    float       scale, offset;   /* value = raw * scale + offset              */
    int         dec;
    const char *unit;
    pid_fmt_t   fmt;
    bool        testing;    /* unverified -> shown with a "test" tag          */
} pid_desc_t;

/* Ported from firmware/poc4_idf_decode with the corrections the car has since
 * taught us (notes 10 and 12):
 *   - the ten MG-inherited DIDs that do not exist on MAXUS are gone
 *   - the six sentinel DIDs (3FFF / FFFE) are gone - they answer, but the
 *     payload means "no data", so any decoded number is meaningless
 *   - the motor PIDs lost their `testing` tag (proven by the idle->driving delta)
 *   - the per-cell module groups are new: single-frame, 12 of them, stride 8
 * Formulas: docs/findings/01-confirmed-pids.md and src/pids.py. */
static const pid_desc_t PIDS[] = {
    /* ---------- BMS 7E5 - confirmed ---------- */
    {"Bus Voltage",  "BMS", 0x7E5,0x7ED, 0xB0,0x41, 2, 0.25f,     0.0f, 1, "V", FMT_NUM, false},
    {"Pack Voltage", "BMS", 0x7E5,0x7ED, 0xB0,0x42, 2, 0.25f,     0.0f, 1, "V", FMT_NUM, false},
    {"Pack Current", "BMS", 0x7E5,0x7ED, 0xB0,0x43, 2, 0.025f, -1000.0f, 1, "A", FMT_NUM, false},
    {"SOC Raw",      "BMS", 0x7E5,0x7ED, 0xB0,0x46, 2, 0.1f,      0.0f, 1, "%", FMT_NUM, false},
    {"Cell V Max",   "BMS", 0x7E5,0x7ED, 0xB0,0x58, 2, 0.001f,    0.0f, 3, "V", FMT_NUM, false},
    {"Cell V Min",   "BMS", 0x7E5,0x7ED, 0xB0,0x59, 2, 0.001f,    0.0f, 3, "V", FMT_NUM, false},
    {"Batt Temp Hi", "BMS", 0x7E5,0x7ED, 0xB0,0x56, 1, 0.5f,    -40.0f, 1, "C", FMT_NUM, false},
    {"Batt Temp Lo", "BMS", 0x7E5,0x7ED, 0xB0,0x57, 1, 0.5f,    -40.0f, 1, "C", FMT_NUM, false},
    {"Charge Status","BMS", 0x7E5,0x7ED, 0xB0,0x48, 1, 0.0f,      0.0f, 0, "",  FMT_BMS_STATUS, false},
    /* ---------- BMS 7E5 - still testing ---------- */
    {"SOH",          "BMS", 0x7E5,0x7ED, 0xB0,0x61, 2, 0.01f,     0.0f, 1, "%", FMT_NUM, true},
    {"Coolant",      "BMS", 0x7E5,0x7ED, 0xB0,0x5C, 1, 0.5f,    -40.0f, 1, "C", FMT_NUM, true},

    /* ---------- Motor / IMCU 7E3 - confirmed by idle->driving delta ---------- */
    {"Motor Temp",   "Motor", 0x7E3,0x7EB, 0xB4,0x05, 1, 1.0f,   -40.0f, 0, "C",   FMT_NUM, false},
    {"Motor Speed",  "Motor", 0x7E3,0x7EB, 0xB4,0x02, 2, 1.0f, -32767.0f, 0, "rpm", FMT_NUM, false},
    {"Motor Torque", "Motor", 0x7E3,0x7EB, 0xB4,0x01, 2, 1.0f, -32767.0f, 0, "",    FMT_NUM, false},
    /* H3, the project's primary goal. Found by diffing full DID scans taken at
     * regen Low / Medium / High: the only DID of 309 that stepped 00 -> 01 -> 02
     * in selector order. Confirmed live by predicting the level from the raw
     * value before being told (docs/findings/13-regen-level-found.md). */
    {"Regen Level",  "Motor", 0x7E3,0x7EB, 0xB4,0x80, 1, 0.0f,      0.0f, 0, "",    FMT_REGEN, false},

    /* ---------- Vehicle - IMCU 7E3 ---------- */
    {"12V Battery",  "Vehicle", 0x7E3,0x7EB, 0x01,0x12, 1, 1.0f, 0.0f, 1, "V",  FMT_NUM, false},
    {"Odometer",     "Vehicle", 0x7E3,0x7EB, 0xE1,0x01, 3, 1.0f, 0.0f, 0, "km", FMT_NUM, false},

    /* ---------- Cells: 12 module groups, 7 bytes each (note 12) ----------
     * Layout {Va_hi,Va_lo, Vb_hi,Vb_lo, Ta, Tb, ?}; MG formulas
     * V = INT16/2000 + 1.0 and T = byte*0.5 - 40 reproduce the pack state.
     * Still `testing`: the formulas come from the MG reference, not our car. */
    {"Module 1",  "Cells", 0x7E5,0x7ED, 0xB0,0x01, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 2",  "Cells", 0x7E5,0x7ED, 0xB0,0x09, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 3",  "Cells", 0x7E5,0x7ED, 0xB0,0x11, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 4",  "Cells", 0x7E5,0x7ED, 0xB0,0x19, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 5",  "Cells", 0x7E5,0x7ED, 0xB0,0x21, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 6",  "Cells", 0x7E5,0x7ED, 0xB0,0x29, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 7",  "Cells", 0x7E5,0x7ED, 0xB0,0xA3, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 8",  "Cells", 0x7E5,0x7ED, 0xB0,0xAB, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 9",  "Cells", 0x7E5,0x7ED, 0xB0,0xB3, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 10", "Cells", 0x7E5,0x7ED, 0xB0,0xBB, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 11", "Cells", 0x7E5,0x7ED, 0xB0,0xC3, 7, 0,0, 0, "", FMT_CELLGROUP, true},
    {"Module 12", "Cells", 0x7E5,0x7ED, 0xB0,0xCB, 7, 0,0, 0, "", FMT_CELLGROUP, true},

    /* ---------- Identification (read once, but cheap enough to keep) ---------- */
    {"VIN",         "Info", 0x7E3,0x7EB, 0xF1,0x90, 0, 0,0, 0, "", FMT_NUM, false},
    {"BMS Version", "Info", 0x7E5,0x7ED, 0xF1,0x94, 0, 0,0, 0, "", FMT_NUM, false},
};
#define N_PIDS (sizeof(PIDS) / sizeof(PIDS[0]))

/* B048 BMS-status enum, verbatim from note 07 (MG firmware); 03 seen live. */
static const char *b048_enum(int v)
{
    switch (v) {
    case 0x00: return "Connected, not charging";
    case 0x01: return "Idle";
    case 0x03: return "Running";
    case 0x06: return "Charging (AC)";
    case 0x07: return "Charging (DC)";
    case 0x08: return "About to sleep";
    case 0x0a: return "Connected";
    case 0x0c: return "Starting charge";
    default:   return "?";
    }
}

/* --- Shared snapshot: one rendered value per PID, guarded by a mutex --- */
typedef struct {
    bool valid;
    char text[48];
    char raw[40];
} pid_val_t;

static SemaphoreHandle_t s_snap_mutex;
static pid_val_t g_vals[N_PIDS];
static uint32_t  g_seq;
static char      g_sta_ip[16] = "-";

/* Serialises TWAI access between the dashboard poller and the scan task, and
 * the flag the poller watches so it stands down for the duration of a scan.
 * Declared here because the poller (below) is defined before the scan code. */
static SemaphoreHandle_t s_bus_mutex;
static volatile bool     s_scan_active;

/* Idle/tab-scoped polling: set by data_get_handler on every /data request,
 * read by dash_poll_task once per cycle. No dedicated mutex - a torn read of
 * either is self-healing (the next 1 s tick corrects it, same tolerance
 * already accepted for s_scan_active above), and s_active_group is always
 * null-terminated right after writing so a torn read can't run past the
 * buffer. Empty string means "no group filter" (poll everything), which is
 * what a bare `curl /data` with no ?g= gets, unchanged from before this. */
static volatile int64_t s_last_data_req_us;
static char             s_active_group[16] = "";
#define IDLE_TIMEOUT_US (7 * 1000 * 1000)   /* tick() polls every 1s while open */

/* Render one reply into `out`. `d` points at the payload after 62 <did><did>. */
static void render(const pid_desc_t *p, const uint8_t *d, int n, pid_val_t *out)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 2 < sizeof(out->raw); i++)
        o += snprintf(out->raw + o, sizeof(out->raw) - o, "%02X", d[i]);

    if (p->nbytes == 0) {                       /* ASCII identification value */
        int m = (n < (int)sizeof(out->text) - 1) ? n : (int)sizeof(out->text) - 1;
        for (int i = 0; i < m; i++)
            out->text[i] = (d[i] >= 0x20 && d[i] < 0x7F) ? (char)d[i] : '.';
        out->text[m] = '\0';
        return;
    }

    if (p->fmt == FMT_CELLGROUP) {
        float va = ((d[0] << 8 | d[1]) / 2000.0f) + 1.0f;
        float vb = ((d[2] << 8 | d[3]) / 2000.0f) + 1.0f;
        snprintf(out->text, sizeof(out->text), "%.3f/%.3f V  %.1f/%.1f C",
                 va, vb, d[4] * 0.5f - 40.0f, d[5] * 0.5f - 40.0f);
        return;
    }

    uint32_t raw = 0;
    for (int b = 0; b < p->nbytes; b++) raw = (raw << 8) | d[b];

    if (p->fmt == FMT_REGEN) {
        static const char *lv[] = {"Low", "Medium", "High"};
        snprintf(out->text, sizeof(out->text), "%s (%02X)",
                 raw < 3 ? lv[raw] : "?", (unsigned)raw);
    }
    else if (p->fmt == FMT_BMS_STATUS)
        snprintf(out->text, sizeof(out->text), "%s (%02X)", b048_enum((int)raw), (unsigned)raw);
    else
        snprintf(out->text, sizeof(out->text), "%.*f %s", p->dec,
                 raw * p->scale + p->offset, p->unit);
}

/* Poll the table forever. Each PID is written into the snapshot under a brief
 * lock of its own rather than building a full copy on the stack - the same
 * lesson the report_task stack overflow taught us. */
static void dash_poll_task(void *arg)
{
    uint8_t buf[UDS_BUF];

    while (1) {
        /* A scan owns the bus for minutes at a time; stand down rather than
         * interleaving 30 PID reads with it. Values simply hold their last
         * reading until the scan finishes. */
        if (s_scan_active) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

        /* Nobody has asked for /data recently -> nobody has the dashboard open
         * -> polling the bus only burns CAN traffic and power for no reader.
         * Stand down the same way; the very next request wakes it up again. */
        int64_t idle_us = esp_timer_get_time() - s_last_data_req_us;
        if (s_last_data_req_us == 0 || idle_us > IDLE_TIMEOUT_US) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* Snapshot once per cycle so a concurrent write from data_get_handler
         * can't change which group this cycle is filtering on partway through. */
        char grp[sizeof(s_active_group)];
        strncpy(grp, s_active_group, sizeof(grp));
        grp[sizeof(grp) - 1] = '\0';

        int ok = 0, attempted = 0;
        for (size_t i = 0; i < N_PIDS; i++) {
            const pid_desc_t *p = &PIDS[i];

            /* Only poll the tab that's actually open - the CAN round-trip is
             * skipped for everything else, so its last rendered value just
             * holds until that tab is viewed again. Empty grp (no ?g= given,
             * e.g. a bare curl) polls everything, same as before this change.
             * A group matching no PID (the synthetic "Scan" tab) naturally
             * polls nothing, with no special case needed. */
            if (grp[0] != '\0' && strcmp(p->group, grp) != 0) continue;
            attempted++;

            pid_val_t v = { .valid = false, .text = "--", .raw = "" };

            xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
            int n = uds_xfer(p->req_id, p->resp_id, p->did_hi, p->did_lo,
                             buf, sizeof(buf), NULL, 200, false);
            xSemaphoreGive(s_bus_mutex);
            /* Need a positive response for the DID we asked for, long enough
             * for this descriptor. */
            if (n >= 3 && buf[0] == 0x62 && buf[1] == p->did_hi && buf[2] == p->did_lo
                && (n - 3) >= p->nbytes) {
                v.valid = true;
                render(p, buf + 3, n - 3, &v);
                ok++;
            }

            xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
            g_vals[i] = v;
            xSemaphoreGive(s_snap_mutex);
        }

        xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
        g_seq++;
        uint32_t seq = g_seq;
        xSemaphoreGive(s_snap_mutex);

        ESP_LOGI(TAG, "[poll #%lu] %d/%d ok | attempted=%d/%d g='%s' | heap=%lu",
                 (unsigned long)seq, ok, attempted, attempted, (int)N_PIDS, grp,
                 (unsigned long)esp_get_free_heap_size());

        /* Nobody answering means nobody will answer sooner by asking again fast
         * - wait longer to avoid pointless transmit spam. Once PIDs are
         * answering, a full cycle already takes ~0.6 s at ~10 ms/PID, so there
         * is no need to re-poll immediately either; the dashboard only needs to
         * feel live, not sub-second. */
        vTaskDelay(pdMS_TO_TICKS(ok > 0 ? 1000 : 2000));
    }
}

/* ------------------------- Regen level: write ------------------------- *
 * The ONLY write this firmware can perform. Everything else is Service 22
 * reads, and that is deliberate: a general write endpoint would put every DID
 * on 33 ECUs one typo away from being overwritten. Here the DID is hard-coded
 * and the value is range-checked, so the worst case is setting the regen
 * selector to a level it already supports.
 *
 * Whether the ECU even accepts this is unknown - 7E3 returns NRC 0x33
 * (securityAccessDenied) on F1A7, so a write may well be locked. The safe first
 * test is to write back the level the car already holds: a rejection tells us it
 * is locked, and an acceptance changes nothing.
 *
 * Service 2E = WriteDataByIdentifier; a positive reply is 0x6E (0x2E + 0x40). */
#define REGEN_REQ_ID   0x7E3
#define REGEN_RESP_ID  0x7EB
#define REGEN_DID_HI   0xB4
#define REGEN_DID_LO   0x80

/* Writes `level` and reports what came back, verbatim, into `out`. */
static bool regen_write(uint8_t level, char *out, size_t out_sz)
{
    if (level > 2) {
        snprintf(out, out_sz, "refused: level must be 0, 1 or 2");
        return false;
    }

    /* The only bus user in this file that used to skip s_bus_mutex.
     * dash_poll_task polls this same DID every cycle, and both receive loops
     * match replies by CAN id alone (no per-transaction correlation byte
     * exists in these UDS frames), so without this lock the two could
     * genuinely consume each other's reply frames - see
     * docs/findings/14-regen-write-attempt.md. */
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);

    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) { }        /* drop anything stale */

    twai_message_t tx = {
        .identifier       = REGEN_REQ_ID,
        .data_length_code = 8,
        .data = { 0x04, 0x2E, REGEN_DID_HI, REGEN_DID_LO, level,
                  UDS_PAD, UDS_PAD, UDS_PAD },
    };
    if (twai_transmit(&tx, pdMS_TO_TICKS(500)) != ESP_OK) {
        snprintf(out, out_sz, "transmit failed - nothing on the bus ACKed it");
        xSemaphoreGive(s_bus_mutex);
        return false;
    }

    int64_t deadline = esp_timer_get_time() + 2000 * 1000;
    while (esp_timer_get_time() < deadline) {
        if (twai_receive(&m, pdMS_TO_TICKS(100)) != ESP_OK) continue;
        if (m.identifier != REGEN_RESP_ID) continue;

        char hex[32];
        frame_hex(m.data, m.data_length_code, hex, sizeof(hex));
        ESP_LOGW(TAG, "regen write %u -> rx %s", level, hex);

        if (m.data[1] == 0x6E && m.data[2] == REGEN_DID_HI && m.data[3] == REGEN_DID_LO) {
            snprintf(out, out_sz, "ACCEPTED (6E B4 80) - wrote level %u", level);
            xSemaphoreGive(s_bus_mutex);
            return true;
        }
        if (m.data[1] == 0x7F) {
            const char *why =
                m.data[3] == 0x33 ? "securityAccessDenied - needs an unlock" :
                m.data[3] == 0x22 ? "conditionsNotCorrect - vehicle state blocks it" :
                m.data[3] == 0x31 ? "requestOutOfRange - value or DID not writable" :
                m.data[3] == 0x7E ? "subFunctionNotSupportedInActiveSession" :
                m.data[3] == 0x7F ? "serviceNotSupportedInActiveSession - needs an Extended session (10 03)" :
                m.data[3] == 0x88 ? "vehicleSpeedTooHigh - stop the vehicle first" :
                m.data[3] == 0x11 ? "service not supported at all" : "see NRC";
            snprintf(out, out_sz, "REJECTED NRC 0x%02X - %s  [%s]", m.data[3], why, hex);
            xSemaphoreGive(s_bus_mutex);
            return false;
        }
        snprintf(out, out_sz, "unexpected reply: %s", hex);
        xSemaphoreGive(s_bus_mutex);
        return false;
    }
    snprintf(out, out_sz, "no reply within 2 s");
    xSemaphoreGive(s_bus_mutex);
    return false;
}

static esp_err_t regen_set_handler(httpd_req_t *req)
{
    char q[64] = "", v[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "level", v, sizeof(v)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "usage: /regen/set?level=0|1|2");
    }

    char msg[160];
    regen_write((uint8_t)atoi(v), msg, sizeof(msg));   /* result is in msg either way */
    ESP_LOGW(TAG, "regen/set level=%s -> %s", v, msg);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, msg);
}

/* ---------------------- Scan over WiFi (no laptop) --------------------- *
 * The board lives in the car; the whole point is to trigger a full DID sweep
 * and collect the result without a USB cable. Results accumulate in a heap
 * buffer and are downloaded afterwards from /scan/result.
 *
 * This is the tool the regen hunt (H3, the primary goal) needs: scan at regen
 * Low, at Medium, at High, then diff the three files. */

#define SCAN_BUF_CAP  (96 * 1024)   /* ~1824 hit lines at ~28 bytes, plus slack */

typedef enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE, SCAN_FAILED } scan_state_t;

static volatile scan_state_t s_scan_state = SCAN_IDLE;
static char  *s_scan_buf;
static size_t s_scan_len;
static int    s_scan_pct;
static int    s_scan_found;
static char   s_scan_cur[32] = "";
static uint32_t s_scan_ecus[SCAN_MAX_ECUS]; /* request ids selected by the caller */
static int    s_scan_n_ecus;

static void sbuf(const char *fmt, ...)
{
    if (!s_scan_buf || s_scan_len + 128 >= SCAN_BUF_CAP) return;
    va_list ap;
    va_start(ap, fmt);
    s_scan_len += vsnprintf(s_scan_buf + s_scan_len, SCAN_BUF_CAP - s_scan_len, fmt, ap);
    va_end(ap);
}

static void scan_task(void *arg)
{
    uint8_t buf[UDS_BUF];
    uint32_t resp_of[SCAN_MAX_ECUS];
    int n_ecus = 0;

    s_scan_len   = 0;
    s_scan_found = 0;
    s_scan_pct   = 0;
    s_scan_state = SCAN_RUNNING;

    sbuf("# poc5 scan over WiFi\n");

    /* ---- Phase A: which of the requested addresses answer? ---- */
    strcpy(s_scan_cur, "discovering");
    for (int i = 0; i < s_scan_n_ecus; i++) {
        uint32_t id = s_scan_ecus[i], from = 0;
        xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
        int n = uds_xfer(id, 0, 0xF1, 0x90, buf, sizeof(buf), &from, 60, false);
        xSemaphoreGive(s_bus_mutex);
        if (n < 0) continue;
        sbuf("[ECU] req=%03X resp=%03X %s\n", (unsigned)id, (unsigned)from, ecu_name(id));
        s_scan_ecus[n_ecus] = id;
        resp_of[n_ecus++]   = from;
    }
    sbuf("# %d ECU(s) responding\n", n_ecus);

    if (n_ecus == 0) {
        sbuf("# no ECU answered - is the vehicle READY?\n");
        s_scan_state = SCAN_FAILED;
        s_scan_active = false;
        vTaskDelete(NULL);
        return;
    }

    /* ---- Phase B: sweep the DID pages on each responding ECU ---- */
    const long total = (long)n_ecus * N_DID_RANGES * 256;
    long done = 0;

    for (int e = 0; e < n_ecus; e++) {
        uint32_t req = s_scan_ecus[e], resp = resp_of[e];
        sbuf("# ECU %03X (%s)\n", (unsigned)req, ecu_name(req));
        int found = 0;

        for (size_t r = 0; r < N_DID_RANGES; r++) {
            for (uint32_t did = DID_RANGES[r].lo; did <= DID_RANGES[r].hi; did++) {
                snprintf(s_scan_cur, sizeof(s_scan_cur), "%03X %04X",
                         (unsigned)req, (unsigned)did);

                xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
                int n = uds_xfer(req, resp, (did >> 8) & 0xFF, did & 0xFF,
                                 buf, sizeof(buf), NULL, 60, false);
                xSemaphoreGive(s_bus_mutex);

                if (++done % 64 == 0) s_scan_pct = (int)(done * 100 / total);

                if (n < 0) continue;
                if (n >= 3 && buf[0] == 0x7F) {
                    /* NRC 0x31 just means "no such DID" and would swamp the
                     * output; anything else is worth recording. */
                    if (buf[2] != 0x31)
                        sbuf("[%03X] %04X NRC %02X\n", (unsigned)req, (unsigned)did, buf[2]);
                    continue;
                }
                if (n >= 3 && buf[0] == 0x62) {
                    found++; s_scan_found++;
                    sbuf("[%03X] %04X + %2d : ", (unsigned)req, (unsigned)did, n - 3);
                    for (int i = 3; i < n && i < 3 + 24; i++) sbuf("%02X ", buf[i]);
                    sbuf("\n");
                }
            }
        }
        sbuf("# ECU %03X: %d supported\n", (unsigned)req, found);
    }

    s_scan_pct = 100;
    strcpy(s_scan_cur, "done");
    sbuf("# SCAN COMPLETE - %d DIDs\n", s_scan_found);
    s_scan_state  = SCAN_DONE;
    s_scan_active = false;
    ESP_LOGI(TAG, "scan complete: %d DIDs, %u bytes", s_scan_found, (unsigned)s_scan_len);
    vTaskDelete(NULL);
}

/* GET /scan/start?ecus=7E3,7E4   (or ?ecus=all for the whole 0x700-0x7FF range)
 * A targeted scan matters: 7E3+7E4 is ~4 minutes where all 33 ECUs is ~25, and
 * the regen hunt needs several passes. */
static esp_err_t scan_start_handler(httpd_req_t *req)
{
    if (s_scan_state == SCAN_RUNNING) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "scan already running");
    }

    char q[128] = "", val[96] = "7E3,7E4";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "ecus", val, sizeof(val));

    s_scan_n_ecus = 0;
    if (strcmp(val, "all") == 0) {
        for (uint32_t id = SCAN_REQ_LO; id <= SCAN_REQ_HI && s_scan_n_ecus < SCAN_MAX_ECUS; id++)
            s_scan_ecus[s_scan_n_ecus++] = id;
    } else {
        for (char *t = strtok(val, ","); t && s_scan_n_ecus < SCAN_MAX_ECUS;
             t = strtok(NULL, ",")) {
            uint32_t id = (uint32_t)strtol(t, NULL, 16);
            if (id >= SCAN_REQ_LO && id <= SCAN_REQ_HI) s_scan_ecus[s_scan_n_ecus++] = id;
        }
    }
    if (s_scan_n_ecus == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "no valid ECU ids (must be 700-7FF)");
    }

    if (!s_scan_buf) s_scan_buf = heap_caps_malloc(SCAN_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (!s_scan_buf) s_scan_buf = malloc(SCAN_BUF_CAP);
    if (!s_scan_buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "out of memory for the scan buffer");
    }

    s_scan_active = true;            /* dashboard poller stands down */
    xTaskCreate(scan_task, "scan", 8192, NULL, 5, NULL);

    char msg[64];
    int n = snprintf(msg, sizeof(msg), "scan started on %d ECU(s)", s_scan_n_ecus);
    return httpd_resp_send(req, msg, n);
}

static esp_err_t scan_status_handler(httpd_req_t *req)
{
    static const char *names[] = {"idle", "running", "done", "failed"};
    char j[192];
    int n = snprintf(j, sizeof(j),
                     "{\"state\":\"%s\",\"pct\":%d,\"found\":%d,\"at\":\"%s\",\"bytes\":%u}",
                     names[s_scan_state], s_scan_pct, s_scan_found, s_scan_cur,
                     (unsigned)s_scan_len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, j, n);
}

static esp_err_t scan_result_handler(httpd_req_t *req)
{
    if (!s_scan_buf || s_scan_len == 0)
        return httpd_resp_sendstr(req, "# no scan result yet\n");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    /* Sent in chunks - the buffer is far larger than one response allows. */
    size_t off = 0;
    while (off < s_scan_len) {
        size_t c = s_scan_len - off;
        if (c > 4096) c = 4096;
        if (httpd_resp_send_chunk(req, s_scan_buf + off, c) != ESP_OK) return ESP_FAIL;
        off += c;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* -------------------------- OTA firmware update ----------------------- *
 * POST the new .bin here and the board reboots into it. This is what makes the
 * cable optional from now on:
 *     curl --data-binary @build/poc5_s3_can.bin http://maxus.local/update  */
static esp_err_t update_post_handler(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "no OTA partition");
    }
    ESP_LOGW(TAG, "OTA: receiving %d bytes -> %s", req->content_len, part->label);

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "esp_ota_begin failed");
    }

    char *buf = malloc(2048);
    if (!buf) { esp_ota_abort(h); return ESP_FAIL; }

    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > 2048 ? 2048 : remaining);
        if (r <= 0) {
            free(buf); esp_ota_abort(h);
            ESP_LOGE(TAG, "OTA: receive failed with %d bytes left", remaining);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "upload interrupted");
        }
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            free(buf); esp_ota_abort(h);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "esp_ota_write failed");
        }
        remaining -= r;
    }
    free(buf);

    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "image invalid - not switching");
    }

    httpd_resp_sendstr(req, "OK - rebooting into the new image\n");
    ESP_LOGW(TAG, "OTA ok, rebooting into %s", part->label);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------- HTTP -------------------------------- */

static esp_err_t data_get_handler(httpd_req_t *req)
{
    static char json[6144];
    int o = 0;

    /* Marks the dashboard as "someone is watching" (dash_poll_task's idle
     * check) and tells the poller which tab to actually poll for. Absent or
     * empty ?g= clears the filter, which is what keeps a bare curl (used
     * throughout this project for diagnostics) refreshing every PID. */
    s_last_data_req_us = esp_timer_get_time();
    char q[64], g[sizeof(s_active_group)] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "g", g, sizeof(g));
    strncpy(s_active_group, g, sizeof(s_active_group));
    s_active_group[sizeof(s_active_group) - 1] = '\0';

    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    o += snprintf(json + o, sizeof(json) - o,
                  "{\"seq\":%lu,\"ip\":\"%s\",\"pids\":[", (unsigned long)g_seq, g_sta_ip);
    for (size_t i = 0; i < N_PIDS; i++) {
        o += snprintf(json + o, sizeof(json) - o,
                      "%s{\"n\":\"%s\",\"g\":\"%s\",\"v\":\"%s\",\"r\":\"%s\",\"t\":%d}",
                      i ? "," : "", PIDS[i].name, PIDS[i].group,
                      g_vals[i].text, g_vals[i].raw, PIDS[i].testing ? 1 : 0);
    }
    o += snprintf(json + o, sizeof(json) - o, "]}");
    xSemaphoreGive(s_snap_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, o);
}

static const char HTML_PAGE[] =
"<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>MAXUS 9 EV</title><style>"
"body{margin:0;padding:12px 12px 72px;background:#111;color:#eee;"
"font:16px system-ui,-apple-system,sans-serif}"
"h1{font-size:20px;text-align:center;margin:6px 0 14px}"
"table{width:100%;border-collapse:collapse}"
"td{padding:11px 4px;border-bottom:1px solid #282828}"
"td.v{text-align:right;color:#4da3ff;font-weight:600;white-space:nowrap}"
"td.r{text-align:right;color:#555;font:12px ui-monospace,monospace;width:1%;"
"padding-left:10px;white-space:nowrap}"
".tag{background:#5a4300;color:#ffc107;font-size:11px;padding:1px 6px;"
"border-radius:8px;margin-left:6px;vertical-align:middle}"
"#foot{text-align:center;color:#777;font-size:13px;margin-top:14px}"
"#tabs{position:fixed;left:0;right:0;bottom:0;display:flex;background:#181818;"
"border-top:1px solid #2c2c2c}"
"#tabs button{flex:1;padding:14px 4px;background:none;border:none;color:#888;"
"font-size:15px;border-top:3px solid transparent}"
"#tabs button.on{color:#4da3ff;border-top-color:#4da3ff}"
"#scan,#rg{display:none}#scan button,#rg button{background:#4da3ff;color:#000;"
"border:none;padding:12px 18px;border-radius:8px;font-size:15px;font-weight:600;"
"margin:4px}"
"#scan input{background:#222;color:#eee;border:1px solid #444;border-radius:6px;"
"padding:10px;font-size:15px;width:60%}"
"#bar{height:8px;background:#282828;border-radius:4px;margin:14px 0}"
"#bar>div{height:100%;width:0;background:#4da3ff;border-radius:4px}"
"#slog{color:#888;font:13px ui-monospace,monospace;white-space:pre-wrap}"
"</style><h1>MAXUS 9 EV</h1><table id=t></table>"
"<div id=scan><p>ECUs (hex, comma separated, or <b>all</b>):</p>"
"<input id=ecus value='7E3,7E4'><p>"
"<button onclick='startScan()'>Start scan</button>"
"<button onclick=\"location.href='/scan/result'\">Download result</button></p>"
"<div id=bar><div></div></div><div id=slog>idle</div></div>"
"<div id=rg><p style='color:#888;font-size:14px'>Set regen level "
"(writes 7E3 B480 - nothing else can be written):</p>"
"<button onclick='setRegen(0)'>Low</button>"
"<button onclick='setRegen(1)'>Medium</button>"
"<button onclick='setRegen(2)'>High</button>"
"<div id=rglog style='color:#888;font:13px ui-monospace,monospace;"
"white-space:pre-wrap;margin-top:10px'>not tried yet</div></div>"
"<div id=foot>connecting...</div>"
"<div id=tabs></div><script>"
"let tab=localStorage.getItem('tab')||'';"
"async function startScan(){"
" const e=document.getElementById('ecus').value;"
" document.getElementById('slog').textContent="
"  await(await fetch('/scan/start?ecus='+encodeURIComponent(e))).text();"
"}"
"async function setRegen(l){"
" const e=document.getElementById('rglog');e.textContent='writing '+l+' ...';"
" try{e.textContent=await(await fetch('/regen/set?level='+l)).text()}"
" catch(err){e.textContent='request failed: '+err}"
"}"
"async function scanTick(){"
" let s;try{s=await(await fetch('/scan/status')).json()}catch(e){return}"
" document.querySelector('#bar>div').style.width=s.pct+'%';"
" document.getElementById('slog').textContent="
"  s.state+'  '+s.pct+'%  found '+s.found+'  '+s.at+'  ('+s.bytes+' bytes)';"
"}"
"async function tick(){"
" if(tab=='Scan'){scanTick()}"
" let d;try{d=await(await fetch('/data?g='+encodeURIComponent(tab))).json()}catch(e){"
"  document.getElementById('foot').textContent='offline';return}"
" const gs=[...new Set(d.pids.map(p=>p.g))].concat(['Scan']);"
" if(!gs.includes(tab))tab=gs[0];"
" const tb=document.getElementById('tabs');"
" if(tb.childElementCount!=gs.length){tb.innerHTML='';gs.forEach(g=>{"
"  const b=document.createElement('button');b.textContent=g;"
"  b.onclick=()=>{tab=g;localStorage.setItem('tab',g);tick()};tb.appendChild(b)})}"
" [...tb.children].forEach(b=>b.className=b.textContent==tab?'on':'');"
" const isScan=tab=='Scan';"
" document.getElementById('scan').style.display=isScan?'block':'none';"
" document.getElementById('rg').style.display=tab=='Motor'?'block':'none';"
" document.getElementById('t').style.display=isScan?'none':'table';"
" document.getElementById('t').innerHTML=d.pids.filter(p=>p.g==tab).map(p=>"
"  `<tr><td>${p.n}${p.t?'<span class=tag>test</span>':''}</td>"
"   <td class=v>${p.v}</td><td class=r>${p.r}</td></tr>`).join('');"
" document.getElementById('foot').textContent="
"  'poll #'+d.seq+(d.ip!='-'?' \\u00b7 '+d.ip:'');"
"}"
"tick();setInterval(tick,1000);</script>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;   /* OTA writes flash from the handler task */
    cfg.recv_wait_timeout = 20;    /* an OTA upload is bigger than a page GET */
    cfg.send_wait_timeout  = 20;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    httpd_uri_t routes[] = {
        {.uri = "/",             .method = HTTP_GET,  .handler = root_get_handler},
        {.uri = "/data",         .method = HTTP_GET,  .handler = data_get_handler},
        {.uri = "/scan/start",   .method = HTTP_GET,  .handler = scan_start_handler},
        {.uri = "/scan/status",  .method = HTTP_GET,  .handler = scan_status_handler},
        {.uri = "/scan/result",  .method = HTTP_GET,  .handler = scan_result_handler},
        {.uri = "/regen/set",    .method = HTTP_GET,  .handler = regen_set_handler},
        {.uri = "/update",       .method = HTTP_POST, .handler = update_post_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(srv, &routes[i]);

    ESP_LOGI(TAG, "HTTP up: / /data /scan/start /scan/status /scan/result /update");
}

/* ------------------------------- WiFi -------------------------------- */

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Keep trying: the router may be out of range (in the car) or rebooting.
         * The AP interface stays up regardless, so the dashboard never dies. */
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        strcpy(g_sta_ip, "-");
        ESP_LOGW(TAG, "STA disconnected (reason=%d) - retrying '%s'", e->reason, STA_SSID);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(g_sta_ip, sizeof(g_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "STA joined '%s' -> http://%s/  (or http://" MDNS_HOST ".local/)",
                 STA_SSID, g_sta_ip);
    }
}

/* APSTA on purpose: joining the home router is what lets a PC on the LAN (and
 * the analysis scripts) reach the board, but the router is not in range once
 * the car leaves - so the board's own AP stays up at the same time. */
static void wifi_init_apsta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_evt, NULL, NULL));

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    strncpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
    ap.ap.ssid_len       = strlen(AP_SSID);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode       = strlen(AP_PASS) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    if (STA_SSID[0]) {
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, STA_SSID, sizeof(sta.sta.ssid));
        strncpy((char *)sta.sta.password, STA_PASS, sizeof(sta.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (STA_SSID[0]) esp_wifi_connect();

    ESP_LOGI(TAG, "AP '%s' up at 192.168.4.1", AP_SSID);
}

static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed - reach the board by IP instead");
        return;
    }
    mdns_hostname_set(MDNS_HOST);
    mdns_instance_name_set("MAXUS 9 EV bridge");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://" MDNS_HOST ".local/");
}

static void run_dashboard(void)
{
    ESP_LOGI(TAG, "*** DASHBOARD MODE - Service 22 reads, WiFi APSTA ***");

    s_snap_mutex = xSemaphoreCreateMutex();
    s_bus_mutex  = xSemaphoreCreateMutex();
    if (!s_snap_mutex || !s_bus_mutex) { ESP_LOGE(TAG, "mutex alloc failed"); return; }
    for (size_t i = 0; i < N_PIDS; i++) strcpy(g_vals[i].text, "--");

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_apsta();
    mdns_start();
    http_start();

    twai_timing_config_t t = BITRATES[0].timing;   /* 500k */
    if (twai_bring_up(TWAI_MODE_NORMAL, &t) != ESP_OK) {
        ESP_LOGE(TAG, "TWAI bring-up failed");
        return;
    }
    ESP_LOGI(TAG, "Polling %d PIDs", (int)N_PIDS);
    xTaskCreate(dash_poll_task, "dash_poll", 6144, NULL, 5, NULL);
}

#endif /* MODE_DASH */

/* ============================== ECU / DID scan ============================ */
#if RUN_MODE == MODE_SCAN

/* Phase B sweeps only ECUs at or above this address. Phase A always reports the
 * full map. Set it to resume a scan that was split across runs instead of
 * re-sweeping addresses already covered. */
#define SCAN_SWEEP_FROM 0x700

static void run_scan(void)
{
    ESP_LOGW(TAG, "*** SCAN MODE - Service 22 reads only, ids 0x%03X-0x%03X ***",
             SCAN_REQ_LO, SCAN_REQ_HI);
    ESP_LOGW(TAG, "Keep the vehicle READY but PARKED for the whole scan.");

    twai_timing_config_t t = BITRATES[0].timing;   /* 500k */
    if (twai_bring_up(TWAI_MODE_NORMAL, &t) != ESP_OK) {
        ESP_LOGE(TAG, "driver bring-up failed");
        return;
    }

    uint8_t  buf[UDS_BUF];
    uint32_t ecus[SCAN_MAX_ECUS], resps[SCAN_MAX_ECUS];
    int      n_ecus = 0;

    /* ---- Phase A: which addresses answer at all? -------------------------
     * A negative response counts: it still proves an ECU is listening there. */
    ESP_LOGI(TAG, "===== PHASE A: ECU discovery =====");
    for (uint32_t id = SCAN_REQ_LO; id <= SCAN_REQ_HI; id++) {
        uint32_t from = 0;
        int n = uds_xfer(id, 0, 0xF1, 0x90, buf, sizeof(buf), &from, 60, false);
        if (n < 0) continue;

        const char *kind = (n >= 3 && buf[0] == 0x7F) ? "negative" : "positive";
        ESP_LOGI(TAG, "[ECU] req=%03X resp=%03X %-8s (%s)",
                 (unsigned)id, (unsigned)from, ecu_name(id), kind);
        if (n_ecus < SCAN_MAX_ECUS) {
            ecus[n_ecus]    = id;
            resps[n_ecus++] = from;
        }
    }
    ESP_LOGI(TAG, "PHASE A done: %d ECU(s) responding", n_ecus);
    if (n_ecus == 0) {
        ESP_LOGE(TAG, "No ECU answered - is the vehicle READY?");
        return;
    }

    /* ---- Phase B: sweep the DID pages on each ECU ------------------------
     * NRC 0x31 (requestOutOfRange) just means "no such DID here" and would
     * drown the log, so it is only counted. Any other NRC is reported. */
    ESP_LOGI(TAG, "===== PHASE B: DID sweep =====");
    for (int e = 0; e < n_ecus; e++) {
        uint32_t req = ecus[e], resp = resps[e];
        if (req < SCAN_SWEEP_FROM) {
            ESP_LOGI(TAG, "===== ECU %03X skipped (below SCAN_SWEEP_FROM %03X) =====",
                     (unsigned)req, SCAN_SWEEP_FROM);
            continue;
        }
        ESP_LOGI(TAG, "===== ECU %03X (%s) =====", (unsigned)req, ecu_name(req));
        int found = 0, nrc31 = 0, other = 0;

        for (size_t r = 0; r < N_DID_RANGES; r++) {
            for (uint32_t did = DID_RANGES[r].lo; did <= DID_RANGES[r].hi; did++) {
                uint8_t hi = (did >> 8) & 0xFF, lo = did & 0xFF;
                int n = uds_xfer(req, resp, hi, lo, buf, sizeof(buf), NULL, 60, false);
                if (n < 0) continue;

                if (n >= 3 && buf[0] == 0x7F) {
                    if (buf[2] == 0x31) { nrc31++; continue; }
                    other++;
                    ESP_LOGW(TAG, "[%03X] %04X NRC %02X",
                             (unsigned)req, (unsigned)did, buf[2]);
                    continue;
                }
                if (n >= 3 && buf[0] == 0x62) {
                    found++;
                    char hex[3 * 24 + 1], asc[24 + 1];
                    int  dn = n - 3, shown = (dn > 24) ? 24 : dn, o = 0;
                    for (int i = 0; i < shown; i++) {
                        o += snprintf(hex + o, sizeof(hex) - o, "%02X ", buf[3 + i]);
                        asc[i] = (buf[3+i] >= 0x20 && buf[3+i] < 0x7F) ? (char)buf[3+i] : '.';
                    }
                    asc[shown] = '\0';
                    ESP_LOGI(TAG, "[%03X] %04X + %2d : %-*s| %s",
                             (unsigned)req, (unsigned)did, dn, 3 * 24, hex, asc);
                }
            }
            ESP_LOGI(TAG, "[%03X] ...%04X-%04X swept (found=%d)", (unsigned)req,
                     DID_RANGES[r].lo, DID_RANGES[r].hi, found);
        }
        ESP_LOGI(TAG, "===== ECU %03X SUMMARY: %d supported, %d unsupported, "
                      "%d other-NRC =====", (unsigned)req, found, nrc31, other);
    }

    ESP_LOGI(TAG, "########## SCAN COMPLETE ##########");
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}

#endif /* MODE_SCAN */

/* Fire every probe in turn and report what came back.
 *
 * Active mode exists because the MAXUS OBD port carries no broadcast traffic at
 * all (listen-only saw zero frames at 4 bitrates with bus_err=0, while the
 * harness measured a healthy 63 ohm across CAN-H/CAN-L) - the port answers
 * requests but does not forward the internal buses, so the transport can only be
 * proven by asking, exactly as the ELM327 path does. */
__attribute__((unused))
static void run_active_uds(void)
{
    ESP_LOGW(TAG, "*** ACTIVE MODE - transmits Service 22 (read-only) requests ***");

    twai_timing_config_t t = BITRATES[0].timing;   /* 500k, ISO 15765-4 */
    esp_err_t err = twai_bring_up(TWAI_MODE_NORMAL, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver bring-up failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Probing %d DIDs per cycle ...", (int)N_PROBES);

    uint32_t cycle = 0;
    uint8_t  buf[UDS_BUF];

    while (1) {
        cycle++;
        ESP_LOGI(TAG, "========== cycle #%lu ==========", (unsigned long)cycle);

        for (size_t i = 0; i < N_PROBES; i++) {
            const uds_probe_t *p = &PROBES[i];
            ESP_LOGI(TAG, "-- %s  (req %03X -> resp %03X)", p->name,
                     (unsigned)p->req_id, (unsigned)p->resp_id);

            int n = uds_request(p, buf, sizeof(buf));
            if (n < 0) {
                ESP_LOGW(TAG, "    NO REPLY");
            } else if (n >= 3 && buf[0] == 0x7F) {
                ESP_LOGW(TAG, "    NEGATIVE response, NRC=0x%02X", buf[2]);
            } else if (n >= 3 && buf[0] == 0x62) {
                /* Positive: 62 <did_hi> <did_lo> <data...> */
                log_payload(buf + 3, n - 3);
                if (n >= 5) {
                    int v = buf[3] * 256 + buf[4];
                    ESP_LOGI(TAG, "    INT16=%d (0x%04X)  x0.25=%.2f  /10=%.1f  /100=%.2f",
                             v, (unsigned)v, v * 0.25, v / 10.0, v / 100.0);
                }
            } else {
                log_payload(buf, n);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        twai_status_info_t st;
        twai_get_status_info(&st);
        if (st.state == TWAI_STATE_BUS_OFF) {
            /* Without an ACK the controller eventually drops to bus-off; recover
             * so a wiring/bus hiccup does not silently end the session. */
            ESP_LOGE(TAG, "BUS_OFF - initiating recovery");
            twai_initiate_recovery();
            vTaskDelay(pdMS_TO_TICKS(1000));
            twai_start();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

#else  /* ===================== Listen-only sniffer ======================== */

/* Try each bitrate in turn until frames arrive. Leaves the driver installed and
 * running on the chosen rate. Returns the index that was locked, or -1 if none
 * produced traffic (in which case 500k is left running for diagnostics). */
static int probe_bitrate(void)
{
    for (size_t i = 0; i < N_BITRATES; i++) {
        twai_timing_config_t t = BITRATES[i].timing;
        esp_err_t err = twai_bring_up(TWAI_MODE_LISTEN_ONLY, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "probe %s: bring-up failed: %s", BITRATES[i].name,
                     esp_err_to_name(err));
            continue;
        }

        ESP_LOGI(TAG, "probe %-4s : listening %d ms ...", BITRATES[i].name, PROBE_MS);
        int64_t deadline = esp_timer_get_time() + (int64_t)PROBE_MS * 1000;
        twai_message_t m;
        while (esp_timer_get_time() < deadline) {
            if (twai_receive(&m, pdMS_TO_TICKS(200)) == ESP_OK) {
                ESP_LOGI(TAG, "probe %-4s : FRAMES SEEN -> locking this bitrate",
                         BITRATES[i].name);
                s_locked_rate = BITRATES[i].name;
                return (int)i;   /* driver stays installed and running */
            }
        }

        twai_status_info_t st;
        twai_get_status_info(&st);
        ESP_LOGW(TAG, "probe %-4s : no frames (bus_err=%lu rx_err=%lu)",
                 BITRATES[i].name, (unsigned long)st.bus_error_count,
                 (unsigned long)st.rx_error_counter);
        twai_tear_down();
    }

    /* Nothing anywhere - fall back to 500k so the report task can still show
     * counters, which is what distinguishes "wiring" from "bitrate". */
    twai_timing_config_t t = BITRATES[0].timing;
    if (twai_bring_up(TWAI_MODE_LISTEN_ONLY, &t) == ESP_OK) s_locked_rate = "500k?";
    ESP_LOGE(TAG, "No frames at any bitrate. If bus_err stays 0 the problem is "
                  "wiring/pins/bus power; if bus_err climbs it is bit timing.");
    return -1;
}

/* Receive frames continuously: log the first RAW_DUMP_N verbatim as bring-up
 * evidence, then aggregate into the id table (per-frame logging at ~10^3
 * frames/s would swamp the console and drop frames, proving nothing). */
static void rx_task(void *arg)
{
    twai_message_t m;
    uint32_t dumped = 0;

    while (1) {
        if (twai_receive(&m, pdMS_TO_TICKS(1000)) != ESP_OK) continue;

        if (dumped < RAW_DUMP_N) {
            char hex[32];
            frame_hex(m.data, m.data_length_code, hex, sizeof(hex));
            ESP_LOGI(TAG, "raw[%2lu] id=0x%03X%s dlc=%u data=%s",
                     (unsigned long)dumped, (unsigned)m.identifier,
                     m.extd ? " (ext)" : "", m.data_length_code, hex);
            if (++dumped == RAW_DUMP_N)
                ESP_LOGI(TAG, "--- raw dump done, switching to aggregated reports ---");
        }

        xSemaphoreTake(s_tbl_mutex, portMAX_DELAY);
        table_add(&m);
        xSemaphoreGive(s_tbl_mutex);
    }
}

/* Print a status block + the whole id table every REPORT_MS. */
static void report_task(void *arg)
{
    int64_t prev_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_MS));

        int64_t now_us  = esp_timer_get_time();
        float   elapsed = (float)(now_us - prev_us) / 1000000.0f;
        prev_us = now_us;

        uint32_t alerts = 0;
        twai_read_alerts(&alerts, 0);

        twai_status_info_t st;
        twai_get_status_info(&st);
        const char *state = (st.state == TWAI_STATE_RUNNING)    ? "RUNNING"
                          : (st.state == TWAI_STATE_BUS_OFF)    ? "BUS_OFF"
                          : (st.state == TWAI_STATE_RECOVERING) ? "RECOVERING"
                                                                : "STOPPED";

        /* Only scalars are snapshotted here. The table itself is copied one
         * entry at a time below: a whole-table copy would need MAX_IDS *
         * sizeof(can_id_stat_t) bytes of stack (3 KB), which overflows the
         * task stack, and holding the mutex across it would stall rx_task. */
        xSemaphoreTake(s_tbl_mutex, portMAX_DELAY);
        uint32_t total   = s_total_rx;
        size_t   n       = s_tbl_n;
        uint32_t dropped = s_dropped_ids;
        xSemaphoreGive(s_tbl_mutex);

        ESP_LOGI(TAG, "===== @%s  state=%s  rx=%lu  ids=%u  bus_err=%lu rx_err=%lu "
                      "missed=%lu overrun=%lu%s",
                 s_locked_rate, state, (unsigned long)total, (unsigned)n,
                 (unsigned long)st.bus_error_count, (unsigned long)st.rx_error_counter,
                 (unsigned long)st.rx_missed_count, (unsigned long)st.rx_overrun_count,
                 dropped ? "  [ID TABLE FULL]" : "");

        if (alerts) {
            ESP_LOGW(TAG, "alerts:%s%s%s%s",
                     (alerts & TWAI_ALERT_BUS_ERROR)     ? " BUS_ERROR"     : "",
                     (alerts & TWAI_ALERT_ERR_PASS)      ? " ERR_PASSIVE"   : "",
                     (alerts & TWAI_ALERT_RX_QUEUE_FULL) ? " RX_QUEUE_FULL" : "",
                     (alerts & TWAI_ALERT_BUS_OFF)       ? " BUS_OFF"       : "");
        }

        if (n == 0) {
            ESP_LOGW(TAG, "no frames yet - check wiring (OBD pin6=CAN-H, pin14=CAN-L), "
                          "termination jumper OFF on a vehicle, and that the car is READY");
            continue;
        }

        /* Entries are only ever appended, never moved or removed, so indexing
         * up to the snapshotted n stays valid; ids discovered while we print
         * simply show up in the next report. */
        for (size_t i = 0; i < n; i++) {
            can_id_stat_t e;
            xSemaphoreTake(s_tbl_mutex, portMAX_DELAY);
            e = s_tbl[i];
            s_tbl[i].prev_count = s_tbl[i].count;
            xSemaphoreGive(s_tbl_mutex);

            char hex[32];
            frame_hex(e.data, e.dlc, hex, sizeof(hex));
            float rate = (elapsed > 0) ? (e.count - e.prev_count) / elapsed : 0;
            ESP_LOGI(TAG, "  id=0x%03X%s n=%-6lu %5.1f/s  dlc=%u  %s",
                     (unsigned)e.id, e.extd ? "e" : " ",
                     (unsigned long)e.count, rate, e.dlc, hex);
        }
    }
}

#endif /* RUN_MODE */

/* ================================= main ================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "MAXUS 9 EV - Phase 3 Step 1: native CAN bring-up");
    ESP_LOGI(TAG, "TWAI TX=GPIO%d RX=GPIO%d", CAN_TX_GPIO, CAN_RX_GPIO);

#if RUN_MODE == MODE_SELFTEST
    run_selftest();
    ESP_LOGI(TAG, "Self-test finished. Rebuild with RUN_MODE MODE_LISTEN for the car.");
#elif RUN_MODE == MODE_ACTIVE
    run_active_uds();   /* never returns */
#elif RUN_MODE == MODE_SCAN
    run_scan();         /* never returns */
#elif RUN_MODE == MODE_DASH
    run_dashboard();    /* starts tasks and returns */
#else
    ESP_LOGI(TAG, "Mode: LISTEN_ONLY (cannot transmit or ACK - vehicle safe)");

    s_tbl_mutex = xSemaphoreCreateMutex();
    if (!s_tbl_mutex) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return;
    }

    probe_bitrate();   /* leaves the driver installed and started */

    xTaskCreate(rx_task,     "can_rx",     4096, NULL, 10, NULL);
    xTaskCreate(report_task, "can_report", 4096, NULL,  5, NULL);
    ESP_LOGI(TAG, "Sniffing. Report every %d ms.", REPORT_MS);
#endif
}
