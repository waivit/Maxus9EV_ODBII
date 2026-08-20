/*
 * UDS Service 22 response parser - C port of src/uds.py:parse().
 * Ports the 11-bit single-frame path + NRC decoding. The multi-frame (PCI 1)
 * path from the Python version is NOT ported: no multi-frame PID has ever been
 * captured on this vehicle, so it stays a documented gap (see src/uds.py).
 */

#include "uds.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

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

/* Convert 2 hex chars at p to a byte; return -1 on non-hex. */
static int hexbyte(const char *p)
{
    char h[3] = {p[0], p[1], 0};
    for (int i = 0; i < 2; i++)
        if (!isxdigit((unsigned char)h[i])) return -1;
    return (int)strtol(h, NULL, 16);
}

uds_result_t uds_parse(const char *raw)
{
    uds_result_t r = {0};
    r.nrc_code = -1;
    r.nrc_name = "";

    /* Sanitize: keep only hex chars, uppercase (drop spaces/CR/LF). */
    char s[128];
    size_t si = 0;
    for (const char *c = raw; *c && si < sizeof(s) - 1; c++) {
        if (isxdigit((unsigned char)*c)) s[si++] = (char)toupper((unsigned char)*c);
    }
    s[si] = '\0';

    /* Need at least id(3) + PCI(2) + one service byte(2). */
    if (si < 7) return r;
    r.valid = true;

    /* s[0..2] = 11-bit CAN id; s[3] = ISO-TP frame-type nibble. */
    char pci_hi = s[3];
    char payload[128];
    size_t pi = 0;

    if (pci_hi == '0') {
        /* Single frame: low PCI nibble = payload byte count. */
        char nch[2] = {s[4], 0};
        if (!isxdigit((unsigned char)nch[0])) return r;
        int n = (int)strtol(nch, NULL, 16);
        for (int k = 0; k < n * 2 && (5 + k) < (int)si; k++)
            payload[pi++] = s[5 + k];
    } else {
        /* Multi-frame not supported here; best-effort skip id+PCI. */
        for (size_t k = 5; k < si; k++) payload[pi++] = s[k];
    }
    payload[pi] = '\0';

    /* Strip trailing AA padding. */
    while (pi >= 2 && payload[pi - 2] == 'A' && payload[pi - 1] == 'A') {
        pi -= 2;
        payload[pi] = '\0';
    }
    if (pi < 2) return r;

    /* Service byte. */
    if (payload[0] == '7' && payload[1] == 'F') {
        if (pi >= 6) {
            int nrc = hexbyte(payload + 4);
            if (nrc >= 0) { r.nrc_code = nrc; r.nrc_name = nrc_name(nrc); }
        }
        return r;
    }

    if (payload[0] == '6' && payload[1] == '2' && pi >= 6) {
        r.positive = true;
        memcpy(r.did, payload + 2, 4);
        r.did[4] = '\0';
        for (size_t j = 6; j + 1 < pi && r.n < UDS_MAX_DATA; j += 2) {
            int b = hexbyte(payload + j);
            if (b < 0) break;
            r.data[r.n++] = (uint8_t)b;
        }
    }
    return r;
}
