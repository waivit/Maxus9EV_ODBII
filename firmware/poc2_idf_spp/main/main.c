/*
 * MAXUS 9 EV - ESP32 Bridge (Phase 2)
 * Step 2 POC (ESP-IDF) - connect to the ELM327 over Bluetooth Classic SPP with
 * LEGACY PIN pairing (SSP disabled via sdkconfig.defaults), run the ELM327 AT
 * init sequence, and read one PID (Pack Voltage).
 *
 * This replaces the Arduino attempt (firmware/poc2_bt_elm327/), which proved
 * the adapter requires legacy PIN bonding (Windows pairing needs PIN 1234) -
 * a mode arduino-esp32 BluetoothSerial cannot reliably drive. Here SSP is off
 * (sdkconfig.defaults) and esp_bt_gap_set_pin() answers the legacy PIN request
 * directly, the same way Windows does.
 *
 * Read-only: only Service 22 reads, never writes to the vehicle.
 * Board: classic ESP32 (WROOM-32 / ESP32-D0WD-V3).
 */

#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "nvs_flash.h"

static const char *TAG = "poc2_spp";

/* Name substring that identifies the ELM327 adapter (case-insensitive). */
#define TARGET_NAME_HINT "OBD"

/* Fallback match by MAC: this adapter's basic inquiry result carries no
 * BDNAME (confirmed live - "found device: (no name)" for this exact address
 * across a full scan), so name matching alone never fires. Address confirmed
 * from the Arduino POC scan and this ESP-IDF scan alike. */
static const esp_bd_addr_t KNOWN_ELM327_BDA = {0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33};

/* ELM327 init sequence - ported verbatim from src/elm327.py:init /
 * firmware/poc2_bt_elm327/poc2_bt_elm327.ino. Followed by header select +
 * one PID read (Pack Voltage). Sent one at a time, each gated on the '>'
 * prompt, matching elmCmd()/readUntilPrompt() from the Arduino sketch. */
static const char *CMD_SEQUENCE[] = {
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATAT2", "ATST96", "ATSP6",
    "ATSH 7E5", "22 B0 42",
};
#define CMD_COUNT (sizeof(CMD_SEQUENCE) / sizeof(CMD_SEQUENCE[0]))

static bool s_discovery_done = false;
static bool s_target_found = false;
static esp_bd_addr_t s_target_bda;

static uint32_t s_spp_handle = 0;
static int s_cmd_index = -1; /* -1 = not started yet */

static char s_rx_buf[256];
static size_t s_rx_len = 0;

static void send_next_cmd(void)
{
    s_cmd_index++;
    if ((size_t)s_cmd_index >= CMD_COUNT) {
        ESP_LOGI(TAG, "Command sequence complete.");
        return;
    }
    const char *cmd = CMD_SEQUENCE[s_cmd_index];
    ESP_LOGI(TAG, "--> %s", cmd);
    s_rx_len = 0;
    memset(s_rx_buf, 0, sizeof(s_rx_buf));

    /* ELM327 commands are terminated with \r, mirroring elm327._send_raw. */
    char out[32];
    int n = snprintf(out, sizeof(out), "%s\r", cmd);
    esp_spp_write(s_spp_handle, n, (uint8_t *)out);
}

static bool name_looks_like_elm(const char *name)
{
    if (!name) {
        return false;
    }
    char up[64];
    size_t i;
    for (i = 0; i < sizeof(up) - 1 && name[i]; i++) {
        up[i] = (char)toupper((unsigned char)name[i]);
    }
    up[i] = '\0';
    return strstr(up, TARGET_NAME_HINT) != NULL;
}

