/*
 * Read-only safety gate. See docs/inspection-safety.md for the policy.
 * Every command to the ELM327 passes safety_cmd_allowed() inside elm_cmd().
 */

#include "safety.h"
#include <string.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "safety";

#define CMD_MAX     40    /* longest command we ever normalise              */
#define REQ_MAX_LEN 8     /* longest request: one single frame (+1 spare)   */
#define VIN_LEN     17

/* AT commands that only configure how the adapter reads. The name is the run
 * of letters after "AT" and must match exactly. A parameter, if any, must be
 * 0-8 hex digits, optionally after one space. Everything else is refused,
 * which covers WM/SW (adapter transmits by itself), PP (adapter EEPROM), SD,
 * D, BRD and so on. */
static const char *AT_ALLOWED[] = {
    "Z", "I", "RV", "DPN",            /* reset, identify, 12 V, protocol   */
    "E", "L", "S", "H",               /* echo, linefeeds, spaces, headers  */
    "AT", "ST", "SP",                 /* adaptive timing, timeout, protocol */
    "SH", "CRA", "CF", "CM",          /* request header, receive filters   */
    "CSM", "MA",                      /* silent monitoring, monitor (listen) */
};

static bool at_allowed(const char *s)      /* s points just after "AT" */
{
    char name[8];
    size_t n = 0;
    while (s[n] >= 'A' && s[n] <= 'Z') {
        if (n >= sizeof(name) - 1) return false;
        name[n] = s[n];
        n++;
    }
    name[n] = '\0';
    if (n == 0) return false;

    bool known = false;
    for (size_t i = 0; i < sizeof(AT_ALLOWED) / sizeof(AT_ALLOWED[0]); i++)
        if (strcmp(name, AT_ALLOWED[i]) == 0) { known = true; break; }
    if (!known) return false;

    const char *p = s + n;
    if (*p == ' ') p++;
    size_t digits = 0;
    for (; *p; p++, digits++)
        if (!isxdigit((unsigned char)*p) || digits >= 8) return false;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Hex request text -> bytes. -1 on any non-hex character or an odd digit
 * count. Spaces are skipped. */
static int parse_hex(const char *s, uint8_t *out, int max)
{
    int n = 0, hi = -1;
    for (; *s; s++) {
        if (*s == ' ') continue;
        int v = hexval(*s);
        if (v < 0) return -1;
        if (hi < 0) { hi = v; continue; }
        if (n >= max) return -1;
        out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    return hi < 0 ? n : -1;
}

static bool request_allowed(const uint8_t *b, int n)
{
    if (n < 1 || n > REQ_MAX_LEN) return false;
    switch (b[0]) {
    case 0x03:                         /* OBD stored DTCs     (read-only)  */
    case 0x07:                         /* OBD pending DTCs    (read-only)  */
    case 0x0A:                         /* OBD permanent DTCs  (read-only)  */
        return n == 1;
    case 0x19:                         /* UDS ReadDTCInformation, reads only */
        if (n == 3 && (b[1] == 0x01 || b[1] == 0x02)) return true;   /* by status mask */
        if (n == 2 && b[1] == 0x0A) return true;                     /* supported DTCs */
        return false;
    default:
        break;
    }
    if (n < 2) return false;
    switch (b[0]) {
    case 0x01:                         /* OBD current data                 */
        return true;
    case 0x09:                         /* OBD vehicle info, but never VIN  */
        return b[1] != 0x02;
    case 0x22:                         /* UDS ReadDataByIdentifier         */
        if ((n - 1) % 2 != 0) return false;
        for (int i = 1; i + 1 < n; i += 2)
            if (b[i] == 0xF1 && b[i + 1] == 0x90) return false;   /* VIN */
        return true;
    default:                           /* every other service is refused   */
        return false;
    }
}

bool safety_cmd_allowed(const char *cmd)
{
    if (!cmd) return false;

    /* Normalise: uppercase, and refuse anything but letters, digits, spaces.
     * That rejects embedded CR/LF, which could smuggle a second command. */
    char s[CMD_MAX + 1];
    size_t n = 0;
    for (; *cmd; cmd++) {
        if (n >= CMD_MAX) return false;
        char c = (char)toupper((unsigned char)*cmd);
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == ' ')) return false;
        s[n++] = c;
    }
    s[n] = '\0';

    /* An empty line makes the ELM327 repeat the previous command. */
    size_t first = 0;
    while (s[first] == ' ') first++;
    if (s[first] == '\0') return false;

    if (s[first] == 'A' && s[first + 1] == 'T') return at_allowed(s + first + 2);

    uint8_t b[REQ_MAX_LEN + 1];
    int len = parse_hex(s, b, REQ_MAX_LEN + 1);
    if (len < 0) return false;
    return request_allowed(b, len);
}

static bool is_vin_char(uint8_t c)
{
    if (c >= '0' && c <= '9') return true;
    if (c < 'A' || c > 'Z') return false;
    return c != 'I' && c != 'O' && c != 'Q';
}

