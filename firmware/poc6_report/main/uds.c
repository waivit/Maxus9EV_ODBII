/*
 * UDS Service 22 response parser for ELM327 text replies, with ISO-TP
 * reassembly. Extends the poc4 single-frame port of src/uds.py.
 *
 * Why reassembly is needed: the 12 BMS module groups (B001...) return 7 data
 * bytes = a 10-byte UDS payload, which cannot fit an ISO-TP single frame (max
 * 7). The ELM327 already sends the flow-control frame itself and prints every
 * frame on its own line (proven on 2026-07-16, docs/findings/11 correction), so
 * only the parsing side was missing.
 *
 * Reply shape with ATH1 + ATS0 + CAF on (one frame per line):
 *   single : 7ED 05 62 B0 61 25 12            -> "7ED0562B0612512"
 *   first  : 7ED 10 0A 62 B0 A3 15 47 15      -> 3-char id + 8 bytes
 *   consec : 7ED 21 54 8C 8C A0 AA AA AA      -> 3-char id + 8 bytes (padded)
 */

#include "uds.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "uds";

#define ISOTP_MAX 64              /* largest reassembled payload we keep       */
#define MF_FRAME_CHARS 19         /* 3-char id + 8 bytes as hex                */

typedef struct { int code; const char *name; } nrc_entry_t;
static const nrc_entry_t NRC_TABLE[] = {
    {0x10, "generalReject"},        {0x11, "serviceNotSupported"},
    {0x12, "subFunctionNotSupported"}, {0x22, "conditionsNotCorrect"},
    {0x24, "requestSequenceError"}, {0x31, "requestOutOfRange"},
    {0x33, "securityAccessDenied"}, {0x35, "invalidKey"},
    {0x78, "responsePending"},      {0x7E, "subFuncNotSupportedActiveSession"},
    {0x7F, "serviceNotSupportedActiveSession"},
};

static const char *nrc_name(int code)
{
    for (size_t i = 0; i < sizeof(NRC_TABLE) / sizeof(NRC_TABLE[0]); i++)
        if (NRC_TABLE[i].code == code) return NRC_TABLE[i].name;
    return "unknown";
}

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Two hex chars -> byte, or -1. */
static int hexbyte(const char *p)
{
    int hi = hexnib(p[0]), lo = hexnib(p[1]);
    return (hi < 0 || lo < 0) ? -1 : (hi << 4) | lo;
}

/* Copy up to max bytes of hex at p (len chars available) into out. */
static int take_bytes(const char *p, size_t len, uint8_t *out, int max)
{
    int got = 0;
    while (got < max && 2 * (size_t)got + 2 <= len) {
        int b = hexbyte(p + 2 * got);
        if (b < 0) break;
        out[got++] = (uint8_t)b;
    }
    return got;
}

/* Interpret one complete UDS payload. Returns true if it is final (anything
 * but a responsePending), so the caller stops looking at further frames. */
static bool interpret(const uint8_t *pl, int len, uds_result_t *r)
{
    if (len < 1) return false;
    if (pl[0] == 0x7F) {
        if (len >= 3 && pl[2] == 0x78) return false;   /* ECU asks for more time */
        if (len >= 3) { r->nrc_code = pl[2]; r->nrc_name = nrc_name(pl[2]); }
        return true;
    }
    if (pl[0] == 0x62 && len >= 3) {
        r->positive = true;
        static const char HX[] = "0123456789ABCDEF";
        r->did[0] = HX[pl[1] >> 4]; r->did[1] = HX[pl[1] & 0xF];
        r->did[2] = HX[pl[2] >> 4]; r->did[3] = HX[pl[2] & 0xF];
        r->did[4] = '\0';
        /* The ISO-TP length bounds the payload, so trailing AA padding is never
         * included. (poc4 stripped AA bytes instead, which would also eat a real
         * trailing 0xAA data byte.) */
        for (int j = 3; j < len && r->n < UDS_MAX_DATA; j++) r->data[r->n++] = pl[j];
        return true;
    }
    return true;   /* some other service byte: stop, not positive */
}

