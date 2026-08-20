#!/usr/bin/env python3
"""Split a poc5 scan capture into one log file per ECU.

The firmware (firmware/poc5_s3_can, MODE_SCAN) tags every result line with the
request address it came from, e.g.

    I (12345) poc5_can: [7E5] B042 +  2 : 06 19  | ..
    I (12350) poc5_can: [ECU] req=7E5 resp=7ED BMS      (positive)

so the per-ECU split can be done here rather than adding a filesystem to the
board. Unknown addresses get a file named after the address itself.

Usage:
    python tools/split_scan_log.py scan_raw.log [-o outdir]
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

# Friendly file names for the ECUs identified on the ELM327 path
# (docs/findings/01-confirmed-pids.md). Anything else is named by address.
ECU_FILES = {
    "7E3": "vcu_motor",
    "7E4": "gateway",
    "7E5": "bms",
}

# "[7E5] B042 + ..." or "[ECU] req=7E5 ..."
TAG_RE = re.compile(r"\[([0-9A-Fa-f]{3})\]")
ECU_RE = re.compile(r"\[ECU\]\s+req=([0-9A-Fa-f]{3})\s+resp=([0-9A-Fa-f]{3})")
# Strip the ESP-IDF log prefix: "I (12345) poc5_can: "
PREFIX_RE = re.compile(r"^[IWE]\s+\(\d+\)\s+\w+:\s?")
# Supported-DID result line: "[7E5] B042 +  2 : 06 19  | .."
HIT_RE = re.compile(
    r"\[([0-9A-Fa-f]{3})\]\s+([0-9A-Fa-f]{4})\s+\+\s+(\d+)\s+:\s+(.*?)\s*\|\s*(.*)$"
)


def ecu_filename(addr: str) -> str:
    return ECU_FILES.get(addr.upper(), f"ecu_{addr.upper()}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile", type=Path, help="raw scan capture from idf.py monitor")
    ap.add_argument("-o", "--outdir", type=Path, default=None,
                    help="output directory (default: alongside the input file)")
    args = ap.parse_args(argv)

    if not args.logfile.is_file():
        print(f"error: no such file: {args.logfile}", file=sys.stderr)
        return 1

    outdir = args.outdir or args.logfile.parent
    outdir.mkdir(parents=True, exist_ok=True)

    # The capture can contain stray non-UTF8 bytes from the serial line.
    text = args.logfile.read_text(encoding="utf-8", errors="replace")

    per_ecu: dict[str, list[str]] = defaultdict(list)
    summary: list[str] = []
    ecu_map: list[tuple[str, str]] = []
    hits: dict[str, int] = defaultdict(int)

    for raw in text.splitlines():
        line = raw.replace("\x00", "").rstrip()
        if not line:
            continue

        m = ECU_RE.search(line)
        if m:
            ecu_map.append((m.group(1).upper(), m.group(2).upper()))
            summary.append(PREFIX_RE.sub("", line))
            continue

        tag = TAG_RE.search(line)
        if not tag:
            # Phase banners, summaries and warnings - keep them in the overview.
            if any(k in line for k in ("PHASE", "SCAN", "SUMMARY", "ECU ", "swept")):
                summary.append(PREFIX_RE.sub("", line))
            continue

        addr = tag.group(1).upper()
        per_ecu[addr].append(PREFIX_RE.sub("", line))
        if HIT_RE.search(line):
            hits[addr] += 1

    if not per_ecu:
        print("No tagged scan lines found - is this a MODE_SCAN capture?",
              file=sys.stderr)
        return 1

    written = []
    for addr, lines in sorted(per_ecu.items()):
        path = outdir / f"{ecu_filename(addr)}.log"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        written.append((path, addr, len(lines), hits.get(addr, 0)))

    overview = outdir / "scan_summary.log"
    body = ["ECU request -> response map:"]
    body += [f"  {req} -> {resp}  ({ecu_filename(req)})" for req, resp in ecu_map]
    body += ["", "Scan output:"] + summary
    overview.write_text("\n".join(body) + "\n", encoding="utf-8")

    print(f"{'file':<28} {'ecu':<6} {'lines':>7} {'supported DIDs':>15}")
    print("-" * 60)
    for path, addr, n, h in written:
        print(f"{path.name:<28} {addr:<6} {n:>7} {h:>15}")
    print(f"\noverview -> {overview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
