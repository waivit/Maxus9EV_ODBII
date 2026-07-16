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
    Parse a UDS Service 22 response over 11-bit CAN (ISO 15765-4, ATSP6).

    Frame layout (ATH1 headers on, ATS0 spaces off):
      [id 3 chars][PCI 2 chars][service 2 chars][did 4 chars][data...]
      e.g. 7ED 05 62 B0 41 06 5E   (id 7ED, single frame, 5 payload bytes)

    The PCI nibble selects the ISO-TP frame type:
      0N  single frame, N payload bytes
      1.. first frame of a multi-frame message (12-bit length)
      2N  consecutive frame

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
    # Need at least id(3) + PCI(2) + one service byte(2).
    if len(s) < 7:
        return result

    result["valid"] = True
    ident   = s[0:3]          # 11-bit CAN id (e.g. 7ED, 7EB)
    pci_hi  = s[3]            # ISO-TP frame-type nibble

    payload = ""
    if pci_hi == "0":
        # Single frame: low PCI nibble is the payload byte count.
        try:
            n = int(s[4], 16)
        except ValueError:
            return result
        payload = s[5: 5 + n * 2]
    elif pci_hi == "1":
        # First frame of a multi-frame message. Best-effort reassembly:
        # 12-bit length in the next 3 nibbles; 6 data bytes follow, then
        # consecutive (2x) frames each re-prefixed by the id when ATH1 is on.
        # Unverified until a real multi-frame PID is captured.
        try:
            total = int(s[4:7], 16)
        except ValueError:
            return result
        payload = s[7:19]
        for part in s[19:].split(ident):
            if part:
                payload += part[2:]   # drop the consecutive-frame PCI byte
        payload = payload[: total * 2]
    else:
        # Unexpected framing: skip id + PCI and try the remainder.
        payload = s[5:]

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
