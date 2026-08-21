#!/usr/bin/env python3
"""Analyse MODE_SCAN captures from firmware/poc5_s3_can.

Three subcommands:

  ids     Identify ECUs from the standard ISO 14229 identification DIDs
          (F183 part number, F192 hardware, F194 software, F18B build date).
          These are standardised, so this is the highest-confidence reading
          available from a single snapshot.

  decode  For one ECU, print each DID's raw payload next to the candidate
          decodings used elsewhere in this project (INT16*0.25, /10, /100,
          byte*0.5-40, byte-40). Everything it prints is a HYPOTHESIS - a
          plausible-looking number is not evidence. Values that are all 00 or
          all FF carry no information and are hidden unless --all is given.

  diff    Compare two captures and list every DID whose payload changed.
          This is the one that actually establishes meaning: capture the same
          scan under two vehicle states and whatever moved is a candidate for
          the thing that differed. It is how the regen-level DID is meant to be
          found (scan at regen Low, then High, then diff).

Usage:
    python tools/analyze_scan.py ids  scan_run1.log scan_run2.log
    python tools/analyze_scan.py decode scan_run2.log --ecu 7E5
    python tools/analyze_scan.py diff regen_low.log regen_high.log
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# "[7E5] B042 +  2 : 06 19  | .."
HIT = re.compile(r"\[([0-9A-Fa-f]{3})\]\s+([0-9A-Fa-f]{4})\s+\+\s+(\d+)\s+:\s+(.*?)\s*\|")
# "[ECU] req=7E5 resp=7ED BMS      (positive)"
ECU = re.compile(r"\[ECU\]\s+req=([0-9A-Fa-f]{3})\s+resp=([0-9A-Fa-f]{3})")

# Identification DIDs defined by ISO 14229 - meaning is standard, not guessed.
STD_IDS = {
    "F183": "part number",
    "F187": "spare part number",
    "F18B": "ECU build date",
    "F18C": "ECU serial",
    "F190": "VIN",
    "F191": "hardware number",
    "F192": "supplier hardware",
    "F194": "supplier software",
}


def load(path: Path) -> tuple[dict[str, dict[str, list[str]]], list[tuple[str, str]]]:
    """Return {ecu: {did: [hex bytes]}} and the request->response map."""
    data: dict[str, dict[str, list[str]]] = {}
    ecus: list[tuple[str, str]] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.replace("\x00", "").splitlines():
        m = ECU.search(line)
        if m:
            ecus.append((m.group(1).upper(), m.group(2).upper()))
            continue
        m = HIT.search(line)
        if m:
            data.setdefault(m.group(1).upper(), {})[m.group(2).upper()] = m.group(4).split()
    return data, ecus


def ascii_of(raw: list[str]) -> str:
    return "".join(chr(int(b, 16)) if 0x20 <= int(b, 16) < 0x7F else "." for b in raw)


def is_blank(raw: list[str]) -> bool:
    return all(b == "00" for b in raw) or all(b == "FF" for b in raw)


def build_date(raw: list[str]) -> str:
    """F18B is BCD yy mm dd. Modules that do not populate it return junk."""
    if len(raw) < 3:
        return "-"
    y, mo, d = raw[:3]
    if not all(c in "0123456789" for c in y + mo + d):
        return f"(invalid {y} {mo} {d})"
    if not ("01" <= mo <= "12" and "01" <= d <= "31"):
        return f"(invalid 20{y}-{mo}-{d})"
    return f"20{y}-{mo}-{d}"


def cmd_ids(args: argparse.Namespace) -> int:
    merged: dict[str, dict[str, list[str]]] = {}
    for p in args.logs:
        data, _ = load(p)
        for ecu, dids in data.items():
            merged.setdefault(ecu, {}).update(dids)
    if not merged:
        print("no scan data found", file=sys.stderr)
        return 1

    hdr = f"{'ECU':<5} {'F183 part':<22} {'F192 hardware':<14} {'F194 software':<24} build date"
    print(hdr)
    print("-" * len(hdr))
    for ecu in sorted(merged):
        d = merged[ecu]
        def s(k: str) -> str:
            return ascii_of(d[k]).strip() if k in d else "-"
        print(f"{ecu:<5} {s('F183'):<22} {s('F192'):<14} {s('F194'):<24} "
              f"{build_date(d.get('F18B', []))}")
    print(f"\n{len(merged)} ECU(s). Strings are read from the ISO 14229 identification "
          f"DIDs; interpreting them as a module name is still inference.")
    return 0


def cmd_decode(args: argparse.Namespace) -> int:
    data, _ = load(args.log)
    ecu = args.ecu.upper()
    if ecu not in data:
        print(f"ECU {ecu} not in {args.log} (have: {', '.join(sorted(data))})", file=sys.stderr)
        return 1

    dids = data[ecu]
    shown = hidden = 0
    print(f"ECU {ecu} - {len(dids)} DIDs\n")
    print(f"{'DID':<6}{'len':>4}  {'raw':<26}{'INT16*.25':>10}{'/10':>9}{'/100':>9}"
          f"{'b0*.5-40':>10}{'b0-40':>7}  ascii")
    print("-" * 96)
    for did in sorted(dids):
        raw = dids[did]
        if is_blank(raw) and not args.all:
            hidden += 1
            continue
        shown += 1
        hexs = " ".join(raw[:8]) + (" ..." if len(raw) > 8 else "")
        cols = [""] * 5
        if len(raw) >= 2:
            v16 = int(raw[0], 16) * 256 + int(raw[1], 16)
            cols[0] = f"{v16 * 0.25:10.2f}"
            cols[1] = f"{v16 / 10:9.1f}"
            cols[2] = f"{v16 / 100:9.2f}"
        else:
            cols[0] = cols[1] = cols[2] = " " * 9
        b0 = int(raw[0], 16)
        cols[3] = f"{b0 * 0.5 - 40:10.1f}"
        cols[4] = f"{b0 - 40:7d}"
        note = f"  <- {STD_IDS[did]}" if did in STD_IDS else ""
        print(f"{did:<6}{len(raw):>4}  {hexs:<26}{''.join(cols)}  {ascii_of(raw[:12])}{note}")

    print(f"\n{shown} shown, {hidden} hidden (all-00 / all-FF; use --all to include).")
    print("Every decoded column is a CANDIDATE, not a finding. Confirm by diffing "
          "two captures taken in different vehicle states.")
    return 0


def cmd_diff(args: argparse.Namespace) -> int:
    a, _ = load(args.before)
    b, _ = load(args.after)

    changed: list[tuple[str, str, str, str]] = []
    only_a: list[tuple[str, str]] = []
    only_b: list[tuple[str, str]] = []

    for ecu in sorted(set(a) | set(b)):
        da, db = a.get(ecu, {}), b.get(ecu, {})
        for did in sorted(set(da) | set(db)):
            if did not in db:
                only_a.append((ecu, did))
            elif did not in da:
                only_b.append((ecu, did))
            elif da[did] != db[did]:
                changed.append((ecu, did, " ".join(da[did]), " ".join(db[did])))

    print(f"before: {args.before.name}")
    print(f"after : {args.after.name}\n")

    if not changed:
        print("No DID changed value between the two captures.")
    else:
        print(f"{len(changed)} DID(s) changed:\n")
        print(f"{'ECU':<5}{'DID':<7}{'before':<26}{'after':<26}")
        print("-" * 64)
        for ecu, did, va, vb in changed:
            print(f"{ecu:<5}{did:<7}{va[:24]:<26}{vb[:24]:<26}")

    if only_a or only_b:
        print(f"\nonly in before: {len(only_a)}   only in after: {len(only_b)}"
              "  (a DID can drop out if the ECU answered NRC 0x78 that run)")

    print("\nA changed DID is a CANDIDATE for whatever differed between the two "
          "states. Confirm it by repeating with a third state - a real setting "
          "should track it monotonically (e.g. regen Low / Medium / High).")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ids", help="identify ECUs from standard identification DIDs")
    p.add_argument("logs", nargs="+", type=Path)
    p.set_defaults(func=cmd_ids)

    p = sub.add_parser("decode", help="show candidate decodings for one ECU")
    p.add_argument("log", type=Path)
    p.add_argument("--ecu", required=True, help="ECU request address, e.g. 7E5")
    p.add_argument("--all", action="store_true", help="include all-00 / all-FF DIDs")
    p.set_defaults(func=cmd_decode)

    p = sub.add_parser("diff", help="list DIDs that changed between two captures")
    p.add_argument("before", type=Path)
    p.add_argument("after", type=Path)
    p.set_defaults(func=cmd_diff)

    args = ap.parse_args(argv)
    for attr in ("log", "before", "after"):
        f = getattr(args, attr, None)
        if f is not None and not f.is_file():
            print(f"error: no such file: {f}", file=sys.stderr)
            return 1
    for f in getattr(args, "logs", []):
        if not f.is_file():
            print(f"error: no such file: {f}", file=sys.stderr)
            return 1
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
