"""
MAXUS 9 EV - Web Dashboard
Auto-refreshing browser view of battery data: raw hex + ASCII + description +
decoded value + OK/WARN/ALARM status, for CONFIRMED and UNDER_INVESTIGATION PIDs.

Design: a single background thread owns the ELM327 (serial is not thread-safe),
polls all PIDs into a shared snapshot; the HTTP handlers only read that snapshot.
Read-only - never writes to the vehicle. stdlib only (no extra dependencies).

Usage:
  cd src && python webdash.py
Then open the printed URL (also reachable from a phone on the same network).
"""

import os
import sys
import json
import time
import socket
import threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

from elm327 import ELM327, select_com_port
from uds import parse
from pids import CONFIRMED, UNDER_INVESTIGATION
# Reuse the alert logic from the terminal diag tool (DRY).
from diag import THRESHOLDS, evaluate, battery_state


PORT = 8000
DEFAULT_INTERVAL = 2.0
INTERVAL_CHOICES = [1, 2, 5, 10]

# Shared state between the poller thread and the HTTP handlers.
_lock = threading.Lock()
_snapshot = {
    "rows": [],
    "state": "unknown",
    "overall": "--",
    "updated": "",
    "connected": False,
    "interval": DEFAULT_INTERVAL,
}
_ctrl = {"interval": DEFAULT_INTERVAL, "running": True}

_STATUS_ORDER = {"OK": 0, "WARN": 1, "ALARM": 2, "--": 0}


def _ascii(data: list) -> str:
    """Printable ASCII of raw bytes; non-printable -> middle dot."""
    return "".join(chr(b) if 32 <= b <= 126 else "·" for b in data)


def _hex(data: list) -> str:
    return " ".join(f"{b:02X}" for b in data)


def _fmt_value(name: str, value, unit: str) -> str:
    if value is None:
        return "no data"
    if name == "Cell Imbalance":
        return f"{value:.0f} {unit}"
    if name == "Charge Status":
        return f"{value:.0f}"
    return f"{value:.2f} {unit}".strip()


def read_row(elm: ELM327, pid) -> dict:
    """Query one PID and return a display row (value + raw hex + ascii + status)."""
    elm.set_header(pid.header)
    resp = elm.send(pid.command, 0.5)
    raw = resp.upper().replace(" ", "").replace("\r", "").replace("\n", "")
    p = parse(raw)

    value = None
    data = p["data"] if p["positive"] else []
    if p["positive"] and data:
        try:
            value = pid.formula(data)
        except Exception:
            value = None

    status = evaluate(pid.name, value) if pid.name in THRESHOLDS else "OK"
    return {
        "name": pid.name,
        "description": pid.notes or "",
        "header": pid.header,
        "command": pid.command,
        "raw_hex": _hex(data) if data else "-",
        "ascii": _ascii(data) if data else "-",
        "value": _fmt_value(pid.name, value, pid.unit),
        "status": status if value is not None else "--",
        "_num": value,  # kept for derived computations
    }


def _computed_row(name: str, description: str, value, unit: str) -> dict:
    status = evaluate(name, value) if name in THRESHOLDS else "OK"
    return {
        "name": name,
        "description": description,
        "header": "",
        "command": "computed",
        "raw_hex": "(computed)",
        "ascii": "-",
        "value": _fmt_value(name, value, unit),
        "status": status if value is not None else "--",
        "_num": value,
    }


def build_snapshot(elm: ELM327) -> dict:
    """Read all PIDs + derived metrics into a fresh snapshot dict."""
    rows = [read_row(elm, pid) for pid in (list(CONFIRMED) + list(UNDER_INVESTIGATION))]
    by_name = {r["name"]: r["_num"] for r in rows}

    cmax, cmin = by_name.get("Cell Voltage Max"), by_name.get("Cell Voltage Min")
    imbalance = (cmax - cmin) * 1000.0 if cmax is not None and cmin is not None else None

    thi, tlo = by_name.get("Batt Temp High"), by_name.get("Batt Temp Low")
    spread = thi - tlo if thi is not None and tlo is not None else None

    rows.append(_computed_row("Cell Imbalance", "Cell V max - min", imbalance, "mV"))
    rows.append(_computed_row("Temp Spread", "Batt temp high - low", spread, "C"))

    worst = "OK"
    for r in rows:
        if _STATUS_ORDER[r["status"]] > _STATUS_ORDER[worst]:
            worst = r["status"]

    for r in rows:
        r.pop("_num", None)

    return {
        "rows": rows,
        "state": battery_state(by_name.get("Pack Current")),
        "overall": worst,
        "updated": time.strftime("%H:%M:%S"),
        "connected": any(r["status"] != "--" for r in rows),
    }


