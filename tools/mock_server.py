#!/usr/bin/env python3
"""BLLED v3 API mock server — develop and screenshot the web UI without hardware.

    python3 tools/mock_server.py            # serves src/www on http://localhost:8080
    python3 tools/mock_server.py --port 9000 --root src/www --cycle 120

It implements every endpoint of ARCHITECTURE.md section 7 (REST + a minimal RFC6455
WebSocket at /ws) against a simulated X1C that loops

    idle -> preparing -> preheating -> lidar stages -> printing -> finish -> idle

once per --cycle seconds (default 120), with temperatures, fans, layers and progress
advancing realistically, an HMS message appearing on roughly every third cycle (--hms
makes that every cycle, and --offset starts partway through it; both are handy for
screenshots), and a
door that opens when a print finishes.  Configuration lives in memory: PUT /api/config
validates key names and enum values exactly like the firmware is supposed to, so a UI
bug that sends a bad key shows up here as a 400 rather than silently working.

Realistic field values are taken from tools/fixtures_x1c_pushall.json.
Python 3.8+, standard library only.  Ctrl-C to stop.
"""

import argparse
import base64
import hashlib
import json
import mimetypes
import os
import random
import re
import select
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FW = "3.0.0"
BUILD = time.strftime("%Y-%m-%dT%H:%M:%S")
MAC = "A0:B7:65:1C:2D:3E"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "www")

# --------------------------------------------------------------------------- config

HEX = re.compile(r"^#[0-9a-fA-F]{6}$")

COLOR_NAMES = ["running", "maintenance", "test", "wifi", "preheat", "finish",
               "stage14", "stage1", "stage8", "stage9", "stage10",
               "pause", "firstLayer", "nozzleClog", "hmsSerious", "hmsFatal",
               "hmsCommon", "filamentRunout", "frontCover", "nozzleTemp", "bedTemp"]

ENUMS = {
    "ledMode": ["auto", "maintenance", "test", "rainbow", "wifi", "off"],
    "finishEffect": ["solid", "breathe", "blink", "fastblink"],
    "errorEffect": ["solid", "breathe", "blink", "fastblink"],
    "pauseEffect": ["solid", "breathe", "blink", "fastblink"],
    "printingVisual": ["solid", "progress", "breathe"],
    "preheatVisual": ["solid", "tempglow"],
    "finishExitMode": ["door", "timer"],
}

# min/max for numeric keys (mirrors validateConfig() in filesystem.h)
RANGES = {
    "brightness": (0, 100), "fadeMs": (0, 5000), "effectSpeed": (1, 10),
    "finishTimerMins": (0, 999), "inactivityMins": (0, 999), "offlineTimeoutSec": (0, 999),
    "mqttExtPort": (1, 65535), "mqttExtIntervalSec": (1, 999),
}

SECRETS = ("wifiPass", "webPass", "mqttExtPass")
NETKEYS = ("wifiSSID", "wifiPass", "BSSID", "host", "printerIP", "serialNumber",
           "accessCode", "webUser", "webPass")


