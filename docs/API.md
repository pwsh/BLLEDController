# BLLED v3 — HTTP / WebSocket / MQTT API reference

Firmware 3.0.0 ("Balder"). This document is the human-readable form of
`docs/ARCHITECTURE.md` §7 and §8; the architecture document wins if the two ever disagree.

All examples assume the controller is at `10.0.42.33`. Substitute your own IP, the mDNS name
(`http://BLLED.local/`), or a mock server (`python3 tools/mock_server.py --port 8080`).

A ready-made smoke test lives in `tools/test_api.sh`:

```bash
./tools/test_api.sh 10.0.42.33                 # read-only checks
./tools/test_api.sh 10.0.42.33 admin secret    # with HTTP Basic auth
BLLED_WRITE=1 ./tools/test_api.sh 10.0.42.33   # plus the mutating calls
```

---

## 1. Conventions

* Every response is `application/json` (the config backup adds a `Content-Disposition` header;
  static assets are gzipped HTML/CSS/JS/images).
* Errors are `{"error":"human readable message"}` with status **400** (bad input),
  **401** (auth required), **404** (unknown route) or **413** (JSON body over 8 kB).
* Mutations are `POST`, `PUT` or `DELETE` only. There is no state-changing `GET` anywhere —
  the v2 `GET /factoryreset` CSRF hole is gone.
* JSON request bodies must be sent with `Content-Type: application/json`.
* Every response carries `X-Content-Type-Options: nosniff`.

### Authentication

HTTP **Basic**, applied to *every* route — API, static assets, WebSocket handshake, firmware
upload and config download included — when **both** `webUser` and `webPass` are set **and** the
device is not in setup-AP mode. In AP mode everything is open so the captive portal can work.

```bash
curl -u admin:secret http://10.0.42.33/api/status
```

Unauthenticated requests get `401` with `WWW-Authenticate: Basic realm="Login Required"`.