int safety_scrub_vin(uint8_t *d, int n)
{
    int masked = 0, i = 0;
    while (i < n) {
        int j = i;
        while (j < n && is_vin_char(d[j])) j++;
        if (j - i >= VIN_LEN) {
            memset(d + i, '*', (size_t)(j - i));
            masked += j - i;
        }
        i = (j == i) ? i + 1 : j;
    }
    return masked;
}

/* ============================== self-test ============================== */

static const char *ALLOWED_CASES[] = {
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATAT2", "ATST96", "ATSP6", "ATSP0",
    "ATSH 7E5", "ATCRA 7ED", "ATCF 7E8", "ATCM 7F8", "ATCSM1", "ATMA", "ATDPN",
    "ATRV", "ATI", "atsh 7e5", "ATSH7E5",
    "01 0C", "0100", "22 B0 61", "22 B0 61 B0 62", "09 04", "09 00",
    /* DTC reads, opened 2026-11-17 (H9) */
    "03", "07", "0A", "19 02 FF", "19 01 FF", "19 0A", "ATSH 7DF", "ATCRA",
};

static const char *BLOCKED_CASES[] = {
    /* services that change or clear things */
    "04", "14 FF FF FF", "10 03", "11 01", "27 01", "2E B4 80 02", "2F 01 02 03",
    "31 01 FF 00", "3E 00", "28 03 01", "85 02", "34 00 44",
    /* DTC: only the exact read forms pass; clearing never does */
    "03 00", "07 01", "0A 00", "19 02", "19 02 FF 00", "19 04 0A 80 00 FF",
    "19 06 0A 80 00 FF", "19 14", "14 FF FF", "04 00",
    /* VIN requests */
    "22 F1 90", "22 B0 61 F1 90", "09 02",
    /* adapter commands that transmit, write, or change the link */
    "ATWM 11", "ATSW 20", "ATPP FF OFF", "ATPP FF SV", "ATSD 01", "ATD", "ATBRD 10",
    /* A parameter that starts with a letter merges into the name ("SHE"),
     * so it is refused unless written with a space: fail-closed by design. */
    "ATSHE40",
    /* malformed / smuggled */
    "", "   ", "22", "22 B0", "01", "ZZ", "22 B0 61\r04", "ATZ;04",
    "ATSP6\r10 03",
};

int safety_selftest(void)
{
    int fails = 0, total = 0;

    for (size_t i = 0; i < sizeof(ALLOWED_CASES) / sizeof(ALLOWED_CASES[0]); i++, total++) {
        bool ok = safety_cmd_allowed(ALLOWED_CASES[i]);
        if (!ok) { fails++; ESP_LOGE(TAG, "selftest FAIL (should pass)  : \"%s\"", ALLOWED_CASES[i]); }
    }
    for (size_t i = 0; i < sizeof(BLOCKED_CASES) / sizeof(BLOCKED_CASES[0]); i++, total++) {
        bool ok = !safety_cmd_allowed(BLOCKED_CASES[i]);
        if (!ok) { fails++; ESP_LOGE(TAG, "selftest FAIL (should block) : \"%s\"", BLOCKED_CASES[i]); }
    }

    /* VIN scrub: a SYNTHETIC 17-byte VIN, never a real one. */
    {
        const char *fake = "ABCDEFGHJ12345678";
        uint8_t d[24];
        memset(d, 0x01, 4);
        memcpy(d + 4, fake, VIN_LEN);
        memset(d + 4 + VIN_LEN, 0x00, 3);
        int m = safety_scrub_vin(d, 24);
        bool ok = (m == VIN_LEN) && d[3] == 0x01 && d[4] == '*' && d[4 + VIN_LEN - 1] == '*' &&
                  d[4 + VIN_LEN] == 0x00;
        total++; if (!ok) { fails++; ESP_LOGE(TAG, "selftest FAIL: VIN not masked exactly"); }
    }
    {   /* 16 characters is not a VIN: left alone */
        uint8_t d[16];
        memcpy(d, "ABCDEFGHJ1234567", 16);
        int m = safety_scrub_vin(d, 16);
        total++; if (m != 0 || d[0] != 'A') { fails++; ESP_LOGE(TAG, "selftest FAIL: 16 bytes masked"); }
    }
    {   /* a normal numeric reply is untouched */
        uint8_t d[6] = {0x00, 0xF1, 0xD7, 0x24, 0xD6, 0x9C};
        int m = safety_scrub_vin(d, 6);
        total++; if (m != 0 || d[1] != 0xF1) { fails++; ESP_LOGE(TAG, "selftest FAIL: numeric data masked"); }
    }
    {   /* I, O, Q break the run (not in the VIN alphabet) */
        uint8_t d[17];
        memcpy(d, "ABCDEFGHIJ1234567", 17);       /* contains an 'I' */
        int m = safety_scrub_vin(d, 17);
        total++; if (m != 0) { fails++; ESP_LOGE(TAG, "selftest FAIL: run with 'I' masked"); }
    }

    ESP_LOGI(TAG, "safety selftest: %d/%d PASS (regression guard, not a vehicle test)",
             total - fails, total);
    return fails;
}
