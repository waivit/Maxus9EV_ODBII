"""
UDS (ISO 14229) Response Parser
Handles Service 22 (Read by Identifier) responses and NRC decoding.
"""

NRC_TABLE = {
    0x10: "generalReject",
    0x11: "serviceNotSupported",
    0x12: "subFunctionNotSupported",
    0x22: "conditionsNotCorrect",
    0x24: "requestSequenceError",
    0x31: "requestOutOfRange",
    0x33: "securityAccessDenied",
    0x35: "invalidKey",
    0x78: "responsePending",
    0x7E: "subFunctionNotSupportedInActiveSession",
    0x7F: "serviceNotSupportedInActiveSession",
}


def parse(raw: str) -> dict:
    """
    Parse a UDS response string.

    Single frame layout:
      [header 6 chars][length 2 chars][service 2 chars][did 4 chars][data...]
      e.g. 7ED 04 62 B0 42 06 A5

    Returns dict with keys:
      valid, positive, nrc_code, nrc_name, did, data (list of ints), data_hex
    """
    result = {
        "valid":    False,
        "positive": False,
        "nrc_code": None,
        "nrc_name": "",
        "did":      "",
        "data":     [],
        "data_hex": "",
    }

    s = raw.strip().upper().replace(" ", "").replace("\r", "").replace("\n", "")
    if len(s) < 10:
        return result

    result["valid"] = True
    header = s[0:6]

    # Reassemble multi-frame payload
    payload = ""
    i = 0
    while i < len(s):
        if s[i:i+6] != header or i + 7 > len(s):
            i += 2
            continue
        nibble = s[i+6]
        if nibble == "0":
            try:
                length = int(s[i+6:i+8], 16)
            except ValueError:
                i += 2
                continue
            payload += s[i+8: i+8 + length * 2]
            i += 8 + length * 2
        elif nibble == "1":
            payload += s[i+9: i+9+12]
            i += 9 + 12
        elif nibble == "2":
            payload += s[i+8: i+8+14]
            i += 8 + 14
        else:
            i += 2

    if not payload:
        payload = s[8:]

    # Strip AA padding
    while payload.endswith("AA"):
        payload = payload[:-2]

    if len(payload) < 2:
        return result

    svc = payload[0:2]

    if svc == "7F":
        try:
            nrc = int(payload[4:6], 16)
            result["nrc_code"] = nrc
            result["nrc_name"] = NRC_TABLE.get(nrc, f"0x{nrc:02X}")
        except (ValueError, IndexError):
            pass
        return result

    if svc == "62" and len(payload) >= 6:
        result["positive"]  = True
        result["did"]       = payload[2:6]
        data_hex            = payload[6:]
        result["data_hex"]  = data_hex
        result["data"]      = []
        for j in range(0, len(data_hex) - 1, 2):
            try:
                result["data"].append(int(data_hex[j:j+2], 16))
            except ValueError:
                break

    return result


def is_positive(raw: str) -> bool:
    r = parse(raw)
    return r["positive"]


def extract_data(raw: str) -> list:
    return parse(raw)["data"]