def defaults():
    c = {
        # printer connection
        "printerIP": "10.0.42.159", "accessCode": "3f9k2ba7", "serialNumber": "00M09D501400123",
        "printerAutoIp": True, "isP1Printer": False,
        # wifi / host / auth
        "wifiSSID": "Fritz!Box 7590 2.4", "wifiPass": "correcthorsebattery",
        "BSSID": "", "rescanWiFiNetwork": False, "host": "BLLED",
        "webUser": "", "webPass": "",
        # led general
        "brightness": 80, "ledMode": "auto", "fadeMs": 500, "effectSpeed": 5,
        "followChamberLight": True, "printingVisual": "progress", "preheatVisual": "tempglow",
        # print events
        "finishIndication": True, "finishEffect": "breathe", "finishExitMode": "door",
        "finishTimerMins": 10, "inactivityEnabled": True, "inactivityMins": 60,
        "controlChamberLight": False, "doorToggleEnabled": True, "offlineTimeoutSec": 30,
        "lidarStagesEnabled": True,
        # errors
        "errorDetection": True, "errorEffect": "blink", "pauseEffect": "breathe",
        "hmsCommonEnabled": False, "hmsIgnoreList": "",
        # external mqtt
        "mqttExtEnabled": True, "mqttExtHost": "10.0.42.10", "mqttExtPort": 1883,
        "mqttExtUser": "blled", "mqttExtPass": "brokerpass", "mqttExtBaseTopic": "",
        "mqttExtIntervalSec": 10, "haDiscovery": True, "haPrefix": "homeassistant",
        # debug
        "debugVerbose": False, "debugChanges": True, "debugMqtt": False,
    }
    col = {
        "running": ("#000000", 255, 255), "maintenance": ("#000000", 255, 255),
        "test": ("#3f3cfb", 0, 0), "wifi": ("#ffa500", 0, 0), "preheat": ("#ff6a00", 0, 0), "finish": ("#00ff00", 0, 0),
        "stage14": ("#000000", 0, 0), "stage1": ("#000000", 0, 0), "stage8": ("#000000", 0, 0),
        "stage9": ("#000000", 0, 0), "stage10": ("#000000", 0, 0),
        "pause": ("#0000ff", 0, 0), "firstLayer": ("#0000ff", 0, 0), "nozzleClog": ("#0000ff", 0, 0),
        "hmsSerious": ("#ff0000", 0, 0), "hmsFatal": ("#ff0000", 0, 0), "hmsCommon": ("#ffa500", 0, 0),
        "filamentRunout": ("#ff0000", 0, 0), "frontCover": ("#ff0000", 0, 0),
        "nozzleTemp": ("#ff0000", 0, 0), "bedTemp": ("#ff0000", 0, 0),
    }
    for n, (rgb, ww, cw) in col.items():
        c[n + "RGB"] = rgb
        c[n + "WW"] = ww
        c[n + "CW"] = cw
    return c


CFG = defaults()
LOCK = threading.RLock()


def known_keys():
    return set(defaults().keys())


def public_config():
    """What GET /api/config returns: secrets masked, accessCode in full."""
    with LOCK:
        out = dict(CFG)
    for k in SECRETS:
        out[k] = "********" if out.get(k) else ""
    return out


def apply_config(patch):
    """Returns (ok, error_string, restart_required). Mirrors the firmware's rules."""
    if not isinstance(patch, dict):
        return False, "body must be a JSON object", False
    unknown = [k for k in patch if k not in known_keys()]
    if unknown:
        return False, "unknown keys: " + ", ".join(sorted(unknown)), False
    restart = False
    with LOCK:
        for k, v in patch.items():
            if k in SECRETS:
                if v == "********":
                    continue
                CFG[k] = str(v)
            elif k in ENUMS:
                if v not in ENUMS[k]:
                    return False, "%s must be one of %s" % (k, "|".join(ENUMS[k])), False
                CFG[k] = v
            elif k.endswith("RGB") and k[:-3] in COLOR_NAMES:
                if not isinstance(v, str) or not HEX.match(v):
                    return False, "%s must be #rrggbb" % k, False
                CFG[k] = v.lower()
            elif (k.endswith("WW") or k.endswith("CW")) and k[:-2] in COLOR_NAMES:
                CFG[k] = max(0, min(255, int(v)))
            elif k == "hmsIgnoreList":
                codes = [c.strip().upper().replace("-", "_")
                         for c in re.split(r"[\s,;]+", str(v)) if c.strip()]
                bad = [c for c in codes if not re.match(r"^HMS(_[0-9A-F]{4}){4}$", c)]
                if bad:
                    return False, "invalid HMS code(s): " + ", ".join(bad), False
                CFG[k] = ",".join(codes)[:255]
            elif isinstance(defaults()[k], bool):
                CFG[k] = bool(v)
            elif isinstance(defaults()[k], int):
                lo, hi = RANGES.get(k, (0, 2 ** 31))
                try:
                    CFG[k] = max(lo, min(hi, int(v)))
                except (TypeError, ValueError):
                    return False, "%s must be a number" % k, False
            else:
                CFG[k] = str(v)
            if k in NETKEYS:
                restart = True
    return True, None, restart


# ----------------------------------------------------------------------- simulation