> Basic auth travels in the clear: the ESP32 does not terminate TLS for its own web server.
> Treat the controller as a trusted-LAN device (`docs/REVIEW.md` #31).

### Legacy aliases

Kept so old bookmarks and scripts keep working:

| Legacy | New |
|---|---|
| `GET /getConfig` | `GET /api/config` |
| `GET /configfile.json` | `GET /api/config/backup` |
| `GET /printerList` | `GET /api/printers` |
| `POST /update` | `POST /api/update` |
| `POST /configrestore` | `POST /api/config/restore` |

**Removed in v3** (all return 404): `/submitConfig`, `/submitWiFi`, `GET /factoryreset`,
`/config.json`, `/wifiScan`.

---

## 2. `GET /api/status`

The single status model. Identical over REST, over the WebSocket, and on the external MQTT
`<base>/status` topic.

```bash
curl -s http://10.0.42.33/api/status | jq
```

```json
{
  "device": {
    "fw": "3.0.0", "host": "BLLED", "ip": "10.0.42.33", "mac": "A0:B1:C2:D3:E4:F5",
    "rssi": -61, "uptimeSec": 1234, "heapFree": 123456, "heapMin": 100000,
    "apMode": false, "mdns": "BLLED.local", "chip": "ESP32-D0WD-V3", "sdk": "v5.5.1-..."
  },
  "printer": {
    "connected": true, "ip": "10.0.42.159", "serial": "00M09D5...", "model": "X1C",
    "fw": "01.08.02.00", "lastReportSec": 1,
    "gcodeState": "RUNNING", "stage": 0, "stageName": "Printing", "overrideStage": 999,
    "progress": 42, "remainingMin": 123, "layer": 10, "totalLayers": 200,
    "nozzleTemp": 220.0, "nozzleTarget": 220.0, "bedTemp": 60.0, "bedTarget": 60.0,
    "chamberTemp": 30.0,
    "fanPart": 100, "fanAux": 0, "fanChamber": 0, "fanHeatbreak": 100,
    "chamberLight": true, "workLight": false, "doorOpen": false, "doorKnown": true,
    "sdcard": true,
    "speedLevel": 2, "jobName": "Benchy.3mf", "printType": "local", "printError": 0,
    "wifiSignal": -30,
    "ams": {"present": true, "trayNow": 1, "trayColor": "#FF0000", "humidity": 3},
    "hms": [
      {"code": "HMS_0300_1200_0002_0001", "severity": "Serious",
       "module": "Motion Controller", "ignored": false}
    ],
    "hmsHighest": "Serious"
  },
  "led": {
    "mode": "auto", "r": 0, "g": 0, "b": 0, "ww": 204, "cw": 204, "brightness": 80,
    "effect": "solid", "reason": "Printing (stage 0)",
    "override": false, "overrideRemainingSec": 0, "identify": false
  },
  "timers": {
    "finishActive": false, "finishRemainingSec": 0, "inactivityRemainingSec": 3210,
    "idleOff": false, "doorToggleOff": false
  },
  "mqtt": {
    "printer":  {"connected": true, "state": 0, "stateText": "Connected", "reconnects": 2},
    "external": {"enabled": false, "connected": false, "state": -1, "stateText": "Disconnected"}
  }
}
```

Notes:

* **Temperatures are `null` when unknown** (the printer has not reported them yet, or
  `chamber_temper` was null). Everything else always has a value.
* `stageName` comes from the table in `src/blled/stages.h`; `stage` is the raw `stg_cur`
  (`-1` X1 idle, `255` P1 idle, `-2` offline).
* `ams.humidity` is the printer's humidity **index 1–5 (1 = wet, 5 = dry)**; Bambu's apps show it as letters A (driest) … E (wettest), i.e. letter = `"EDCBA"[index-1]`. `ams.trayColor` is the active tray's colour.
* `hms` lists the printer's active alerts (Bambu **HMS** = Health Management System; codes and severities as shown on the printer). `overrideStage` is the stage the HMS code table implies (`999` = none).
* `severity` is `Fatal | Serious | Common | Info`; `hmsHighest` is `None` when the list is empty
  or every entry is ignored. `module` is decoded from the HMS `attr >> 24`.
* `led.r/g/b/ww/cw` are the **actual PWM values last written** (after fade, effect and
  brightness), so a breathing LED reports a changing value.
* `led.brightness` is the persisted `brightness` setting, not the momentary effect level.
* `doorKnown` is `false` until the printer has reported a door change since boot. On an X1C whose door
  did not actuate its switch when closed the bit never changed; pressing the switch by hand flipped it.
  While `false` the UI shows a muted *Door: not reported* chip and door-based features fall back to
  their timers. (The top lid has no sensor.)
* `wifiSignal` is the *printer's* own RSSI; `device.rssi` is the controller's.
* `mqtt.*.state` is the raw `PubSubClient::state()` code (`0` connected, negative = error).

---

## 3. Configuration

### `GET /api/config`

Flat JSON with every `PrinterConfig` key (see `src/blled/types.h` for the field-by-field
meaning) plus `wifiSSID`, `host`, `webUser`, `mqttExtUser`.

**Secrets** (`wifiPass`, `webPass`, `mqttExtPass`) come back as `""` when unset and `"********"`
when set — never in the clear (`docs/REVIEW.md` #29). `accessCode` **is** returned in full: the UI
needs it to show what the printer is paired with.

```bash
curl -s http://10.0.42.33/api/config | jq
```

```json
{
  "wifiSSID": "workshop", "wifiPass": "********", "host": "BLLED",
  "webUser": "admin", "webPass": "********", "BSSID": "b8:27:eb:99:41:07",
  "printerIP": "10.0.42.159", "accessCode": "12345678", "serialNumber": "00M09D5...",
  "printerAutoIp": true, "isP1Printer": false,
  "brightness": 80, "ledMode": "auto", "fadeMs": 500, "effectSpeed": 5,
  "followChamberLight": true, "printingVisual": "solid", "preheatVisual": "solid",
  "runningRGB": "#000000", "runningWW": 255, "runningCW": 255,
  "...": "one <name>RGB/<name>WW/<name>CW triplet per colour",
  "mqttExtEnabled": false, "mqttExtHost": "", "mqttExtPort": 1883,
  "mqttExtUser": "", "mqttExtPass": "", "mqttExtBaseTopic": "",
  "mqttExtIntervalSec": 10, "haDiscovery": true, "haPrefix": "homeassistant",
  "debugVerbose": false, "debugChanges": true, "debugMqtt": false
}
```

### `PUT /api/config`

Partial merge — only the keys you send change. Values are validated and clamped
(brightness 0–100, WW/CW 0–255, minutes 0–999, ports 1–65535, strings truncated to their buffer).
Unknown keys are rejected with **400** listing them (keys the request *did* understand are still
applied).

Secret semantics: **absent or `"********"` = unchanged, `""` = clear.**

```bash
# one setting
curl -X PUT -u admin:secret -H 'Content-Type: application/json' \
     -d '{"brightness":60}' http://10.0.42.33/api/config

# a whole tab's worth
curl -X PUT -H 'Content-Type: application/json' -d '{
  "ledMode": "auto", "fadeMs": 800, "effectSpeed": 7,
  "printingVisual": "progress", "runningRGB": "#ffffff",
  "runningWW": 200, "runningCW": 200
}' http://10.0.42.33/api/config

# change the WiFi password (network key -> restart required)
curl -X PUT -H 'Content-Type: application/json' \
     -d '{"wifiSSID":"workshop","wifiPass":"hunter2"}' http://10.0.42.33/api/config
```

The response is the full config exactly as `GET /api/config` returns it, plus
`"restartRequired": true` when one of the **network keys** actually changed value:

```
wifiSSID  wifiPass  BSSID  host  printerIP  serialNumber  accessCode  webUser  webPass
```

The device does **not** restart by itself — call `POST /api/action {"action":"restart"}` when the
user is ready. Everything else applies live.

`{"rescanWiFiNetwork": true}` is accepted as a transient request (re-pin the strongest AP for the
configured SSID); it is never persisted.

Errors:

```bash
$ curl -sX PUT -H 'Content-Type: application/json' -d '{"nope":1}' http://10.0.42.33/api/config
{"error":"unknown or invalid keys: nope"}
```

### `GET /api/config/backup`

Downloads `blledconfig.json` with **every** setting **including the plaintext WiFi password,
access code and web login**. It is a backup — treat the file like a password.

```bash
curl -u admin:secret -OJ http://10.0.42.33/api/config/backup
```

### `POST /api/config/restore`

`multipart/form-data`, one file part (the UI calls it `file`; any part name works). The upload is
streamed to `/blledconfig.tmp`, must parse as JSON and contain at least one key the firmware
understands, and only then replaces `/blledconfig.json`. Authentication is checked **before the
first byte is written** (`docs/REVIEW.md` #28). Files over 32 kB are rejected.

```bash
curl -u admin:secret -F "file=@blledconfig.json" http://10.0.42.33/api/config/restore
# {"ok":true,"restartRequired":true}    -- the device reboots ~1.5 s later
```

v2 backups are accepted: the legacy key names and value semantics are migrated on load.

### `POST /api/config/reset`

Deletes the config file and reboots into factory defaults (setup AP).

```bash
curl -u admin:secret -X POST -H 'Content-Type: application/json' -d '{}' \
     http://10.0.42.33/api/config/reset
# {"ok":true}
```

---

## 4. LED control

### `POST /api/led` — set a manual override

The override sits at priority 2 of the LED ladder: it beats everything except `ledMode: off`.

| field | type | meaning |
|---|---|---|
| `hex` | `"#rrggbb"` | RGB colour (mutually exclusive with `r`/`g`/`b`) |
| `r`, `g`, `b` | 0–255 | RGB colour |
| `ww`, `cw` | 0–255 | warm-white / cold-white channels (default 0) |
| `effect` | `solid`\|`breathe`\|`blink`\|`fastblink`\|`rainbow` | default `solid` |
| `durationSec` | 0–86400 | 0 or absent = until cleared |
| `brightness` | 0–100 | temporary, restored to the saved value on clear |

```bash
# orange for 30 s
curl -X POST -H 'Content-Type: application/json' \
     -d '{"hex":"#ff8800","durationSec":30}' http://10.0.42.33/api/led

# warm white at 40 %, blinking, until cleared
curl -X POST -H 'Content-Type: application/json' \
     -d '{"r":0,"g":0,"b":0,"ww":255,"cw":80,"effect":"blink","brightness":40}' \
     http://10.0.42.33/api/led
```

Returns the `led` object from `/api/status`. Bad input → 400
(`{"error":"hex must be #rrggbb"}`, `{"error":"effect must be solid|breathe|blink|fastblink|rainbow"}`,
`{"error":"brightness must be 0..100"}`, `{"error":"durationSec must be 0..86400"}`).

### `DELETE /api/led` — clear the override

```bash
curl -X DELETE http://10.0.42.33/api/led
```

### `POST /api/led/mode` — persist the LED mode

```bash
curl -X POST -H 'Content-Type: application/json' -d '{"mode":"maintenance"}' \
     http://10.0.42.33/api/led/mode
```

`auto` | `maintenance` | `test` | `rainbow` | `wifi` | `off`. Returns the `led` object.

### `POST /api/led/brightness` — persist the brightness

```bash
curl -X POST -H 'Content-Type: application/json' -d '{"brightness":80}' \
     http://10.0.42.33/api/led/brightness
```

0–100. **0 forces every channel to 0.** Returns the `led` object.

### `POST /api/led/identify` — three white blinks

```bash
curl -X POST -H 'Content-Type: application/json' -d '{}' http://10.0.42.33/api/led/identify
# {"ok":true}
```

---

## 5. Actions and discovery

### `POST /api/action`

```bash
curl -X POST -H 'Content-Type: application/json' -d '{"action":"restart"}'                 http://10.0.42.33/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"chamberLight","on":true}'  http://10.0.42.33/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"workLight","on":false}'    http://10.0.42.33/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"pushall","force":true}'    http://10.0.42.33/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"rescanWifi"}'              http://10.0.42.33/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"discover"}'                http://10.0.42.33/api/action
curl -X POST -H 'Content-Type: application/json' -d '{"action":"reconnectMqtt"}'           http://10.0.42.33/api/action
```

| action | effect | extra fields |
|---|---|---|
| `restart` | reboot the controller after ~1.5 s | — |
| `chamberLight` | drive the printer's chamber light | `on` (**required**) |
| `workLight` | drive the printer's work light | `on` (**required**) |
| `pushall` | ask the printer for a full state report | `force` (bypass the 5-minute rate limit) |
| `rescanWifi` | scan and pin the strongest BSSID for the configured SSID | — |
| `discover` | start an SSDP round for Bambu printers | — |
| `reconnectMqtt` | drop and re-establish the printer MQTT connection | — |

All return `{"ok":true}`. An unknown action or a missing `on` returns 400.

Printer commands are queued and sent by the MQTT task, so a `200` means "accepted", not
"the printer has acted on it" — watch `/api/status` for the result.

### `GET /api/printers`

The SSDP discovery cache.

```bash
curl -s http://10.0.42.33/api/printers
# [{"ip":"10.0.42.159","usn":"00M09D5...","model":"X1C"}]
```

Discovery is a background state machine; kick a fresh round with
`POST /api/action {"action":"discover"}` and poll this endpoint a few seconds later.

### `GET /api/wifi/scan`

Asynchronous. The first call starts a scan and returns `{"scanning":true}`; poll until the list
arrives (sorted strongest first, up to 32 entries).

```bash
curl -s http://10.0.42.33/api/wifi/scan
# {"scanning":true}
curl -s http://10.0.42.33/api/wifi/scan
# {"networks":[{"ssid":"workshop","bssid":"b8:27:eb:99:41:07","rssi":-48,"channel":6,"secure":true}]}
```

### `GET /api/info`

Static build and hardware facts.

```bash
curl -s http://10.0.42.33/api/info | jq
```

```json
{
  "fw": "3.0.0", "build": "Aug 25 2026 22:41:03", "codename": "Balder",
  "chip": "ESP32-D0WD-V3", "chipRev": 3, "cores": 2,
  "flashSize": 4194304, "sketchSize": 1367534, "sketchFree": 598546,
  "sdk": "v5.5.1-arduino-3.3.2",
  "pins": {"r": 19, "g": 18, "b": 21, "ww": 22, "cw": 23},
  "libs": {"ArduinoJson": "7.4.3", "ESPAsyncWebServer": "3.12.0",
           "AsyncTCP": "3.5.0", "MycilaWebSerial": "8.2.3"}
}
```

### `POST /api/update` — OTA firmware upload

`multipart/form-data`, one file part (the UI calls it `firmware`). **Authenticated** — the v2
endpoint was wide open (`docs/REVIEW.md` #27).

```bash
curl -u admin:secret -F "firmware=@.pio/build/esp32dev/firmware.bin" \
     http://10.0.42.33/api/update
# {"ok":true}      -- the device reboots ~1.5 s later
```

A rejected or corrupt image returns **400** with the `Update` library's reason, e.g.
`{"error":"firmware update failed: Bad Size Given"}`, and the running firmware is untouched.

---

## 6. WebSocket `/ws`

```
ws://10.0.42.33/ws
```

Same HTTP Basic auth as everything else (checked at the handshake; the browser reuses the
credentials it already sent for the page). The web UI falls back to polling `/api/status` every
2 s if the socket cannot be opened.

**Server → client**: the `/api/status` object, once per second while at least one client is
connected, plus an immediate push (coalesced to at most one per 200 ms) whenever the printer
state or the LED output changed.

**Client → server** (optional, JSON text frames under 512 bytes):

```json
{"cmd":"led","hex":"#00ff00","effect":"breathe","durationSec":60}
{"cmd":"led","r":255,"g":0,"b":0,"ww":0,"cw":0,"brightness":50}
{"cmd":"clearLed"}
```

Same semantics as `POST /api/led` / `DELETE /api/led`. There is no per-command reply; the next
status push reflects the change.

```bash
websocat ws://10.0.42.33/ws
# {"device":{...},"printer":{...},...}
```

At most 4 simultaneous clients are kept; the oldest are dropped.

---

## 7. External MQTT broker

Off by default. Enable it in **Connection → External MQTT** or over the API:

```bash
curl -X PUT -H 'Content-Type: application/json' -d '{
  "mqttExtEnabled": true,
  "mqttExtHost": "10.0.42.10", "mqttExtPort": 1883,
  "mqttExtUser": "blled", "mqttExtPass": "secret",
  "mqttExtBaseTopic": "blled/workshop",
  "mqttExtIntervalSec": 10,
  "haDiscovery": true, "haPrefix": "homeassistant"
}' http://10.0.42.33/api/config
```

Plain TCP only (no TLS). Client id `BLLED-<last-3-MAC-bytes>`. Reconnect back-off 5 s → 60 s.
`mqttExtBaseTopic` defaults to `blled/<host>` when left empty. Enabling, disabling or changing
the broker settings takes effect immediately — no restart needed.

`mqtt.external` in `/api/status` reports the live connection state.

### Published topics

| Topic | Retained | Payload |
|---|---|---|
| `<base>/availability` | yes | `online`, or `offline` via the MQTT Last Will |
| `<base>/status` | yes | the full `/api/status` object, every `mqttExtIntervalSec` **and** within 1 s of any change |
| `<base>/led` | yes | the `led` sub-object, on change (rate-limited to 2 Hz) |
| `<base>/light` | yes | Home Assistant JSON-light state, on change |

`<base>/status` is 1.5–2 kB and is streamed straight to the socket, so the client buffer stays at
512 bytes.

```bash
mosquitto_sub -h 10.0.42.10 -v -t 'blled/workshop/#'
```

```json
# blled/workshop/light
{"state":"ON","brightness":204,"color_mode":"rgb","color":{"r":255,"g":136,"b":0},"effect":"solid"}
```

`state` is `ON` while a manual override is active. `brightness` is 0–255 (the 0–100 setting
scaled). `color` is the override colour, or the current engine output when `OFF`.

### Subscribed topics

#### `<base>/set` — the BLLED command shape

Any combination of:

```json
{"hex":"#ff0000","ww":0,"cw":0,"effect":"breathe","durationSec":300,"brightness":50}
{"clear":true}
{"mode":"maintenance"}
{"brightness":80}
{"chamberLight":true}
{"identify":true}
```

* A colour (`hex` or any of `r`/`g`/`b`/`ww`/`cw`) applies an override; `brightness` alongside it
  is the override's temporary brightness.
* A **bare** `{"brightness":n}` (no colour, no mode) persists the saved brightness instead.
* `mode` persists `ledMode`.

```bash
mosquitto_pub -h 10.0.42.10 -t blled/workshop/set -m '{"hex":"#00ff00","durationSec":60}'
mosquitto_pub -h 10.0.42.10 -t blled/workshop/set -m '{"clear":true}'
```

#### `<base>/cmd` — plain text, for simple automations and the HA buttons

| Payload | Effect |
|---|---|
| `ON` | white override until cleared |
| `OFF` | clear the override |
| `IDENTIFY` | three white blinks |
| `PUSHALL` | ask the printer for a full state report |
| `RESTART` | reboot the controller |

```bash
mosquitto_pub -h 10.0.42.10 -t blled/workshop/cmd -m ON
```

#### `<base>/light/set` — Home Assistant's JSON-light command shape

HA publishes its own fixed shape here (it cannot be templated — see `docs/HA-DISCOVERY.md` §3a):

```json
{"state":"ON","brightness":200,"color":{"r":255,"g":100,"b":50},"effect":"solid"}
{"state":"OFF"}
```

`OFF` **clears the override** (the LEDs return to whatever the automatic logic wants), it does not
force them dark — use `{"mode":"off"}` on `<base>/set` for that.

---

## 8. Home Assistant discovery

With `haDiscovery: true` (the default) the controller publishes classic per-entity retained
discovery configs under `<haPrefix>/<component>/blled_<mac6>_<key>/config` on every broker
connect, and whenever `host`, `haPrefix` or the base topic changes. Setting `haDiscovery: false`
publishes an empty retained payload to each of those topics once, which removes the entities from
Home Assistant.

Every payload uses the `~` topic shorthand and the abbreviated keys from `docs/HA-DISCOVERY.md`
§2, stays under 500 bytes, and points `avty_t` at `<base>/availability`. Only the light carries
the full `dev` + `o` blocks; the others send `"dev":{"ids":["blled_<mac6>"]}` and let HA merge.

Device block:

```json
{"ids":["blled_a1b2c3"],"name":"<host>","mf":"DutchDeveloper","mdl":"BLLED",
 "sw":"3.0.0","cu":"http://10.0.42.33/"}
```

### Entities (22)

| Component | `uniq_id` suffix | Name | Source |
|---|---|---|---|
| `light` | `_light` | *(device name — main entity, `name: null`)* | `<base>/light` ⇄ `<base>/light/set`, JSON schema, brightness, rgb, effects |
| `select` | `_mode` | LED mode | `led.mode` → `<base>/set` |
| `number` | `_brightness` | Brightness | `led.brightness` 0–100 % → `<base>/set` |
| `sensor` | `_stage` | Stage | `printer.stageName` |
| `sensor` | `_gcodestate` | G-code state | `printer.gcodeState` |
| `sensor` | `_progress` | Progress | `printer.progress` % |
| `sensor` | `_remaining` | Remaining | `printer.remainingMin` min, `dev_cla: duration` |
| `sensor` | `_layer` | Layer | `printer.layer` |
| `sensor` | `_totallayers` | Total layers | `printer.totalLayers` |
| `sensor` | `_nozzletemp` | Nozzle temperature | `printer.nozzleTemp` °C, `dev_cla: temperature` |
| `sensor` | `_bedtemp` | Bed temperature | `printer.bedTemp` °C, `dev_cla: temperature` |
| `sensor` | `_chambertemp` | Chamber temperature | `printer.chamberTemp` °C, `dev_cla: temperature` |
| `sensor` | `_ledreason` | LED reason | `led.reason` |
| `sensor` | `_hmshighest` | Printer alert level | `printer.hmsHighest` |
| `sensor` | `_rssi` | WiFi signal | `device.rssi` dBm, `dev_cla: signal_strength`, diagnostic |
| `binary_sensor` | `_connected` | Printer connected | `printer.connected`, `dev_cla: connectivity`, diagnostic |
| `binary_sensor` | `_door` | Door | `printer.doorOpen`, `dev_cla: door` |
| `binary_sensor` | `_chamberlight` | Chamber light | `printer.chamberLight`, `dev_cla: light` |
| `binary_sensor` | `_finishactive` | Finish indication | `timers.finishActive` |
| `button` | `_identify` | Identify | `<base>/cmd` ← `IDENTIFY` |
| `button` | `_pushall` | Refresh printer state | `<base>/cmd` ← `PUSHALL`, diagnostic |
| `button` | `_restart` | Restart controller | `<base>/cmd` ← `RESTART`, `dev_cla: restart`, config |

Every nested `val_tpl` is guarded with `| default(...)` so a `null` temperature or a missing key
leaves the entity `unknown` instead of throwing a silent Jinja error
(`docs/HA-DISCOVERY.md` §3d/§7).

Inspect what was published:

```bash
mosquitto_sub -h 10.0.42.10 -v -t 'homeassistant/+/blled_+/config'
```

### Example automation

```yaml
automation:
  - alias: "Flash the printer LEDs red on a fatal printer alert"
    trigger:
      - platform: state
        entity_id: sensor.blled_hms_highest_severity
        to: "Fatal"
    action:
      - service: mqtt.publish
        data:
          topic: blled/workshop/set
          payload: '{"hex":"#ff0000","effect":"fastblink","durationSec":600}'
```

---

## 9. Static routes

| Route | Content |
|---|---|
| `GET /` | the SPA (`index.html`); **redirects to `/wifi` in setup-AP mode** |
| `GET /index.html`, `/app.js`, `/style.css`, `/blled.svg`, `/favicon.png`, `/favicon.ico` | gzipped assets baked into the firmware |
| `GET /wifi` | the first-time setup page (`wifiSetup.html`) |
| `GET /webserial` | the live log console |

HTML is served `Cache-Control: no-cache`; JS/CSS `max-age=3600`; images `max-age=86400`. All
assets are gzipped (`Content-Encoding: gzip`) — there are no external requests, so the UI works
offline on a phone.

Unknown routes return `{"error":"not found"}` with 404, except in AP mode where the captive-portal
catch-all redirects everything to `/wifi`.

> The WebSerial log stream (`/webserialws`) uses the same HTTP Basic credentials as every other route (open in AP mode).
