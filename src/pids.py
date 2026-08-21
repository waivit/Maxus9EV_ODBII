"""
MAXUS 9 EV - Confirmed OBD2 PIDs
Discovered through reverse engineering sessions.
Platform: SAIC/MAXUS, Protocol: CAN 11/500 (ATSP6)

ECU mapping (names corrected 2026-08-21 from the ECUs' own ISO 14229
identification DIDs - see docs/findings/12-ecu-identification-and-bms-structure.md):
  Request 7E3 -> Response 7EB  (IMCU - Integrated Motor Control Unit; was "VCU",
                                a name inherited from the MG cross-reference)
  Request 7E4 -> Response 7EC  (CCU  - Central Control Unit / gateway)
  Request 7E5 -> Response 7ED  (BMS  - Battery Management System)

A native-CAN scan found 33 responding ECUs and 1824 readable DIDs in total
(docs/findings/11-native-can-ecu-scan.md). The three above are the ones reachable
and useful over the ELM327 path that these Python tools use.
"""

from dataclasses import dataclass
from typing import Callable, Optional


@dataclass
class PID:
    name:    str
    header:  str
    command: str
    formula: Callable[[list], float]
    unit:    str
    notes:   str = ""


# ======================================================
# Confirmed Working PIDs
# ======================================================
CONFIRMED = [
    PID(
        name    = "Bus Voltage",
        header  = "7E5",
        command = "22 B0 41",
        formula = lambda b: ((b[0]*256)+b[1]) * 0.25,
        unit    = "V",
        notes   = "HV battery bus voltage",
    ),
    PID(
        name    = "Pack Voltage",
        header  = "7E5",
        command = "22 B0 42",
        formula = lambda b: ((b[0]*256)+b[1]) * 0.25,
        unit    = "V",
        notes   = "HV battery pack voltage",
    ),
    PID(
        name    = "Pack Current",
        header  = "7E5",
        command = "22 B0 43",
        formula = lambda b: ((b[0]*256)+b[1]) * 0.025 - 1000,
        unit    = "A",
        notes   = "Positive = discharging, negative = charging",  # confirmed live, H5
    ),
    PID(
        name    = "Batt Temp High",
        header  = "7E5",
        command = "22 B0 56",
        formula = lambda b: (b[0] / 2.0) - 40,
        unit    = "C",
        notes   = "byte 1 is the sensor index, not part of the value (note 12)",
    ),
    PID(
        name    = "Batt Temp Low",
        header  = "7E5",
        command = "22 B0 57",
        formula = lambda b: (b[0] / 2.0) - 40,
        unit    = "C",
        notes   = "byte 1 is the sensor index, not part of the value (note 12)",
    ),
    PID(
        name    = "Cell Voltage Max",
        header  = "7E5",
        command = "22 B0 58",
        formula = lambda b: ((b[0]*256)+b[1]) * 0.001,
        unit    = "V",
        notes   = "byte 2 is the cell index, not part of the value",
    ),
    PID(
        name    = "Cell Voltage Min",
        header  = "7E5",
        command = "22 B0 59",
        formula = lambda b: ((b[0]*256)+b[1]) * 0.001,
        unit    = "V",
        notes   = "byte 2 is the cell index, not part of the value",
    ),
    PID(
        name    = "SOC Raw (B046)",
        header  = "7E5",
        command = "22 B0 46",
        formula = lambda b: ((b[0]*256)+b[1]) / 10.0,
        unit    = "%",
        # The PID and this formula are settled (3 independent references plus two
        # settled live points). What is NOT settled is the dash transform: the
        # instrument cluster shows roughly Raw + 4 %. Deliberately not applied
        # here - see H1 / docs/findings/10-poc4-live-crosscheck.md.
        notes   = "Raw SOC. Dash shows ~Raw+4% (transform uncalibrated, H1)",
    ),
    PID(
        name    = "12V Battery",
        header  = "7E3",
        command = "22 01 12",
        formula = lambda b: b[0] * 1.0,
        unit    = "V",
        notes   = "on IMCU (7E3), not BMS. MAXUS uses byte*1, NOT MG's byte/10",
    ),
    PID(
        name    = "Charge Status",
        header  = "7E5",
        command = "22 B0 48",
        formula = lambda b: float(b[0]),
        unit    = "",
        notes   = "enum: 00=ConnNotCharging 01=Idle 03=Running 06=AC 07=DC "
                  "08=Sleep 0a=Connected 0c=StartingCharge (03 confirmed live)",
    ),

    # --- IMCU (7E3): proven live by an idle -> driving delta, note 10 ---
    PID(
        name    = "Motor Temp",
        header  = "7E3",
        command = "22 B4 05",
        formula = lambda b: b[0] - 40.0,
        unit    = "C",
        notes   = "IMCU. Note the scale differs from BMS temps (no x0.5)",
    ),
    PID(
        name    = "Motor Speed",
        header  = "7E3",
        command = "22 B4 02",
        formula = lambda b: ((b[0]*256)+b[1]) - 0x7FFF,
        unit    = "rpm",
        notes   = "IMCU. 0 idle -> 1106 while driving (note 10)",
    ),
    PID(
        name    = "Motor Torque",
        header  = "7E3",
        command = "22 B4 01",
        formula = lambda b: ((b[0]*256)+b[1]) - 0x7FFF,
        unit    = "",
        notes   = "IMCU. 0 idle -> 330 while driving (note 10)",
    ),
    PID(
        name    = "Regen Level",
        header  = "7E3",
        command = "22 B4 80",
        formula = lambda b: float(b[0]),
        unit    = "",
        # H3, the project's primary goal. Found by diffing full DID scans taken at
        # each of the three selector positions: the only one of 309 DIDs that
        # stepped 00 -> 01 -> 02 in order (docs/findings/13-regen-level-found.md).
        notes   = "enum: 0=Low, 1=Medium, 2=High. On IMCU (7E3)",
    ),
    PID(
        name    = "Odometer",
        header  = "7E3",
        command = "22 E1 01",
        formula = lambda b: float((b[0]*65536)+(b[1]*256)+b[2]),
        unit    = "km",
        notes   = "IMCU. 3-byte value",
    ),
]


