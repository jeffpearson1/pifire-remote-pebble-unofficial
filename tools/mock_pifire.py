"""Minimal fake PiFire for testing the watch app in the emulator.

Serves /api/current and accepts the /api/set/... commands the app sends.
Usage: python3 tools/mock_pifire.py [port]   (default 8080)
"""
import json
import random
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

state = {
    "mode": "Smoke",
    "psp": 0,
    "s_plus": False,
    "grill": 182.0,
    "probes": {"Probe1": 141.0, "Probe2": 0},
}


def current():
    target = state["psp"] if state["mode"] == "Hold" else 180
    if state["mode"] in ("Stop", "Shutdown"):
        target = 70
    state["grill"] += (target - state["grill"]) * 0.2 + random.uniform(-2, 2)
    state["probes"]["Probe1"] += 0.4
    # Like a real PiFire, probes read 0 while stopped.
    stopped = state["mode"] == "Stop"
    return {
        "current": {
            "P": {"Grill": 0 if stopped else round(state["grill"], 1)},
            "F": state["probes"],
            "NT": {},
            "PSP": state["psp"],
            "TS": 0,
        },
        "notify_data": [
            {"label": "Probe1", "name": "Brisket", "req": True, "target": 203, "type": "probe"},
            {"label": "Probe2", "name": "Probe2", "req": False, "target": 0, "type": "probe"},
        ],
        "status": {
            "mode": state["mode"],
            "s_plus": state["s_plus"],
            "units": "F",
            "name": "Mock PiFire",
            "lid_open_detected": False,
            "critical_error": False,
        },
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        parts = [p for p in self.path.split("/") if p]
        if parts == ["api", "current"]:
            return self.reply(current())
        if parts[:3] == ["api", "set", "mode"] and len(parts) >= 4:
            mode = parts[3].capitalize()
            if mode == "Hold" and len(parts) >= 5:
                state["psp"] = float(parts[4])
            state["mode"] = mode
            return self.reply({"result": "OK", "message": "Mode set", "data": {}})
        if parts[:3] == ["api", "set", "splus"] and len(parts) == 4:
            state["s_plus"] = parts[3] == "true"
            return self.reply({"result": "OK", "message": "Smoke plus set", "data": {}})
        self.reply({"result": "ERROR", "message": "Unknown path"}, 404)

    def reply(self, body, code=200):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"Mock PiFire on http://localhost:{port}")
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()
