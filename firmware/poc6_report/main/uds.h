/*
 * UDS (ISO 14229) Service 22 response parser for ELM327 text replies.
 * 11-bit CAN (ISO 15765-4, ATSP6, ATH1 + ATS0) with ISO-TP reassembly:
 * single frame, or first frame + consecutive frames. NRC decoding included.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define UDS_MAX_DATA 32

typedef struct {
    bool     valid;                 /* at least one frame had a plausible shape */
    bool     positive;             /* 62 + DID + data (a good read)            */
    int      nrc_code;             /* -1 if none; else the 7F NRC byte         */
    const char *nrc_name;          /* human name for nrc_code, "" if none      */
    char     did[5];               /* 4-char DID hex, e.g. "B042"              */
    uint8_t  data[UDS_MAX_DATA];   /* decoded data bytes (after the DID)       */
    int      n;                    /* number of data bytes                     */
} uds_result_t;

/* Parse a sanitized ELM327 reply. Frames may be separated by whitespace (as the
 * ESP32 sanitizer turns CR into spaces) or concatenated with no separator (as
 * stored by the Python scanner). Tokens with non-hex characters (SEARCHING...,
 * NO DATA) are ignored. A responsePending (7F xx 78) frame is skipped so that
 * the real reply following it in the same read is used. */
uds_result_t uds_parse(const char *raw);

/* Same, with an explicit CAN id length in hex characters: 3 for 11-bit
 * (ATSP6/8), 8 for 29-bit (ATSP7/9). uds_parse() is uds_parse_id(raw, 3). */
uds_result_t uds_parse_id(const char *raw, int id_chars);

/* Run the parser against captured reply vectors; logs each case and returns the
 * number of failures. A regression guard for the parser, not a vehicle test. */
int uds_selftest(void);

/* INT16 = data[0]*256 + data[1] (matches src/pids.py). */
static inline int uds_int16(const uds_result_t *r)
{
    return (r->n >= 2) ? (r->data[0] * 256 + r->data[1]) : -1;
}