uds_result_t uds_parse(const char *raw)
{
    uds_result_t r = {0};
    r.nrc_code = -1;
    r.nrc_name = "";

    /* Multi-frame assembly state. */
    uint8_t pl[ISOTP_MAX];
    int  mf_total = 0, mf_have = 0, mf_next_sn = 0;
    bool mf_active = false;
    char mf_id[3] = {0};

    const char *t = raw;
    while (*t) {
        /* Next whitespace-separated token. */
        while (*t && isspace((unsigned char)*t)) t++;
        const char *start = t;
        bool all_hex = true;
        while (*t && !isspace((unsigned char)*t)) {
            if (hexnib(*t) < 0) all_hex = false;
            t++;
        }
        size_t tlen = (size_t)(t - start);
        if (tlen == 0 || !all_hex) continue;

        /* A token may hold several frames back to back (no separator). Walk it
         * frame by frame using the PCI to find each frame's length. */
        size_t pos = 0;
        while (tlen - pos >= 5) {
            const char *f = start + pos;
            size_t rem = tlen - pos;
            int pci = hexnib(f[3]);
            size_t flen;
            if (pci == 0) {
                int n = hexnib(f[4]);
                flen = 5 + 2 * (size_t)n;
                if (flen > rem) flen = rem;
            } else {
                flen = rem < MF_FRAME_CHARS ? rem : MF_FRAME_CHARS;
            }
            pos += flen;
            r.valid = true;

            if (pci == 0) {                                  /* single frame */
                int n = hexnib(f[4]);
                int got = take_bytes(f + 5, flen - 5, pl, n);
                mf_active = false;
                if (interpret(pl, got, &r)) return r;
            } else if (pci == 1 && flen >= 7) {              /* first frame */
                mf_total = (hexnib(f[4]) << 8) | hexbyte(f + 5);
                if (mf_total > ISOTP_MAX) mf_total = ISOTP_MAX;
                mf_have = take_bytes(f + 7, flen - 7, pl, mf_total < 6 ? mf_total : 6);
                mf_next_sn = 1;
                mf_active = true;
                memcpy(mf_id, f, 3);
            } else if (pci == 2 && mf_active && memcmp(mf_id, f, 3) == 0) {
                int sn = hexnib(f[4]);
                if (sn != mf_next_sn) {                      /* lost a frame */
                    ESP_LOGW(TAG, "ISO-TP sequence %d, expected %d - dropped", sn, mf_next_sn);
                    mf_active = false;
                    continue;
                }
                int want = mf_total - mf_have;
                if (want > 7) want = 7;
                mf_have += take_bytes(f + 5, flen - 5, pl + mf_have, want);
                mf_next_sn = (mf_next_sn + 1) & 0xF;
                if (mf_have >= mf_total) {
                    mf_active = false;
                    if (interpret(pl, mf_have, &r)) return r;
                }
            }
            /* anything else (flow control, stray id, padding): ignore */
        }
    }
    /* An unfinished multi-frame message is not a result. */
    return r;
}

/* ============================== self-test ============================== */

typedef struct {
    const char *label;
    const char *raw;
    bool        positive;
    const char *did;        /* when positive */
    const char *data_hex;   /* when positive */
    int         nrc;        /* -1 = none expected */
} uds_case_t;

/* Vectors marked "capture" are verbatim ELM327 replies from
 * captures/20260716_104946_7E5_B0_scan061-0FF.jsonl (stored there with no frame
 * separator). "capture, spaced" is the same bytes split the way the ESP32
 * sanitizer delivers them. "synthetic" cases are hand-built for paths the
 * capture does not contain. */
static const uds_case_t CASES[] = {
    {"capture: B061 single frame", "7ED0562B0612512", true, "B061", "2512", -1},
    {"capture: B063 single frame, 3 bytes", "7ED0662B0630001FD", true, "B063", "0001FD", -1},
    {"capture: B0A3 multi-frame, joined", "7ED100A62B0A31547157ED21548C8CA0AAAAAA",
     true, "B0A3", "154715548C8CA0", -1},
    {"capture, spaced: B0A3 multi-frame", "7ED100A62B0A3154715 7ED21548C8CA0AAAAAA",
     true, "B0A3", "154715548C8CA0", -1},
    {"capture, spaced: B0C3 multi-frame", "7ED100A62B0C3154E15 7ED21538C8CA4AAAAAA",
     true, "B0C3", "154E15538C8CA4", -1},
    {"capture: B06B NRC 0x31", "7ED037F2231", false, NULL, NULL, 0x31},
    {"synthetic: ELM status text before reply", "SEARCHING... 7ED0562B0612512",
     true, "B061", "2512", -1},
    {"synthetic: 0x78 pending, then reply", "7ED037F2278 7ED0562B0612512",
     true, "B061", "2512", -1},
    {"synthetic: first frame only (truncated)", "7ED100A62B0A3154715", false, NULL, NULL, -1},
    {"synthetic: wrong sequence number", "7ED100A62B0A3154715 7ED22548C8CA0AAAAAA",
     false, NULL, NULL, -1},
    {"synthetic: NO DATA", "NO DATA", false, NULL, NULL, -1},
};

int uds_selftest(void)
{
    int fails = 0, total = (int)(sizeof(CASES) / sizeof(CASES[0]));
    for (int i = 0; i < total; i++) {
        const uds_case_t *c = &CASES[i];
        uds_result_t r = uds_parse(c->raw);
        char hex[2 * UDS_MAX_DATA + 1] = {0};
        for (int k = 0; k < r.n; k++) {
            static const char HX[] = "0123456789ABCDEF";
            hex[2 * k] = HX[r.data[k] >> 4];
            hex[2 * k + 1] = HX[r.data[k] & 0xF];
        }
        bool ok = (r.positive == c->positive) && (r.nrc_code == c->nrc);
        if (ok && c->positive)
            ok = strcmp(r.did, c->did) == 0 && strcmp(hex, c->data_hex) == 0;
        if (!ok) fails++;
        ESP_LOGI(TAG, "selftest %-4s %s -> pos=%d did=%s data=%s nrc=%d",
                 ok ? "ok" : "FAIL", c->label, r.positive, r.did, hex, r.nrc_code);
    }
    ESP_LOGI(TAG, "uds selftest: %d/%d PASS (regression guard, not a vehicle test)",
             total - fails, total);
    return fails;
}
