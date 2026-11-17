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
    return uds_parse_id(raw, 3);
}

uds_result_t uds_parse_id(const char *raw, int idl)
{
    uds_result_t r = {0};
    r.nrc_code = -1;
    r.nrc_name = "";
    if (idl != 3 && idl != 8) return r;           /* 11-bit or 29-bit only */
    const size_t idz = (size_t)idl;
    const size_t mf_frame_chars = idz + 16;       /* id + 8 data bytes as hex */

    /* Multi-frame assembly state. */
    uint8_t pl[ISOTP_MAX];
    int  mf_total = 0, mf_have = 0, mf_next_sn = 0;
    bool mf_active = false;
    char mf_id[8] = {0};

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
        while (tlen - pos >= idz + 2) {
            const char *f = start + pos;
            const char *p = f + idz;                     /* PCI byte as hex */
            size_t rem = tlen - pos;
            int pci = hexnib(p[0]);
            size_t flen;
            if (pci == 0) {
                int n = hexnib(p[1]);
                flen = idz + 2 + 2 * (size_t)n;
                if (flen > rem) flen = rem;
            } else {
                flen = rem < mf_frame_chars ? rem : mf_frame_chars;
            }
            pos += flen;
            r.valid = true;

            if (pci == 0) {                                  /* single frame */
                int n = hexnib(p[1]);
                int got = take_bytes(p + 2, flen - idz - 2, pl, n);
                mf_active = false;
                if (interpret(pl, got, &r)) return r;
            } else if (pci == 1 && flen >= idz + 4) {        /* first frame */
                mf_total = (hexnib(p[1]) << 8) | hexbyte(p + 2);
                if (mf_total > ISOTP_MAX) mf_total = ISOTP_MAX;
                mf_have = take_bytes(p + 4, flen - idz - 4, pl, mf_total < 6 ? mf_total : 6);
                mf_next_sn = 1;
                mf_active = true;
                memcpy(mf_id, f, idz);
            } else if (pci == 2 && mf_active && memcmp(mf_id, f, idz) == 0) {
                int sn = hexnib(p[1]);
                if (sn != mf_next_sn) {                      /* lost a frame */
                    ESP_LOGW(TAG, "ISO-TP sequence %d, expected %d - dropped", sn, mf_next_sn);
                    mf_active = false;
                    continue;
                }
                int want = mf_total - mf_have;
                if (want > 7) want = 7;
                mf_have += take_bytes(p + 2, flen - idz - 2, pl + mf_have, want);
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

/* ===================== multi-ECU collection (H9) ===================== */

typedef struct { int total, next_sn; bool active, done; } isotp_asm_t;

int isotp_collect(const char *raw, int idl, isotp_msg_t *out, int max)
{
    if ((idl != 3 && idl != 8) || max <= 0) return 0;
    if (max > ISOTP_COLLECT_MAX) max = ISOTP_COLLECT_MAX;
    const size_t idz = (size_t)idl;
    const size_t mf_frame_chars = idz + 16;
    isotp_asm_t st[ISOTP_COLLECT_MAX];
    memset(st, 0, sizeof(st));
    int used = 0;

    const char *t = raw;
    while (*t) {
        while (*t && isspace((unsigned char)*t)) t++;
        const char *start = t;
        bool all_hex = true;
        while (*t && !isspace((unsigned char)*t)) {
            if (hexnib(*t) < 0) all_hex = false;
            t++;
        }
        size_t tlen = (size_t)(t - start);
        if (tlen == 0 || !all_hex) continue;

        size_t pos = 0;
        while (tlen - pos >= idz + 2) {
            const char *f = start + pos;
            const char *p = f + idz;
            size_t rem = tlen - pos;
            int pci = hexnib(p[0]);
            size_t flen;
            if (pci == 0) {
                flen = idz + 2 + 2 * (size_t)hexnib(p[1]);
                if (flen > rem) flen = rem;
            } else {
                flen = rem < mf_frame_chars ? rem : mf_frame_chars;
            }
            pos += flen;

            if (pci == 0 || pci == 1) {                  /* a new message starts */
                if (used >= max) continue;
                int k = used++;
                memset(&out[k], 0, sizeof(out[k]));
                memcpy(out[k].id, f, idz);
                st[k] = (isotp_asm_t){0};
                if (pci == 0) {
                    out[k].len = take_bytes(p + 2, flen - idz - 2, out[k].data, hexnib(p[1]));
                    st[k].done = true;
                } else if (flen >= idz + 4) {
                    st[k].total = (hexnib(p[1]) << 8) | hexbyte(p + 2);
                    if (st[k].total > ISOTP_MSG_MAX) st[k].total = ISOTP_MSG_MAX;
                    out[k].len = take_bytes(p + 4, flen - idz - 4, out[k].data,
                                            st[k].total < 6 ? st[k].total : 6);
                    st[k].next_sn = 1;
                    st[k].active = true;
                } else {
                    used--;                              /* malformed first frame */
                    continue;
                }
            } else if (pci == 2) {                       /* continue this id's message */
                for (int k = used - 1; k >= 0; k--) {
                    if (!st[k].active || memcmp(out[k].id, f, idz) != 0) continue;
                    int sn = hexnib(p[1]);
                    if (sn != st[k].next_sn) {           /* lost a frame: abandon it */
                        st[k].active = false;
                        break;
                    }
                    int want = st[k].total - out[k].len;
                    if (want > 7) want = 7;
                    out[k].len += take_bytes(p + 2, flen - idz - 2, out[k].data + out[k].len, want);
                    st[k].next_sn = (st[k].next_sn + 1) & 0xF;
                    if (out[k].len >= st[k].total) { st[k].active = false; st[k].done = true; }
                    break;
                }
            }
        }
    }

    /* Keep only complete messages, and drop responsePending (7F xx 78). */
    int n = 0;
    for (int k = 0; k < used; k++) {
        if (!st[k].done) continue;
        if (out[k].len >= 3 && out[k].data[0] == 0x7F && out[k].data[2] == 0x78) continue;
        if (n != k) out[n] = out[k];
        n++;
    }
    return n;
}

void dtc_format(uint8_t hi, uint8_t lo, char out[6])
{
    static const char LETTER[] = "PCBU";
    static const char HX[] = "0123456789ABCDEF";
    out[0] = LETTER[hi >> 6];
    out[1] = HX[(hi >> 4) & 0x3];
    out[2] = HX[hi & 0xF];
    out[3] = HX[lo >> 4];
    out[4] = HX[lo & 0xF];
    out[5] = '\0';
}

/* Synthetic replies only: no DTC reply has been captured on any car yet. */
static int dtc_selftest(int *total)
{
    int fails = 0;
    static isotp_msg_t m[ISOTP_COLLECT_MAX];
    char code[6];

    /* dtc_format: one code per letter class */
    struct { uint8_t a, b; const char *want; } F[] = {
        {0x01, 0x33, "P0133"}, {0x0A, 0x80, "P0A80"}, {0x41, 0x23, "C0123"},
        {0x81, 0x23, "B0123"}, {0xC1, 0x23, "U0123"}, {0xFF, 0xFF, "U3FFF"},
    };
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        dtc_format(F[i].a, F[i].b, code);
        (*total)++;
        if (strcmp(code, F[i].want) != 0) {
            fails++;
            ESP_LOGE(TAG, "dtc selftest FAIL: %02X%02X -> %s, want %s", F[i].a, F[i].b, code, F[i].want);
        }
    }

    /* Two ECUs answer a functional 03: one DTC from 7E8, none from 7EA. */
    int n = isotp_collect("7E80443010133 7EA024300", 3, m, ISOTP_COLLECT_MAX);
    (*total)++;
    if (!(n == 2 && strcmp(m[0].id, "7E8") == 0 && m[0].len == 4 && m[0].data[0] == 0x43 &&
          m[0].data[1] == 1 && m[0].data[2] == 0x01 && m[0].data[3] == 0x33 &&
          strcmp(m[1].id, "7EA") == 0 && m[1].len == 2 && m[1].data[1] == 0)) {
        fails++;
        ESP_LOGE(TAG, "dtc selftest FAIL: functional 03 from two ECUs (n=%d)", n);
    }

    /* UDS 19 02 FF, multi-frame: 59 02 FF + P0A80-00 st 2F + U0123-45 st 09 */
    n = isotp_collect("7ED100B5902FF0A8000 7ED212FC1234509AAAA", 3, m, ISOTP_COLLECT_MAX);
    (*total)++;
    if (!(n == 1 && m[0].len == 11 && m[0].data[0] == 0x59 && m[0].data[3] == 0x0A &&
          m[0].data[6] == 0x2F && m[0].data[7] == 0xC1 && m[0].data[10] == 0x09)) {
        fails++;
        ESP_LOGE(TAG, "dtc selftest FAIL: 19 02 multi-frame (n=%d len=%d)", n, n ? m[0].len : -1);
    }

    /* responsePending, then the real (negative) answer: only the NRC 0x11 remains */
    n = isotp_collect("7ED037F1978 7ED037F1911", 3, m, ISOTP_COLLECT_MAX);
    (*total)++;
    if (!(n == 1 && m[0].data[0] == 0x7F && m[0].data[2] == 0x11)) {
        fails++;
        ESP_LOGE(TAG, "dtc selftest FAIL: 0x78 then NRC 0x11 (n=%d)", n);
    }

    /* Interleaved multi-frame from two ECUs (8-byte payloads: 43 03 + 3 DTCs);
     * a first frame always carries 6 data bytes. */
    n = isotp_collect("7E81008430301010202 7E91008430304040505 "
                      "7E8210333AAAAAAAAAA 7E9210644AAAAAAAAAA",
                      3, m, ISOTP_COLLECT_MAX);
    (*total)++;
    if (!(n == 2 && m[0].len == 8 && m[1].len == 8 && m[0].data[7] == 0x33 && m[1].data[7] == 0x44)) {
        fails++;
        ESP_LOGE(TAG, "dtc selftest FAIL: interleaved ECUs (n=%d)", n);
    }
    return fails;
}

/* ============================== self-test ============================== */

typedef struct {
    const char *label;
    const char *raw;
    bool        positive;
    const char *did;        /* when positive */
    const char *data_hex;   /* when positive */
    int         nrc;        /* -1 = none expected */
    int         idl;        /* id length in hex chars; 0 = 3 (11-bit) */
} uds_case_t;

/* Vectors marked "capture" are verbatim ELM327 replies from
 * captures/20260716_104946_7E5_B0_scan061-0FF.jsonl (stored there with no frame
 * separator). "capture, spaced" is the same bytes split the way the ESP32
 * sanitizer delivers them. "synthetic" cases are hand-built for paths the
 * capture does not contain. */
static const uds_case_t CASES[] = {
    {"capture: B061 single frame", "7ED0562B0612512", true, "B061", "2512", -1, 3},
    {"capture: B063 single frame, 3 bytes", "7ED0662B0630001FD", true, "B063", "0001FD", -1, 3},
    {"capture: B0A3 multi-frame, joined", "7ED100A62B0A31547157ED21548C8CA0AAAAAA",
     true, "B0A3", "154715548C8CA0", -1, 3},
    {"capture, spaced: B0A3 multi-frame", "7ED100A62B0A3154715 7ED21548C8CA0AAAAAA",
     true, "B0A3", "154715548C8CA0", -1, 3},
    {"capture, spaced: B0C3 multi-frame", "7ED100A62B0C3154E15 7ED21538C8CA4AAAAAA",
     true, "B0C3", "154E15538C8CA4", -1, 3},
    {"capture: B06B NRC 0x31", "7ED037F2231", false, NULL, NULL, 0x31, 3},
    {"synthetic: ELM status text before reply", "SEARCHING... 7ED0562B0612512",
     true, "B061", "2512", -1, 3},
    {"synthetic: 0x78 pending, then reply", "7ED037F2278 7ED0562B0612512",
     true, "B061", "2512", -1, 3},
    {"synthetic: first frame only (truncated)", "7ED100A62B0A3154715", false, NULL, NULL, -1, 3},
    {"synthetic: wrong sequence number", "7ED100A62B0A3154715 7ED22548C8CA0AAAAAA",
     false, NULL, NULL, -1, 3},
    {"synthetic: NO DATA", "NO DATA", false, NULL, NULL, -1, 3},
    /* 29-bit CAN (ATSP7/9): the id prints as 8 hex chars. No car has given us
     * a 29-bit capture yet, so these are synthetic: the 11-bit captures above
     * with the id swapped for 18DAF110. */
    {"synthetic 29-bit: single frame", "18DAF1100562B0612512",
     true, "B061", "2512", -1, 8},
    {"synthetic 29-bit: multi-frame, spaced",
     "18DAF110100A62B0A3154715 18DAF11021548C8CA0AAAAAA",
     true, "B0A3", "154715548C8CA0", -1, 8},
    {"synthetic 29-bit: NRC 0x31", "18DAF110037F2231", false, NULL, NULL, 0x31, 8},
};

int uds_selftest(void)
{
    int fails = 0, total = (int)(sizeof(CASES) / sizeof(CASES[0]));
    for (int i = 0; i < total; i++) {
        const uds_case_t *c = &CASES[i];
        uds_result_t r = uds_parse_id(c->raw, c->idl ? c->idl : 3);
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
    int dtc_total = 0;
    int dtc_fails = dtc_selftest(&dtc_total);
    fails += dtc_fails;
    total += dtc_total;
    ESP_LOGI(TAG, "uds selftest: %d/%d PASS (regression guard, not a vehicle test)",
             total - fails, total);
    return fails;
}