static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        const char *name = NULL;
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                name = (const char *)p->val;
            }
        }
        ESP_LOGI(TAG, "  found device: %s  [%02x:%02x:%02x:%02x:%02x:%02x]",
                 name ? name : "(no name)",
                 param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
                 param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);
        bool mac_match = memcmp(param->disc_res.bda, KNOWN_ELM327_BDA, ESP_BD_ADDR_LEN) == 0;
        if (!s_target_found && (name_looks_like_elm(name) || mac_match)) {
            memcpy(s_target_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
            s_target_found = true;
            ESP_LOGI(TAG, "      ^ matches ELM327 (%s) - will connect",
                     mac_match ? "known MAC" : "name hint");
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_discovery_done = true;
            if (!s_target_found) {
                ESP_LOGW(TAG, "No ELM327-like device found. Note names/MACs above.");
                break;
            }
            ESP_LOGI(TAG, "Discovery done. Starting SPP SDP lookup on target...");
            esp_spp_start_discovery(s_target_bda);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT:
        /* Legacy PIN request. esp_bt_gap_set_pin(FIXED, ...) below normally
         * answers this automatically; logged here for visibility. */
        ESP_LOGI(TAG, "PIN requested by [%02x:%02x:%02x:%02x:%02x:%02x] (min_16_digit=%d)",
                 param->pin_req.bda[0], param->pin_req.bda[1], param->pin_req.bda[2],
                 param->pin_req.bda[3], param->pin_req.bda[4], param->pin_req.bda[5],
                 param->pin_req.min_16_digit);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Legacy PIN auth SUCCESS with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Legacy PIN auth FAILED, status=%d", param->auth_cmpl.stat);
        }
        break;
    default:
        break;
    }
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(TAG, "SPP initialized. Scanning ~10s for Bluetooth Classic devices...");
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
            ESP_LOGI(TAG, "SDP found SPP channel %d. Connecting (authenticated)...",
                     param->disc_comp.scn[0]);
            /* AUTHENTICATE alone did not trigger a real HCI authentication
             * request (no PIN prompt seen, L2CAP failed immediately) -
             * ENCRYPT requires authentication as a prerequisite, which forces
             * Bluedroid to actually run the legacy PIN pairing flow first. */
            esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT, ESP_SPP_ROLE_MASTER,
                            param->disc_comp.scn[0], s_target_bda);
        } else {
            ESP_LOGE(TAG, "SDP found no SPP service (status=%d). Cannot connect.",
                     param->disc_comp.status);
        }
        break;
    case ESP_SPP_OPEN_EVT:
        ESP_LOGI(TAG, "BT connected (handle=%lu). Running ELM327 init...",
                 (unsigned long)param->open.handle);
        s_spp_handle = param->open.handle;
        send_next_cmd();
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGW(TAG, "BT link closed.");
        break;
    case ESP_SPP_DATA_IND_EVT: {
        size_t len = param->data_ind.len;
        if (s_rx_len + len < sizeof(s_rx_buf)) {
            memcpy(s_rx_buf + s_rx_len, param->data_ind.data, len);
            s_rx_len += len;
            s_rx_buf[s_rx_len] = '\0';
        }
        if (memchr(param->data_ind.data, '>', len)) {
            /* Full reply received (ELM327 prompt seen). Sanitize before logging:
             * the ELM327 emits CR/LF and non-printable line-noise bytes (esp.
             * while it resets after ATZ). Printing those raw corrupts the log
             * and crashes esp-idf-monitor on a Windows cp1252 console. Mirrors
             * src/elm327.py:_send_raw cleanup (.replace(">","").strip()) and the
             * Arduino elmCmd (replace \r with space, drop >, trim). */
            char clean[sizeof(s_rx_buf)];
            size_t ci = 0;
            for (size_t i = 0; i < s_rx_len; i++) {
                unsigned char c = (unsigned char)s_rx_buf[i];
                if (c == '>') continue;                            /* drop prompt */
                clean[ci++] = (c >= 0x20 && c <= 0x7e) ? (char)c : ' ';
            }
            clean[ci] = '\0';
            char *start = clean;
            while (*start == ' ') start++;                         /* ltrim */
            char *end = start + strlen(start);
            while (end > start && end[-1] == ' ') *--end = '\0';    /* rtrim */
            ESP_LOGI(TAG, "<-- %s", start);
            /* Raw bytes as ground truth - pure ASCII hex, safe for any console,
             * confirms exactly what the adapter sent (reset-noise vs payload). */
            ESP_LOG_BUFFER_HEXDUMP(TAG, s_rx_buf, s_rx_len, ESP_LOG_INFO);
            send_next_cmd();
        }
        break;
    }
    default:
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(esp_bt_gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(esp_spp_cb));
    /* esp_spp_init() defaults to L2CAP ERTM, which cheap/older ELM327
     * Bluetooth modules (BT 2.0/2.1, Basic Mode only) reject - confirmed live
     * as an immediate "Incompatible CFG" L2CAP failure right after the ACL
     * link comes up. esp_spp_enhanced_init() lets us force Basic Mode. */
    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = false,
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&spp_cfg));

    esp_bt_gap_set_device_name("MaxusBridge");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    /* Fixed legacy PIN "1234" - matches the ELM327 default used for Windows
     * pairing. With SSP disabled (sdkconfig.defaults), this drives legacy
     * pairing instead of Secure Simple Pairing. */
    esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin_code);

    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, " POC2 (ESP-IDF): ESP32 BT -> ELM327");
    ESP_LOGI(TAG, "==============================");
    /* Discovery is kicked off from ESP_SPP_INIT_EVT above. */
}
