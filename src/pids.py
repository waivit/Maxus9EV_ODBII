"""
MAXUS 9 EV - Confirmed OBD2 PIDs
Discovered through reverse engineering sessions.
Platform: SAIC/MAXUS, Protocol: CAN 11/500 (ATSP6)

ECU mapping:
  Request 7E3 -> Response 7EB  (VCU)
  Request 7E4 -> Response 7EC  (Gateway)
  Request 7E5 -> Response 7ED  (BMS)
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
    ),
    PID(
        name    = "Batt Temp Low",
        header  = "7E5",
        command = "22 B0 57",
        formula = lambda b: (b[0] / 2.0) - 40,
        unit    = "C",
    ),
    PID(
        name    = "Cell Voltage Max",
        header  = "7E5",
        command = "22 B0 58",
        formula = lambda b: ((b[0]*256)+b[1]) * 0.001,
        unit    = "V",
    ),
    PID(
        name    = "Cell Voltage Min",
        header  = "7E5",
        command = "22 B0 59",
        formula = lambda b: ((b[0]*256)+b[1]) * 0.001,
        unit    = "V",
    ),
    PID(
        name    = "12V Battery",
        header  = "7E3",
        command = "22 01 12",
        formula = lambda b: b[0] * 1.0,
        unit    = "V",
    ),
    PID(
        name    = "Charge Status",
        header  = "7E5",
        command = "22 B0 48",
        formula = lambda b: float(b[0]),
        unit    = "",
        notes   = "Values TBD: 3=driving?",
    ),
]


# ======================================================
# Unknown / Under Investigation
# ======================================================
UNDER_INVESTIGATION = [
    PID(
        name    = "SOC (candidate B04A)",
        header  = "7E5",
        command = "22 B0 4A",
        formula = lambda b: b[0] / 2.0,
        unit    = "%",
        notes   = "Returns 0 - formula may be wrong",
    ),
    PID(
        name    = "SOH",
        header  = "7E5",
        command = "22 B0 4C",
        formula = lambda b: ((b[0]*256)+b[1]) * 0.1 if len(b) > 1 else b[0] * 0.5,
        unit    = "%",
        notes   = "Returns 1.5% - formula likely wrong",
    ),
]


# ======================================================
# Known NOT to work (NRC 0x31)
# ======================================================
NOT_AVAILABLE = [
    ("7E5", "22 B0 40", "SOC - NRC 0x31 requestOutOfRange"),
    ("7EC", "22 D0 08", "SOC via gateway - NRC 0x31"),
]
