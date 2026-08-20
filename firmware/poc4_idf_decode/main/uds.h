/*
 * UDS (ISO 14229) Service 22 response parser - C port of src/uds.py.
 * 11-bit CAN single-frame (ISO 15765-4, ATSP6) + NRC decoding.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define UDS_MAX_DATA 32

typedef struct {
    bool     valid;                 /* frame had a plausible structure       */
    bool     positive;             /* 62 + DID + data (a good read)          */
    int      nrc_code;             /* -1 if none; else the 7F NRC byte       */
    const char *nrc_name;          /* human name for nrc_code, "" if none    */
    char     did[5];               /* 4-char DID hex, e.g. "B042"            */
    uint8_t  data[UDS_MAX_DATA];   /* decoded data bytes                     */
    int      n;                    /* number of data bytes                   */
} uds_result_t;

/* Parse a sanitized reply string (may contain the CAN id header, spaces, CR).
 * Mirrors src/uds.py:parse - handles single frame (PCI 0N) + NRC (7F). */
uds_result_t uds_parse(const char *raw);

/* INT16 = data[0]*256 + data[1] (matches src/pids.py). */
static inline int uds_int16(const uds_result_t *r)
{
    return (r->n >= 2) ? (r->data[0] * 256 + r->data[1]) : -1;
}