STAGE_NAMES = {
    -2: "Offline", -1: "Idle", 0: "Printing", 1: "Auto bed leveling", 2: "Heatbed preheating",
    3: "Sweeping XY mech mode", 4: "Changing filament", 5: "M400 pause",
    6: "Paused: filament runout", 7: "Heating hotend", 8: "Calibrating extrusion",
    9: "Scanning bed surface", 10: "Inspecting first layer", 11: "Identifying build plate",
    12: "Calibrating Micro Lidar", 13: "Homing toolhead", 14: "Cleaning nozzle tip",
    15: "Checking extruder temperature", 16: "Paused by user", 17: "Paused: front cover falling",
    20: "Paused: nozzle temperature malfunction", 21: "Paused: heat bed temperature malfunction",
    34: "Paused: first layer error", 35: "Paused: nozzle clog", 255: "Idle",
}
GCODE_STATES = ["IDLE", "PREPARE", "SLICING", "RUNNING", "PAUSE", "FINISH", "FAILED", "INIT", "OFFLINE"]
HMS_SEVERITY = {1: "Fatal", 2: "Serious", 3: "Common", 4: "Info"}

# Phase table: (fraction of the cycle, stage, gcode state)
PHASES = [
    (0.11, -1, "IDLE"),
    (0.04, 2, "PREPARE"),
    (0.09, 2, "PREPARE"),
    (0.05, 7, "PREPARE"),
    (0.04, 14, "PREPARE"),
    (0.08, 1, "PREPARE"),
    (0.04, 8, "PREPARE"),
    (0.04, 9, "PREPARE"),
    (0.03, 13, "RUNNING"),
    (0.04, 10, "RUNNING"),
    (0.28, 0, "RUNNING"),
    (0.16, -1, "FINISH"),
]

SAMPLE_HMS = [
    ("HMS_0300_1200_0002_0001", 2, "Motion Controller"),
    ("HMS_0C00_0100_0002_0007", 3, "XCam"),
    ("HMS_0700_2000_0002_0001", 2, "AMS"),
    ("HMS_0500_0300_0001_0002", 1, "Mainboard"),
]

JOBS = ["Benchy_0.2mm_PLA.3mf", "voron_cube.gcode.3mf", "bracket_v4_PETG.3mf", "gridfinity_6x3.3mf"]


