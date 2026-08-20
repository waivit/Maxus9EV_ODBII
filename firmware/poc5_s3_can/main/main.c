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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/twai.h"

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

#define RUN_MODE MODE_SCAN

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

#elif RUN_MODE == MODE_ACTIVE || RUN_MODE == MODE_SCAN   /* ==== Active UDS ==== */

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

/* ============================== ECU / DID scan ============================ */
#if RUN_MODE == MODE_SCAN

/* Diagnostic request range. Hard limit - see the guard in uds_xfer(). */
#define SCAN_REQ_LO   0x700
#define SCAN_REQ_HI   0x7FF

/* This vehicle answers on far more addresses than the three the ELM327 path
 * knew about (33 found on the first run), so the table has to be generous. */
#define SCAN_MAX_ECUS   48

/* Phase B sweeps only ECUs at or above this address. Phase A always reports the
 * full map. Set it to resume a scan that was split across runs instead of
 * re-sweeping addresses already covered. */
#define SCAN_SWEEP_FROM 0x73E

/* DID blocks worth sweeping. The B0/B3/B4/B7/B9/BA pages are where every PID we
 * have confirmed so far lives (docs/findings/09-ovms-cross-reference.md); F1 is
 * the standard identification page; 00 is the generic OBD page. */
static const struct { uint16_t lo, hi; } DID_RANGES[] = {
    { 0xB000, 0xB0FF }, { 0xB300, 0xB3FF }, { 0xB400, 0xB4FF },
    { 0xB700, 0xB7FF }, { 0xB900, 0xB9FF }, { 0xBA00, 0xBAFF },
    { 0xF100, 0xF1FF }, { 0x0000, 0x00FF },
};
#define N_DID_RANGES (sizeof(DID_RANGES) / sizeof(DID_RANGES[0]))

/* Friendly names for the ECUs we already identified on the ELM327 path
 * (docs/findings/01-confirmed-pids.md). Unknown ones are reported by address. */
static const char *ecu_name(uint32_t req_id)
{
    switch (req_id) {
    case 0x7E3: return "VCU";
    case 0x7E4: return "GATEWAY";
    case 0x7E5: return "BMS";
    default:    return "UNKNOWN";
    }
}

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
