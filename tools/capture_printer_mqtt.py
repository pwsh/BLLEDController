#!/usr/bin/env python3
"""Capture raw MQTT reports from a Bambu printer (LAN mode) for parser testing.

Usage: capture_printer_mqtt.py <printer_ip> <serial> <access_code> [seconds] [outfile]
Sends a `pushall` after connect so the first message is a full state snapshot.
"""
import json, ssl, sys, time
import paho.mqtt.client as mqtt

ip, serial, code = sys.argv[1], sys.argv[2], sys.argv[3]
secs = int(sys.argv[4]) if len(sys.argv) > 4 else 20
out = sys.argv[5] if len(sys.argv) > 5 else "printer_capture.jsonl"

msgs = []
def on_connect(c, u, f, rc, props=None):
    print("connected rc=", rc)
    c.subscribe(f"device/{serial}/report")
    c.publish(f"device/{serial}/request", json.dumps({"pushing": {"sequence_id": "0", "command": "pushall"}}))
    c.publish(f"device/{serial}/request", json.dumps({"info": {"sequence_id": "0", "command": "get_version"}}))
def on_message(c, u, m):
    try: p = json.loads(m.payload)
    except Exception: p = {"_raw": m.payload[:200].hex()}
    msgs.append({"t": round(time.time(), 3), "topic": m.topic, "len": len(m.payload), "payload": p})
    print(f"{m.topic} {len(m.payload)}B keys={list(p.keys())}")

c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="blled-capture")
c.tls_set(cert_reqs=ssl.CERT_NONE); c.tls_insecure_set(True)
c.username_pw_set("bblp", code)
c.on_connect = on_connect; c.on_message = on_message
c.connect(ip, 8883, 10); c.loop_start(); time.sleep(secs); c.loop_stop()
with open(out, "w") as f:
    for m in msgs: f.write(json.dumps(m) + "\n")
print(f"wrote {len(msgs)} messages to {out}")