class Sim(object):
    def __init__(self, cycle, always_hms=False, offset=0.0):
        self.cycle = cycle
        self.always_hms = always_hms
        self.t0 = time.time() - offset
        self.boot = time.time()
        self.cycles = -1
        self.job = JOBS[0]
        self.total_layers = 214
        self.hms = []
        self.door_open = False
        self.door_edges = 0
        self.chamber_light = True
        self.work_light = False
        self.online = True
        self.reconnects = 2
        self.override = None          # dict(color, effect, until, brightness)
        self.identify_until = 0
        self.finish_started = 0
        self.last_activity = time.time()
        self.mqtt_ext_connected = True
        self.wifi_scan_started = 0
        self.discover_started = 0
        self.pending_restart = 0

    # -- phase ----------------------------------------------------------------
    def phase(self):
        t = (time.time() - self.t0) % self.cycle
        n = int((time.time() - self.t0) // self.cycle)
        if n != self.cycles:
            self.cycles = n
            self.job = random.choice(JOBS)
            self.total_layers = random.randint(80, 420)
            self.hms = []
            if self.always_hms or random.random() < 0.35:
                code, sev, mod = random.choice(SAMPLE_HMS)
                self.hms = [{"code": code, "severity": HMS_SEVERITY[sev], "module": mod}]
        acc = 0.0
        for i, (frac, stage, gs) in enumerate(PHASES):
            span = frac * self.cycle
            if t < acc + span:
                return i, stage, gs, (t - acc) / span, t
            acc += span
        return len(PHASES) - 1, -1, "FINISH", 1.0, t

    def state(self):
        idx, stage, gs, frac, t = self.phase()
        printing = PHASES[idx][1] == 0
        progress = int(frac * 100) if printing else (0 if idx < 10 else 100)
        layer = int(progress / 100.0 * self.total_layers) if idx >= 8 else 0
        remaining = int((100 - progress) / 100.0 * (self.cycle * PHASES[10][0] / 60.0) * 26) if printing else 0

        heating = idx in (1, 2, 3)
        bed_tgt = 0.0 if gs in ("IDLE", "FINISH") else 60.0
        noz_tgt = 0.0 if gs in ("IDLE", "FINISH") else 220.0
        ramp = min(1.0, frac + (0.35 if idx > 3 else 0.0)) if heating else (1.0 if noz_tgt else 0.0)
        bed = 24 + (bed_tgt - 24) * (ramp if bed_tgt else 0.0) + random.uniform(-.4, .4)
        noz = 24 + (noz_tgt - 24) * (ramp if noz_tgt else 0.0) + random.uniform(-.8, .8)
        if gs == "FINISH":
            cool = frac
            bed = 60 - 30 * cool
            noz = 220 - 150 * cool
        chamber = 28 + 12 * (progress / 100.0) + random.uniform(-.3, .3)

        # door pops open shortly after the print finishes
        want_door = gs == "FINISH" and frac > 0.35
        if want_door != self.door_open:
            self.door_open = want_door
            self.door_edges += 1
            self.last_activity = time.time()

        hms = []
        for h in self.hms:
            ignored = h["code"] in CFG.get("hmsIgnoreList", "").split(",")
            hms.append(dict(h, ignored=ignored))
        active = [h for h in hms if not h["ignored"]]
        highest = "None"
        if active:
            order = {"Fatal": 1, "Serious": 2, "Common": 3, "Info": 4}
            highest = sorted(active, key=lambda h: order.get(h["severity"], 9))[0]["severity"]

        return {
            "connected": self.online, "ip": CFG["printerIP"], "serial": CFG["serialNumber"],
            "model": "X1C", "fw": "01.08.02.00", "lastReportSec": int(time.time()) % 2,
            "gcodeState": gs, "stage": stage, "stageName": STAGE_NAMES.get(stage, "Unknown"),
            "overrideStage": 999,
            "progress": progress, "remainingMin": remaining,
            "layer": layer, "totalLayers": self.total_layers if idx >= 8 else 0,
            "nozzleTemp": round(noz, 1), "nozzleTarget": round(noz_tgt, 1),
            "bedTemp": round(bed, 1), "bedTarget": round(bed_tgt, 1),
            "chamberTemp": round(chamber, 1),
            "fanPart": 100 if printing else 0,
            "fanAux": 93 if printing else 0,
            "fanChamber": 40 if printing else 0,
            "fanHeatbreak": 100 if gs == "RUNNING" else 0,
            "chamberLight": self.chamber_light, "workLight": self.work_light,
            "doorOpen": self.door_open, "sdcard": True, "speedLevel": 2,
            "jobName": self.job if idx >= 1 else "",
            "printType": "local" if idx >= 1 else "",
            "printError": 0, "wifiSignal": -30,
            "ams": {"present": True, "trayNow": 0, "trayColor": "#f4a300", "humidity": 4},
            "hms": hms, "hmsHighest": highest,
        }

    # -- LED decision (a simplified ledEvaluate for the preview) ---------------
    def led(self, p):
        def col(name):
            return (CFG[name + "RGB"], CFG[name + "WW"], CFG[name + "CW"])

        def out(hexrgb, ww, cw, effect, reason, override=False, remain=0):
            h = hexrgb.lstrip("#")
            return {"mode": CFG["ledMode"], "r": int(h[0:2], 16), "g": int(h[2:4], 16),
                    "b": int(h[4:6], 16), "ww": ww, "cw": cw,
                    "brightness": CFG["brightness"], "effect": effect, "reason": reason,
                    "override": override, "overrideRemainingSec": remain,
                    "identify": time.time() < self.identify_until}

        now = time.time()
        if CFG["ledMode"] == "off":
            return out("#000000", 0, 0, "solid", "LED mode: off")
        if self.override:
            o = self.override
            remain = 0 if not o["until"] else max(0, int(o["until"] - now))
            if o["until"] and now > o["until"]:
                self.override = None
            else:
                return out(o["hex"], o["ww"], o["cw"], o["effect"], "Manual override", True, remain)
        if now < self.identify_until:
            return out("#ffffff", 255, 255, "fastblink", "Identify")
        if CFG["ledMode"] == "maintenance":
            return out(*(col("maintenance") + ("solid", "LED mode: maintenance")))
        if CFG["ledMode"] == "test":
            return out(*(col("test") + ("solid", "LED mode: test")))
        if CFG["ledMode"] == "wifi":
            return out("#00ff00", 0, 0, "solid", "WiFi -52 dBm")
        if CFG["ledMode"] == "rainbow":
            return out("#000000", 0, 0, "rainbow", "LED mode: rainbow")

        stage, gs = p["stage"], p["gcodeState"]
        if CFG["errorDetection"] and p["hmsHighest"] in ("Fatal", "Serious"):
            name = "hmsFatal" if p["hmsHighest"] == "Fatal" else "hmsSerious"
            return out(*(col(name) + (CFG["errorEffect"], "HMS %s" % p["hmsHighest"])))
        if CFG["errorDetection"] and p["hmsHighest"] == "Common" and CFG["hmsCommonEnabled"]:
            return out(*(col("hmsCommon") + (CFG["errorEffect"], "HMS Common")))
        if gs == "PAUSE" or stage in (16, 30):
            return out(*(col("pause") + (CFG["pauseEffect"], "Paused")))
        if CFG["followChamberLight"] and not self.chamber_light:
            return out("#000000", 0, 0, "solid", "Chamber light off")
        if CFG["lidarStagesEnabled"] and stage in (14, 1, 8, 9, 10):
            return out(*(col("stage%d" % stage) + ("solid", "Lidar stage %d" % stage)))
        if gs == "FINISH" and CFG["finishIndication"]:
            return out(*(col("finish") + (CFG["finishEffect"], "Print finished")))
        if stage in (2, 7):
            r, w, c = col("running")
            if CFG["preheatVisual"] == "tempglow":
                ratio = max(p["nozzleTemp"] / max(p["nozzleTarget"], 1), p["bedTemp"] / max(p["bedTarget"], 1))
                ratio = max(0.0, min(1.0, ratio))
                s = 0.15 + 0.85 * ratio
                w, c = int(w * s), int(c * s)
            return out(r, w, c, "solid", "Preheating (stage %d)" % stage)
        if stage == 0 and gs == "RUNNING":
            r, w, c = col("running")
            eff = "solid"
            if CFG["printingVisual"] == "breathe":
                eff = "breathe"
            elif CFG["printingVisual"] == "progress":
                fr, fw, fc = col("finish")
                t = p["progress"] / 100.0
                a = [int(r.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4)]
                b = [int(fr.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4)]
                r = "#%02x%02x%02x" % tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
                w = int(w + (fw - w) * t)
                c = int(c + (fc - c) * t)
            return out(r, w, c, eff, "Printing (stage 0)")
        return out(*(col("running") + ("solid", "Idle (stage %d)" % stage)))

    def status(self):
        p = self.state()
        led = self.led(p)
        finish_active = p["gcodeState"] == "FINISH" and CFG["finishIndication"]
        inact = max(0, CFG["inactivityMins"] * 60 - int(time.time() - self.last_activity))
        return {
            "device": {
                "fw": FW, "host": CFG["host"], "ip": "10.0.42.33", "mac": MAC,
                "rssi": -52 - int(6 * random.random()), "uptimeSec": int(time.time() - self.boot),
                "heapFree": 148000 + random.randint(-4000, 4000), "heapMin": 121344,
                "apMode": False, "mdns": CFG["host"] + ".local", "chip": "ESP32-WROOM-32",
                "sdk": "v5.5.1-arduino-3.3.2",
            },
            "printer": p,
            "led": led,
            "timers": {
                "finishActive": finish_active,
                "finishRemainingSec": CFG["finishTimerMins"] * 60 if (finish_active and CFG["finishExitMode"] == "timer") else 0,
                "inactivityRemainingSec": inact if CFG["inactivityEnabled"] else 0,
                "idleOff": CFG["inactivityEnabled"] and inact == 0,
                "doorToggleOff": False,
            },
            "mqtt": {
                "printer": {"connected": self.online, "state": 0 if self.online else -1,
                            "stateText": "Connected" if self.online else "Disconnected",
                            "reconnects": self.reconnects},
                "external": {"enabled": CFG["mqttExtEnabled"],
                             "connected": CFG["mqttExtEnabled"] and self.mqtt_ext_connected,
                             "state": 0 if CFG["mqttExtEnabled"] else -1,
                             "stateText": "Connected" if (CFG["mqttExtEnabled"] and self.mqtt_ext_connected) else "Disconnected"},
            },
        }


SIM = None

# ----------------------------------------------------------------------- websocket

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_frame(payload, opcode=0x1):
    data = payload.encode("utf-8") if isinstance(payload, str) else payload
    head = bytearray([0x80 | opcode])
    n = len(data)
    if n < 126:
        head.append(n)
    elif n < 65536:
        head.append(126)
        head += struct.pack(">H", n)
    else:
        head.append(127)
        head += struct.pack(">Q", n)
    return bytes(head) + data


def ws_read(sock):
    """Read one client frame. Returns (opcode, payload) or None on close/error."""
    def rd(n):
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    h = rd(2)
    if not h:
        return None
    op = h[0] & 0x0F
    masked = h[1] & 0x80
    ln = h[1] & 0x7F
    if ln == 126:
        e = rd(2)
        if not e:
            return None
        ln = struct.unpack(">H", e)[0]
    elif ln == 127:
        e = rd(8)
        if not e:
            return None
        ln = struct.unpack(">Q", e)[0]
    key = rd(4) if masked else b"\0\0\0\0"
    if key is None:
        return None
    data = rd(ln) if ln else b""
    if data is None:
        return None
    if masked:
        data = bytes(b ^ key[i % 4] for i, b in enumerate(data))
    return op, data


# -------------------------------------------------------------------------- server

def parse_multipart(body, content_type):
    """Very small multipart/form-data reader -> {field: (filename, bytes)}."""
    m = re.search(r'boundary="?([^";]+)"?', content_type or "")
    if not m:
        return {}
    bnd = ("--" + m.group(1)).encode()
    out = {}
    for part in body.split(bnd):
        if not part.strip(b"-\r\n"):
            continue
        head, _, data = part.partition(b"\r\n\r\n")
        hm = re.search(rb'name="([^"]*)"', head)
        if not hm:
            continue
        fm = re.search(rb'filename="([^"]*)"', head)
        out[hm.group(1).decode()] = (fm.group(1).decode() if fm else None, data.rstrip(b"\r\n"))
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "BLLEDmock/" + FW

    # -- helpers -----------------------------------------------------------
    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("%s  %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def err(self, msg, code=400):
        self.send_json({"error": msg}, code)

    def body_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            return json.loads(raw.decode() or "{}")
        except ValueError:
            return None

    def send_bytes(self, data, ctype, code=200, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    # -- routing -----------------------------------------------------------
    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/ws":
            return self.websocket()

        if path == "/api/status":
            return self.send_json(SIM.status())
        if path in ("/api/config", "/getConfig"):
            return self.send_json(public_config())
        if path in ("/api/config/backup", "/configfile.json"):
            with LOCK:
                body = json.dumps(CFG, indent=2).encode()
            return self.send_bytes(body, "application/json", 200,
                                   {"Content-Disposition": 'attachment; filename="blledconfig.json"'})
        if path in ("/api/printers", "/printerList"):
            if SIM.discover_started and time.time() - SIM.discover_started < 2:
                return self.send_json([])
            return self.send_json([
                {"ip": CFG["printerIP"] or "10.0.42.159", "usn": CFG["serialNumber"] or "00M09D501400123", "model": "X1C"},
                {"ip": "10.0.42.171", "usn": "01P00A340900456", "model": "P1S"},
            ])
        if path == "/api/wifi/scan":
            if not SIM.wifi_scan_started or time.time() - SIM.wifi_scan_started > 30:
                SIM.wifi_scan_started = time.time()
                return self.send_json({"scanning": True})
            if time.time() - SIM.wifi_scan_started < 2.5:
                return self.send_json({"scanning": True})
            SIM.wifi_scan_started = 0
            nets = [
                {"ssid": CFG["wifiSSID"] or "Fritz!Box 7590 2.4", "bssid": "3c:a6:2f:11:02:aa", "rssi": -47, "channel": 6, "secure": True},
                {"ssid": "Fritz!Box 7590 Guest", "bssid": "3c:a6:2f:11:02:ab", "rssi": -52, "channel": 6, "secure": False},
                {"ssid": "workshop-ap", "bssid": "b8:27:eb:99:41:07", "rssi": -68, "channel": 11, "secure": True},
                {"ssid": "Bambu-X1C-AP", "bssid": "00:1a:2b:3c:4d:5e", "rssi": -74, "channel": 1, "secure": True},
                {"ssid": "neighbour_5G_ext", "bssid": "9c:3d:cf:00:11:22", "rssi": -85, "channel": 3, "secure": True},
            ]
            return self.send_json({"networks": sorted(nets, key=lambda n: -n["rssi"])})
        if path == "/api/info":
            return self.send_json({
                "fw": FW, "build": BUILD, "codename": "Balder", "chip": "ESP32-D0WD-V3",
                "chipRev": 3, "cores": 2, "flashSize": 4194304,
                "sketchSize": 1189744, "sketchFree": 707872, "sdk": "v5.5.1-arduino-3.3.2",
                "pins": {"r": 19, "g": 18, "b": 21, "ww": 22, "cw": 23},
                "libs": {"ArduinoJson": "7.4.2", "PubSubClient": "2.8", "ESPAsyncWebServer": "3.7.10"},
            })
        if path.startswith("/api/"):
            return self.err("not found", 404)

        return self.static(path)

    def do_PUT(self):
        if self.path.split("?")[0] != "/api/config":
            return self.err("not found", 404)
        patch = self.body_json()
        if patch is None:
            return self.err("malformed JSON")
        ok, msg, restart = apply_config(patch)
        if not ok:
            return self.err(msg)
        SIM.last_activity = time.time()
        out = public_config()
        if restart:
            out["restartRequired"] = True
        broadcast()
        return self.send_json(out)

    def do_DELETE(self):
        if self.path.split("?")[0] == "/api/led":
            SIM.override = None
            broadcast()
            return self.send_json(SIM.status()["led"])
        return self.err("not found", 404)

    def do_POST(self):
        path = self.path.split("?")[0]
        ctype = self.headers.get("Content-Type", "")

        if path in ("/api/update", "/update"):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n)
            parts = parse_multipart(body, ctype)
            f = parts.get("firmware")
            if not f or not f[1]:
                return self.err("no firmware part in the upload")
            print("  [mock] firmware upload: %s, %d bytes (discarded)" % (f[0], len(f[1])))
            return self.send_json({"ok": True, "size": len(f[1])})

        if path in ("/api/config/restore", "/configrestore"):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n)
            parts = parse_multipart(body, ctype)
            f = parts.get("file")
            if not f:
                return self.err("no file part in the upload")
            try:
                data = json.loads(f[1].decode())
            except ValueError:
                return self.err("not valid JSON")
            if not isinstance(data, dict) or not (set(data) & known_keys()):
                return self.err("no known configuration keys in the file")
            ok, msg, _ = apply_config({k: v for k, v in data.items() if k in known_keys()})
            if not ok:
                return self.err(msg)
            broadcast()
            return self.send_json({"ok": True, "restartRequired": True})

        if path == "/api/config/reset":
            with LOCK:
                CFG.clear()
                CFG.update(defaults())
            SIM.pending_restart = time.time() + 1.5
            broadcast()
            return self.send_json({"ok": True})

        obj = self.body_json()
        if obj is None:
            return self.err("malformed JSON")

        if path == "/api/led":
            if "hex" in obj:
                hexv = str(obj["hex"])
                if not HEX.match(hexv):
                    return self.err("hex must be #rrggbb")
            else:
                try:
                    hexv = "#%02x%02x%02x" % (int(obj.get("r", 0)), int(obj.get("g", 0)), int(obj.get("b", 0)))
                except (TypeError, ValueError):
                    return self.err("r/g/b must be numbers")
            eff = obj.get("effect", "solid")
            if eff not in ENUMS["finishEffect"] + ["rainbow"]:
                return self.err("unknown effect")
            dur = int(obj.get("durationSec", 0) or 0)
            SIM.override = {"hex": hexv.lower(),
                            "ww": max(0, min(255, int(obj.get("ww", 0)))),
                            "cw": max(0, min(255, int(obj.get("cw", 0)))),
                            "effect": eff,
                            "until": time.time() + dur if dur else 0}
            broadcast()
            return self.send_json(SIM.status()["led"])

        if path == "/api/led/mode":
            if obj.get("mode") not in ENUMS["ledMode"]:
                return self.err("mode must be one of " + "|".join(ENUMS["ledMode"]))
            with LOCK:
                CFG["ledMode"] = obj["mode"]
            broadcast()
            return self.send_json(SIM.status()["led"])

        if path == "/api/led/brightness":
            try:
                b = int(obj.get("brightness"))
            except (TypeError, ValueError):
                return self.err("brightness must be a number")
            with LOCK:
                CFG["brightness"] = max(0, min(100, b))
            broadcast()
            return self.send_json(SIM.status()["led"])

        if path == "/api/led/identify":
            SIM.identify_until = time.time() + 3
            broadcast()
            return self.send_json({"ok": True})

        if path == "/api/action":
            a = obj.get("action")
            if a == "restart":
                SIM.pending_restart = time.time() + 1.5
                print("  [mock] restart requested (simulated)")
            elif a == "chamberLight":
                if "on" not in obj:
                    return self.err("chamberLight requires \"on\"")
                SIM.chamber_light = bool(obj["on"])
            elif a == "workLight":
                if "on" not in obj:
                    return self.err("workLight requires \"on\"")
                SIM.work_light = bool(obj["on"])
            elif a == "pushall":
                SIM.last_activity = time.time()
            elif a == "rescanWifi":
                SIM.wifi_scan_started = 0
            elif a == "discover":
                SIM.discover_started = time.time()
            elif a == "reconnectMqtt":
                SIM.reconnects += 1
            else:
                return self.err("unknown action: %s" % a)
            broadcast()
            return self.send_json({"ok": True})

        return self.err("not found", 404)

    # -- static ------------------------------------------------------------
    def static(self, path):
        alias = {"/": "index.html", "/wifi": "wifiSetup.html", "/webserial": "webSerialPage.html",
                 "/index.html": "index.html"}
        name = alias.get(path, path.lstrip("/"))
        full = os.path.normpath(os.path.join(self.server.root, name))
        if not full.startswith(os.path.normpath(self.server.root)) or not os.path.isfile(full):
            return self.err("not found: %s" % path, 404)
        with open(full, "rb") as fh:
            data = fh.read()
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype.endswith("javascript"):
            ctype += "; charset=utf-8"
        return self.send_bytes(data, ctype, 200, {"Cache-Control": "no-store"})

    # -- websocket ---------------------------------------------------------
    def websocket(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            return self.err("not a websocket handshake")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        self.wfile.flush()
        self.close_connection = True
        sock = self.connection
        CLIENTS.add(sock)
        last = 0.0
        try:
            while True:
                r, _, _ = select.select([sock], [], [], 0.2)
                if r:
                    fr = ws_read(sock)
                    if fr is None:
                        break
                    op, data = fr
                    if op == 0x8:
                        break
                    if op == 0x9:
                        sock.sendall(ws_frame(data, 0xA))
                    elif op == 0x1:
                        try:
                            self.ws_command(json.loads(data.decode()))
                        except ValueError:
                            pass
                now = time.time()
                if now - last >= 1.0 or WAKE.is_set():
                    last = now
                    sock.sendall(ws_frame(json.dumps(SIM.status())))
        except (OSError, BrokenPipeError):
            pass
        finally:
            CLIENTS.discard(sock)

    def ws_command(self, obj):
        cmd = obj.get("cmd")
        if cmd == "clearLed":
            SIM.override = None
        elif cmd == "led" and HEX.match(str(obj.get("hex", ""))):
            SIM.override = {"hex": obj["hex"].lower(), "ww": int(obj.get("ww", 0)),
                            "cw": int(obj.get("cw", 0)), "effect": obj.get("effect", "solid"),
                            "until": time.time() + int(obj.get("durationSec", 0) or 0)
                                     if obj.get("durationSec") else 0}


CLIENTS = set()
WAKE = threading.Event()


def broadcast():
    """Push a status frame to every connected WS client (coalesced by the reader loop)."""
    payload = ws_frame(json.dumps(SIM.status()))
    for s in list(CLIENTS):
        try:
            s.sendall(payload)
        except OSError:
            CLIENTS.discard(s)


def main():
    global SIM
    ap = argparse.ArgumentParser(description="BLLED v3 web-UI mock server")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--root", default=ROOT, help="directory to serve (default src/www)")
    ap.add_argument("--cycle", type=float, default=120.0, help="seconds per print cycle")
    ap.add_argument("--hms", action="store_true", help="always raise an HMS message (screenshots)")
    ap.add_argument("--offset", type=float, default=0.0,
                    help="start this many seconds into the cycle (screenshots)")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every request")
    args = ap.parse_args()

    SIM = Sim(args.cycle, args.hms, args.offset)
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.daemon_threads = True
    srv.root = os.path.normpath(args.root)
    srv.verbose = args.verbose
    print("BLLED mock v%s  http://localhost:%d/   (serving %s, %.0fs print cycle)"
          % (FW, args.port, srv.root, args.cycle))
    print("  /api/status  /api/config  /api/led  /api/action  /api/wifi/scan  /api/printers  /ws")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