def poller_loop(elm: ELM327):
    while _ctrl["running"]:
        try:
            snap = build_snapshot(elm)
        except Exception as e:
            snap = None
            err = str(e)
        with _lock:
            if snap is not None:
                snap["interval"] = _ctrl["interval"]
                _snapshot.update(snap)
            else:
                _snapshot["connected"] = False
                _snapshot["updated"] = time.strftime("%H:%M:%S") + f" (error: {err})"
        time.sleep(_ctrl["interval"])


# ======================================================
# HTTP
# ======================================================
PAGE = """<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MAXUS 9 EV - Battery Dashboard</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: system-ui, sans-serif; margin: 0; background:#111; color:#eee; }
  header { padding: 12px 16px; background:#1b1b1b; border-bottom:1px solid #333;
           display:flex; flex-wrap:wrap; gap:16px; align-items:center; }
  .pill { padding:4px 10px; border-radius:6px; font-weight:600; }
  .OK{background:#14532d;color:#4ade80} .WARN{background:#78350f;color:#fbbf24}
  .ALARM{background:#7f1d1d;color:#f87171} .muted{color:#888}
  table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
  th,td{padding:8px 12px;text-align:left;border-bottom:1px solid #262626}
  th{color:#9ca3af;font-size:12px;text-transform:uppercase;letter-spacing:.5px}
  td.mono,code{font-family:ui-monospace,Menlo,Consolas,monospace}
  tr.ALARM{background:#2a1414} tr.WARN{background:#2a2011}
  .dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px}
  select{background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:4px}
</style></head><body>
<header>
  <strong>MAXUS 9 EV</strong>
  <span>State: <span id="state" class="pill">-</span></span>
  <span>Overall: <span id="overall" class="pill">-</span></span>
  <span class="muted">Updated <span id="updated">-</span></span>
  <span id="conn" class="muted">connecting...</span>
  <span style="margin-left:auto">Refresh
    <select id="iv">
      <option value="1">1s</option><option value="2" selected>2s</option>
      <option value="5">5s</option><option value="10">10s</option>
    </select>
  </span>
</header>
<table>
  <thead><tr><th>Parameter</th><th>Description</th><th>Raw Hex</th>
  <th>ASCII</th><th>Value</th><th>Status</th></tr></thead>
  <tbody id="rows"></tbody>
</table>
<script>
let iv = 2000;
function pill(el, s){ el.className = "pill " + s; el.textContent = s; }
async function tick(){
  try{
    const r = await fetch("/data?interval=" + (iv/1000));
    const d = await r.json();
    // State is informational (charge/discharge/idle is normal), not an alert.
    pill(document.getElementById("state"), "muted");
    document.getElementById("state").textContent = d.state;
    pill(document.getElementById("overall"), d.overall);
    document.getElementById("updated").textContent = d.updated;
    document.getElementById("conn").textContent = d.connected ? "connected" : "no data";
    const tb = document.getElementById("rows");
    tb.innerHTML = d.rows.map(x =>
      `<tr class="${x.status}">
        <td>${x.name}</td><td class="muted">${x.description}</td>
        <td class="mono">${x.raw_hex}</td><td class="mono">${x.ascii}</td>
        <td>${x.value}</td>
        <td><span class="dot ${x.status}" style="background:currentColor"></span>
            <span class="${x.status}" style="background:none">${x.status}</span></td>
      </tr>`).join("");
  }catch(e){ document.getElementById("conn").textContent = "server offline"; }
}
let timer = setInterval(tick, iv);
document.getElementById("iv").addEventListener("change", e => {
  iv = parseInt(e.target.value,10)*1000;
  clearInterval(timer); timer = setInterval(tick, iv); tick();
});
tick();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # silence per-request logging
        pass

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path.startswith("/data"):
            q = parse_qs(parsed.query)
            if "interval" in q:
                try:
                    iv = float(q["interval"][0])
                    if iv in INTERVAL_CHOICES:
                        _ctrl["interval"] = iv
                except ValueError:
                    pass
            with _lock:
                body = json.dumps(_snapshot).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            body = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)


def lan_ip() -> str:
    """Best-effort local IP (no packets actually sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    print("=" * 46)
    print(" MAXUS 9 EV - Web Dashboard")
    print("=" * 46)

    port = select_com_port()
    elm = ELM327(port)
    if not elm.connect():
        sys.exit(1)
    elm.init()

    t = threading.Thread(target=poller_loop, args=(elm,), daemon=True)
    t.start()

    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    ip = lan_ip()
    print(f"Dashboard on:")
    print(f"  this PC : http://127.0.0.1:{PORT}")
    print(f"  phone   : http://{ip}:{PORT}   (same WiFi/hotspot)")
    print("Ctrl+C to stop.\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        _ctrl["running"] = False
        server.shutdown()
        elm.disconnect()


if __name__ == "__main__":
    main()
