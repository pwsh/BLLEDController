---
title: REST & WebSocket API
parent: Guides
nav_order: 2
---

# REST & WebSocket API
{: .no_toc }

Everything the web interface does is a plain HTTP call, so the controller automates well. This page
is the tour; the [API reference](../API.md) is the complete contract.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

The examples use `192.168.1.50` — substitute your controller's address or its `.local` name.

---

## The rules

- Every response is JSON. Errors look like `{"error":"human readable message"}` with status
  **400**, **401**, **404** or **413**.
- Anything that changes state is a `POST`, `PUT` or `DELETE`. There is no state-changing `GET`
  anywhere.
- JSON bodies must be sent with `Content-Type: application/json`.
- If you have set a web login it is **HTTP Basic on every route** — API, static files, WebSocket
  handshake, firmware upload and backup download alike:

  ```bash
  curl -u admin:secret http://192.168.1.50/api/status
  ```

  In setup-AP mode everything is open, so that the captive portal can work.

## Read everything: `GET /api/status`

One object with `device`, `printer`, `led`, `timers` and `mqtt`. It is the same object the
dashboard renders, the same one the WebSocket pushes, and the same one that goes to your MQTT
broker.

```bash
curl -s http://192.168.1.50/api/status | jq
```

```json
{
  "device":  { "fw": "3.0.0", "host": "blled", "ip": "192.168.1.50", "rssi": -50, "uptimeSec": 264, "…": "…" },
  "printer": { "connected": true, "gcodeState": "RUNNING", "stageName": "Printing",
               "progress": 41, "remainingMin": 96, "layer": 44, "totalLayers": 108,
               "nozzleTemp": 220, "bedTemp": 60, "doorOpen": false, "doorKnown": true,
               "hms": [], "hmsHighest": "None", "…": "…" },
  "led":     { "mode": "auto", "r": 0, "g": 0, "b": 0, "ww": 230, "cw": 230,
               "brightness": 90, "effect": "solid", "reason": "Printing (stage 0)",
               "override": false },
  "timers":  { "finishActive": false, "inactivityRemainingSec": 3342, "…": "…" },
  "mqtt":    { "printer": { "connected": true }, "external": { "enabled": false } }
}
```

Useful bits: `led.reason` is the human-readable explanation of the current colour, and
`printer.doorKnown` tells you whether the door state can be trusted at all
(see [Door sensor](door-sensor.md)).

## Change settings: `PUT /api/config`

Partial JSON — send only the keys you want to change.

```bash
# one setting
curl -X PUT -H 'Content-Type: application/json' \
     -d '{"brightness":60}' http://192.168.1.50/api/config

# a few at once
curl -X PUT -H 'Content-Type: application/json' \
     -d '{"printingVisual":"progress","preheatVisual":"tempglow","fadeMs":800}' \
     http://192.168.1.50/api/config
```

`GET /api/config` returns the current configuration (passwords redacted). Changing a network key —
WiFi, printer, controller name, web login — comes back with `restartRequired: true`; the controller
does not restart itself.

Key names are the ones in the [Settings reference](../manual.md); colours are three keys each,
`<name>RGB`, `<name>WW`, `<name>CW`.

## Drive the LEDs: `/api/led`

```bash
# orange for 30 seconds
curl -X POST -H 'Content-Type: application/json' \
     -d '{"hex":"#ff8800","durationSec":30}' http://192.168.1.50/api/led

# warm white at 40 %, blinking, until cleared
curl -X POST -H 'Content-Type: application/json' \
     -d '{"ww":255,"cw":0,"brightness":40,"effect":"blink","durationSec":0}' \
     http://192.168.1.50/api/led

# hand the strip back to the automatic logic
curl -X DELETE http://192.168.1.50/api/led
```

An override sits near the top of the priority ladder, so it beats almost everything the printer
might be doing. `durationSec: 0` means "until cleared".

Persisting a mode or brightness (rather than overriding) has its own endpoints:

```bash
curl -X POST -H 'Content-Type: application/json' -d '{"mode":"maintenance"}' \
     http://192.168.1.50/api/led/mode
curl -X POST -H 'Content-Type: application/json' -d '{"brightness":75}' \
     http://192.168.1.50/api/led/brightness
curl -X POST http://192.168.1.50/api/led/identify      # three white blinks
```

## Make it do things: `POST /api/action`

```bash
curl -X POST -H 'Content-Type: application/json' -d '{"action":"restart"}'                http://192.168.1.50/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"chamberLight","on":true}' http://192.168.1.50/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"pushall","force":true}'   http://192.168.1.50/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"discover"}'               http://192.168.1.50/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"reconnectMqtt"}'          http://192.168.1.50/api/action
```

Also `workLight` (with `on`) and `rescanWifi`. Printer commands are queued and sent by the MQTT
task, so `{"ok":true}` means *accepted*, not *the printer has done it* — watch `/api/status` for
the result.

## Live updates: WebSocket `/ws`

```
ws://192.168.1.50/ws
```

The server pushes the `/api/status` object once a second while a client is connected, plus an
immediate push whenever the printer state or the LED output changes. Same Basic auth as everything
else, checked at the handshake.

```bash
websocat ws://192.168.1.50/ws
```

You can also send commands over the socket, as JSON text frames:

```json
{"cmd":"led","hex":"#00ff00","effect":"breathe","durationSec":60}
{"cmd":"clearLed"}
```

There is no per-command reply — the next status push reflects the change. At most four clients are
kept; the oldest get dropped.

## Backups, resets and updates

```bash
curl -s http://192.168.1.50/api/config/backup -o blled-backup.json
curl -X POST -H 'Content-Type: application/json' --data-binary @blled-backup.json \
     http://192.168.1.50/api/config/restore
curl -X POST http://192.168.1.50/api/config/reset          # factory reset
curl -F "firmware=@BLLC_V3.0.0.bin.ota" http://192.168.1.50/api/update
```

Restore and reset reboot the device about 1.5 s after answering. See
[Backup & restore](backup-restore.md).

## Discovery helpers

```bash
curl -s http://192.168.1.50/api/printers      # the SSDP cache of Bambu printers
curl -s http://192.168.1.50/api/wifi/scan     # async: {"scanning":true}, then the list
curl -s http://192.168.1.50/api/info          # build, chip, pins, library versions
```

`/api/wifi/scan` starts a scan on the first call and returns `{"scanning":true}`; poll it until the
list arrives.

## Old paths still work

`GET /getConfig`, `GET /configfile.json`, `GET /printerList`, `POST /update` and
`POST /configrestore` are kept as aliases so old bookmarks and scripts keep working.

Gone for good (they return 404): `/submitConfig`, `/submitWiFi`, `GET /factoryreset`,
`/config.json` and `/wifiScan`.

## Caching

The static assets — `index.html`, `app.js`, `style.css`, the icons — are baked into the firmware
and gzipped, and each build stamps them with its own **ETag**. A browser that has the previous
build cached gets a `304` until you flash a new one, at which point the ETag changes and the new
files are fetched. If a phone ever shows you a stale interface after an update, a hard reload is
the fix.

## Try it all at once

`tools/test_api.sh` in the repository is a curl smoke test against a real controller:

```bash
tools/test_api.sh 192.168.1.50           # optional: BLLED_USER / BLLED_PASS for HTTP auth
```

---

The full contract — every field of every response, every error code, the exact override semantics —
is in the [API reference](../API.md).