# ======================================================
# Unknown / Under Investigation
# ======================================================
UNDER_INVESTIGATION = [
    PID(
        name    = "SOH (candidate B061, MG)",
        header  = "7E5",
        command = "22 B0 61",
        formula = lambda b: ((b[0]*256)+b[1]) / 100.0,
        unit    = "%",
        notes   = "TESTING (H2) - reads 94.3% consistently across sessions, "
                  "but no dash/dealer SOH yet to confirm absolute accuracy",
    ),
    PID(
        name    = "Coolant Temp (candidate B05C, MG)",
        header  = "7E5",
        command = "22 B0 5C",
        formula = lambda b: b[0] * 0.5 - 40,
        unit    = "C",
        notes   = "TESTING (H6) - reads exactly the same as Batt Temp High, so it "
                  "still needs a warm-pack delta test to prove it is a separate sensor",
    ),

    # --- Per-cell module groups (BMS 7E5) -----------------------------------
    # The native-CAN scan found twelve 7-byte groups at stride 8 (note 12):
    #   B001 B009 B011 B019 B021 B029  and  B0A3 B0AB B0B3 B0BB B0C3 B0CB
    # Layout looks like {cellV_a, cellV_b, temp_a, temp_b, ...} with the MG
    # formulas V = INT16/2000 + 1.0 and T = byte*0.5 - 40 producing values that
    # match the pack state at capture time. Deliberately NOT added as PIDs here:
    # twelve extra reads would slow the ELM327 poll loop noticeably, and these
    # are better read over native CAN (firmware/poc5_s3_can). Recorded so the
    # addresses are not lost.

    # --- Superseded by the MG reference; kept for history, hidden from the
    #     dashboard to avoid duplicate/known-wrong SOC/SOH rows.
    # PID(
    #     name    = "SOC (candidate B04A)",
    #     header  = "7E5",
    #     command = "22 B0 4A",
    #     formula = lambda b: b[0] / 2.0,
    #     unit    = "%",
    #     notes   = "Returns 0 - formula may be wrong (B04A is main-relay status per MG ref)",
    # ),
    # PID(
    #     name    = "SOH",
    #     header  = "7E5",
    #     command = "22 B0 4C",
    #     formula = lambda b: ((b[0]*256)+b[1]) * 0.1 if len(b) > 1 else b[0] * 0.5,
    #     unit    = "%",
    #     notes   = "Returns 1.5% - formula likely wrong",
    # ),
]


# ======================================================
# Known NOT to work
# ======================================================
NOT_AVAILABLE = [
    # NRC 0x31 requestOutOfRange
    ("7E5", "22 B0 40", "SOC - NRC 0x31 requestOutOfRange"),
    ("7EC", "22 D0 08", "SOC via gateway - NRC 0x31"),

    # MG-referenced DIDs that simply do not exist on MAXUS: they returned no data
    # live while other DIDs on the same header answered in the same cycle, so this
    # is not an addressing problem (docs/findings/10-poc4-live-crosscheck.md).
    ("7E3", "22 B3 09", "VCU coolant (MG) - absent on MAXUS"),
    ("7E5", "22 00 5B", "SOC alt (MG) - absent on MAXUS"),
    ("7E5", "22 00 46", "Ambient temp (MG) - absent on MAXUS"),
    ("7E3", "22 B9 00", "Gear (MG) - absent on MAXUS"),
    ("7E3", "22 B1 8C", "Ignition state (MG) - absent on MAXUS"),
    ("7E3", "22 B7 03", "HV contactor (MG) - absent on MAXUS"),
    ("7E3", "22 BA 40", "Handbrake (MG) - absent on MAXUS"),
    ("7E3", "22 BA 00", "Vehicle speed (MG) - absent on MAXUS"),
    ("7E3", "22 B7 12", "Charge rate (MG) - absent on MAXUS"),
    ("7E5", "22 00 0D", "Vehicle speed (MG) - absent on MAXUS"),
]


# ======================================================
# Sentinel values - answer, but the payload means "no data"
# ======================================================
# These DIDs respond positively yet return a fixed unavailable marker, so any
# decoded number is meaningless and must be filtered rather than displayed
# (notes 10 and 12).
SENTINELS = {
    "B045": "3FFF",      # "resistance" - decodes to an absurd 8191.5 ohm
    "B0CE": "FFFE",      # "BMS range"
    "B074": "FFFFFFFE",
    "B0BD": "FFFE",
    "B0EE": "FFFFFFFE",
    "B0EF": "FFFFFFFE",
}
