# BLLED v3 Architecture & API Contract

This is the binding design for the v3 rework. Three workstreams build against it in parallel:
**core firmware** (LED engine, state, parsing, persistence), **API + external MQTT**, and **web UI**.
The shared interface is `src/blled/types.h` + `src/blled/stages.h` (already written) and the JSON
contracts below. If something here is ambiguous, follow `types.h`; if you must deviate, document it
in the file header and in `docs/CHANGELOG.md`.

## 1. Goals

1. Fix the concurrency, blocking and logic defects in `docs/REVIEW.md`.
2. Keep the exact hardware behaviour users rely on (5-channel PWM on GPIO 19/18/21/22/23, same default
   colours, same stage/error semantics) while making the LED engine non-blocking.
3. Expose everything the printer tells us and everything the controller decides through one status
   model, served identically over REST, WebSocket and (optionally) an external MQTT broker with
   Home Assistant discovery.
4. A responsive, modern single-page UI with clear grouping and a tooltip on every setting.
5. Arduino core 3.3.x (pioarduino), current library versions, version `3.0.0`.

## 2. Threading model (non-negotiable)

| Task | Owns | May only |
|---|---|---|
| **main loop** (Arduino `loop()`, core 1) | LED hardware (`ledc*`), `ledRuntime`, timers, LittleFS writes, mDNS/SSDP, WiFi reconnect, printer discovery, WebSocket pushes | read `printerState`/`printerConfig` under lock; call `mqttEnqueue()` |
| **mqttTask** (FreeRTOS, core 1, prio 1, 20 kB) | both `PubSubClient`s (printer TLS 8883 and optional external broker), JSON parsing of reports | write `printerState` under lock and set `printerStateDirty`; drain the command queue |
| **AsyncTCP task** (HTTP/WS handlers) | request/response only | read state under lock to build JSON; write `printerConfig`/override fields under lock; set `configDirty`/`ledDirty`; call `mqttEnqueue()`; **never** touch LEDs, LittleFS or PubSubClient directly |

Primitives (in `types.h`): `stateMutex` (recursive), `STATE_LOCK()`/`STATE_UNLOCK()`, `volatile bool
printerStateDirty, ledDirty, configDirty, configReloadRequested, restartRequested`. Hold the lock
only long enough to copy/assign plain fields — never while doing I/O, logging or JSON serialisation
of large documents (copy into a local `PrinterState` snapshot first; it is a POD struct).

Logging from `mqttTask`: use `LogSerial` **only for infrequent messages** (connect/disconnect,
errors). Per-message debug output (`debugMqtt`) must go through `Serial` directly, not WebSerial.

## 3. Modules and ownership

```
src/main.cpp                 core   setup()/loop(); AP-mode captive portal; restart handling
src/blled/types.h            (done) all structs, enums, globals, lock macros, cross-module declarations
src/blled/stages.h           (done) stage/gcode/HMS name tables + serial-prefix → model
src/blled/leds.h             core   LED engine + state evaluation (section 4)
src/blled/filesystem.h       core   config load/save/migrate/validate (section 5)
src/blled/mqttmanager.h      core   printer MQTT task, report parsing (section 6), command queue
src/blled/AutoGrowBufferStream.h core  fixed (size_t, no per-message shrink)
src/blled/wifi-manager.h     core   non-blocking reconnect/back-off, async scan helper
src/blled/bblPrinterDiscovery.h core non-blocking SSDP discovery state machine
src/blled/serialmanager.h    core   bounds-checked provisioning
src/blled/ssdp.h             core   unchanged except lib 2.x
src/blled/logSerial.h        core   unchanged
src/blled/api.h              api    buildStatusJson(), buildConfigJson(), applyConfigJson(), /api/* handlers
src/blled/mqttpublish.h      api    external broker client, HA discovery, command topic
src/blled/web-server.h       api    static routes, auth, WS, OTA, legacy aliases; calls api.h
src/www/index.html + app.js + style.css   ui   SPA (section 8)
src/www/wifiSetup.html       ui     captive-portal page (standalone, same CSS)
src/www/particleCanvas.js, blled.svg, favicon.png  ui  keep/replace as desired
tools/mock_server.py         ui     Python mock implementing section 7 for UI development
tools/capture_printer_mqtt.py, tools/fixtures_x1c_pushall.json  (done) real X1C data
docs/                        all    ARCHITECTURE.md, REVIEW.md, API.md, CHANGELOG.md, manual.md
```

`pre_build.py` gzips every `*.html|js|css|svg|png` in `src/www/` into `www.h` as
`<name_with_underscores>_gz`, `_gz_len`, `_gz_mime`. Keep total compressed web assets **< 60 kB**.

## 4. LED engine (`leds.h`)

### 4.1 Evaluation → decision → engine

```
ledEvaluate()   // main loop, when ledDirty || printerStateDirty || a timer fired (≥10 Hz check)
  snapshot printerState under lock
  compute LedDecision { COLOR color; LedEffect effect; char reason[48]; float progress; }
  ledApply(decision)  // sets engine target; no-op if unchanged
ledTick()       // main loop every iteration: fade + effect modulation + brightness → ledcWrite
```

Priority ladder for `ledEvaluate()` (first match wins; must reproduce upstream semantics, see
`docs/REVIEW.md` for the bugs to *not* reproduce):

1. `ledMode == Off` → black, reason "LED mode: off"
2. API/MQTT override active (`ledRuntime.overrideUntilMs > millis()` or permanent) → override colour/effect, reason "Manual override"
3. Identify request → 3 white blinks then clear (reason "Identify")
4. `ledMode == Maintenance` → `maintenanceColor` (default WW+CW 255)
5. `ledMode == Test` → `testColor`
6. `ledMode == WifiStrength` → RSSI colour (same thresholds as upstream)
7. `ledMode == Rainbow` → effect Rainbow (brightness-scaled, time-based hue)
8. Boot/WiFi-connecting phases → `wifiColor` (only before `globalVariables.started`)
9. Door double-close toggle (`ledRuntime.doorToggleOff`) → black, reason "Toggled off via door"
10. **Errors** (if `errorDetection`): stage/override 6, 17, 20, 21 → respective colours; HMS highest severity Fatal/Serious → `hmsFatalColor`/`hmsSeriousColor`; Common (if `hmsCommonEnabled`) → `hmsCommonColor`. Effect = `errorEffect`.
11. **Pause**: stage 16/30 or gcode PAUSE → `pauseColor`; stage 34 → `firstLayerColor`; 35 → `nozzleClogColor`; effect = `pauseEffect`.
12. Printer offline (`!online` for ≥ `offlineTimeoutSec`) → black, reason "Printer offline"
13. `followChamberLight && !chamberLight` → black, reason "Chamber light off"
14. Lidar stages (if `lidarStagesEnabled` or always when configured non-black): 14, 1, 8, 9, 10 (also override 10), 12 → stage colours
15. Inactivity timeout expired (idle stages −1/255, not in finish-wait, `inactivityEnabled`) → black; request chamber light off if `controlChamberLight`
16. Finish active (`ledRuntime.finishActive && finishIndication`) → `finishColor`, effect `finishEffect`
17. Preheat (stage 2, or 7 heating hotend) → `runningColor`; if `preheatVisual == TempGlow` modulate (section 4.3)
18. Printing (stage 0 && RUNNING, or gcode RUNNING with stage 3/4/11/13/15/19/22/24 sub-stages) → `runningColor`; `printingVisual` applies
19. Idle (−1/255), FAILED, PREPARE, SLICING, INIT, OFFLINE/−2 → `runningColor`
20. Fallback → keep current (reason "No rule")

`overrideStage` is cleared when the HMS list no longer contains the code that set it, and on
IDLE/FINISH/FAILED/RUNNING transitions. `finishActive` starts on `FINISH` transition and ends on a door
edge (`finishExitMode == door`, 6 s debounce as upstream) or `finishTimerMins` (`timer`), then the
inactivity timer restarts. Door edges (from `printerState.doorEdgeCount`) restart inactivity, force
chamber light on (if `controlChamberLight`), and a close within 2 s of an open toggles
`doorToggleOff` (if `doorToggleEnabled`). Any change from MQTT clears `idleOffActive` and restarts
the inactivity timer, as upstream.

### 4.2 Engine (`ledTick`)

* Target and current channel values are `float[5]` (r,g,b,ww,cw, 0–255). Fade is time-based:
  `fadeMs` (default 500) linear interpolation from the value at the moment the target changed.
* Effect modulation multiplies a scalar `m ∈ [0,1]` computed from `millis()` and `effectSpeed` (1–10):
  `Solid` m=1; `Breathe` m = 0.25+0.75·(0.5+0.5·sin) with period 6 s→1.5 s across speeds;
  `Blink` 50 % duty, period 1.2 s→0.3 s; `FastBlink` period 0.3 s→0.1 s; `Rainbow` ignores colour,
  hue rotates 60 s→6 s per cycle, RGB only.
* Global brightness `brightness/100` applied last; **brightness 0 must produce all channels 0**.
* Write `ledcWrite` only when the rounded 8-bit value of a channel changed.
* `ledRuntime.output[5]` always holds the last written values (for the API).

### 4.3 Visualisations (new, config-selectable)

* `printingVisual`: `solid` | `progress` (blend `runningColor → finishColor` by `mc_percent`, so a
  print visibly "ripens" toward the finish colour) | `breathe`.
* `preheatVisual`: `solid` | `tempglow` (colour = `runningColor` scaled by 0.15 + 0.85·ratio where
  ratio = max(nozzle/nozzleTarget, bed/bedTarget) clamped 0..1; below 30 % ratio add a red tint).
* `errorEffect`, `pauseEffect`, `finishEffect`: `solid|breathe|blink|fastblink` (defaults solid).
* `effectSpeed` 1–10 (default 5), `fadeMs` 0–5000 (default 500).

### 4.4 Cross-task LED API (declared in `types.h`, defined in `leds.h`)

```cpp
void ledRequestOverride(const COLOR& c, LedEffect e, uint32_t durationMs); // 0 = until cleared
void ledClearOverride();
void ledRequestIdentify();
```
These only set `ledRuntime` fields under lock and `ledDirty = true`.

## 5. Persistence (`filesystem.h`)

* File `/blledconfig.json`, **flat** keys (backup files stay human-editable). Key names are the
  `PrinterConfig` field names in `types.h` (comments there give each key).
* `loadConfig()` is table-driven: every key has a default; missing keys never crash. Colours are
  three keys `<name>RGB` (`"#rrggbb"`), `<name>WW`, `<name>CW` exactly as upstream.
* **Legacy migration** on load (upstream v2 keys): `maintMode→ledMode=maintenance`,
  `discoMode→rainbow`, `showtestcolor→test`, `debugwifi→wifi`, `replicatestate→followChamberLight`,
  `finishExit(bool)→finishExitMode`, `finishTimerMins` when value > 1000 treat as ms,
  `inactivityTimeOut(ms)→inactivityMins`, `doorSwitch→lidarStagesEnabled`, `p1Printer→isP1Printer`,
  `debuging→debugVerbose`, `debugingchange→debugChanges`, `mqttdebug→debugMqtt`, `bssi→BSSID`,
  `printerIp→printerIP`, `appw→wifiPass`, `ssid→wifiSSID`.
* `validateConfig()` clamps: brightness 0–100, WW/CW 0–255, mins 0–999, ports 1–65535, string
  lengths to buffer sizes, `hmsIgnoreList` normalised once (upper-case, `-`→`_`, comma-separated).
* `saveConfig()` is called by the main loop when `configDirty` (never from the async task).
* Restore upload: buffer to `/blledconfig.tmp`, `deserializeJson` must succeed and contain at least
  one known key, then rename.

## 6. Printer MQTT (`mqttmanager.h`)

* On connect: subscribe `device/<serial>/report`, enqueue `PushAll` and `GetVersion`. Re-`pushall`
  at most every 5 min and whenever `gcode_state` is unknown after 10 s.
* Parse with an ArduinoJson filter for exactly the fields in `PrinterState` (see `types.h` comments
  for the JSON source of each field). Fan speeds arrive as strings `"0".."15"` → percent.
  `home_flag` is a signed 32-bit int; use `(uint32_t)` before bit tests. `chamber_temper` may be null.
  `lights_report` has `chamber_light` and `work_light` with modes `on|off|flashing`.
* HMS: build `HmsEntry` for every element (max 8 kept, most severe first), mark `ignored` via the
  normalised ignore list, set `hmsHighestSeverity` = most severe non-ignored, set `overrideStage`
  from the known code table, clear when absent.
* `info.get_version` reply → `printerFw` (module `ota`).
* Set `printerStateDirty = true` after any field changed; also `lastReportMs = millis()`.
* Command queue: `bool mqttEnqueue(MqttCmd, int32_t arg)` (8-slot ring, `portMUX`), drained in the
  task loop: `ChamberLight(arg 0/1)`, `WorkLight`, `PushAll`, `GetVersion`. `controlChamberLight()`
  keeps its name as a thin wrapper that enqueues.
* After each `mqttClient.loop()` the task calls `mqttPublishLoop()` (from `mqttpublish.h`, api
  workstream) — a weak no-op default is declared in `types.h` so core compiles without it.

## 7. HTTP / WebSocket API (`api.h`, `web-server.h`)

Auth: HTTP Basic when `webUser`/`webPass` set **and** not in AP mode — applies to **every** route
including OTA, restore, WS and static assets. Mutations are `POST`/`PUT`/`DELETE` only.
All JSON responses `application/json`; errors `{"error":"message"}` with 400/401/404/500.
Legacy aliases kept: `GET /getConfig` → `/api/config`, `GET /configfile.json` → `/api/config/backup`,
`GET /printerList` → `/api/printers`, `POST /update` → `/api/update`, `POST /configrestore` →
`/api/config/restore`. `/submitConfig`, `/submitWiFi`, `/factoryreset` (GET) and `/config.json` are
**removed**.

### 7.1 `GET /api/status` (also the WebSocket `/ws` message, pushed every 1 s and immediately on `printerStateDirty`/LED change)

```json
{
  "device": {"fw":"3.0.0","host":"BLLED","ip":"10.0.42.33","mac":"AA:BB:..","rssi":-61,"uptimeSec":1234,
             "heapFree":123456,"heapMin":100000,"apMode":false,"mdns":"BLLED.local","chip":"ESP32","sdk":"..."},
  "printer": {"connected":true,"ip":"10.0.42.159","serial":"00M09D5...","model":"X1C","fw":"01.08.02.00",
              "lastReportSec":1,
              "gcodeState":"RUNNING","stage":0,"stageName":"Printing","overrideStage":999,
              "progress":42,"remainingMin":123,"layer":10,"totalLayers":200,
              "nozzleTemp":220.0,"nozzleTarget":220.0,"bedTemp":60.0,"bedTarget":60.0,"chamberTemp":30.0,
              "fanPart":100,"fanAux":0,"fanChamber":0,"fanHeatbreak":100,
              "chamberLight":true,"workLight":false,"doorOpen":false,"sdcard":true,"speedLevel":2,
              "jobName":"Benchy.3mf","printType":"local","printError":0,"wifiSignal":-30,
              "ams":{"present":true,"trayNow":1,"trayColor":"#FF0000","humidity":3},
              "hms":[{"code":"HMS_0300_1200_0002_0001","severity":"Serious","module":"Motion Controller","ignored":false}],
              "hmsHighest":"Serious"},
  "led": {"mode":"auto","r":0,"g":0,"b":0,"ww":204,"cw":204,"brightness":80,"effect":"solid",
          "reason":"Printing (stage 0)","override":false,"overrideRemainingSec":0,"identify":false},
  "timers": {"finishActive":false,"finishRemainingSec":0,"inactivityRemainingSec":3210,"idleOff":false,"doorToggleOff":false},
  "mqtt": {"printer":{"connected":true,"state":0,"stateText":"Connected","reconnects":2},
           "external":{"enabled":false,"connected":false,"state":-1,"stateText":"Disconnected"}}
}
```
`stageName` from `stages.h`; `severity` names Fatal/Serious/Common/Info; `module` from `attr>>24`.
Temperatures are numbers (null when unknown). `wifiSignal` is the printer's own RSSI in dBm.

### 7.2 Config

* `GET /api/config` → flat JSON of every `PrinterConfig` key **plus** `wifiSSID`, `host`, `webUser`,
  `mqttExtUser`. Secrets (`wifiPass`, `webPass`, `mqttExtPass`) are returned as `""` if unset or
  `"********"` if set; `accessCode` is returned in full (needed to verify pairing).
* `PUT /api/config` body = flat JSON, **partial allowed** (only present keys change). Values are
  validated/clamped; unknown keys → 400 listing them. Secrets: `"********"` or absent = unchanged,
  `""` = clear. Network keys (`wifiSSID`, `wifiPass`, `BSSID`, `host`, `printerIP`, `serialNumber`,
  `accessCode`, `webUser`, `webPass`) are saved and the response includes `"restartRequired": true`
  (the device does **not** restart automatically; UI offers the button). Everything else applies
  live. Response: full config JSON as `GET`.
* `GET /api/config/backup` → file download `blledconfig.json` (secrets **included**; it is a backup).
* `POST /api/config/restore` multipart `file` → validated then written; response
  `{"ok":true,"restartRequired":true}`.
* `POST /api/config/reset` → factory defaults (config file deleted), restart.

### 7.3 LED control

* `POST /api/led` body `{"hex":"#ff8800"}` or `{"r":..,"g":..,"b":..,"ww":..,"cw":..}` plus optional
  `"effect":"solid|breathe|blink|fastblink"`, `"durationSec": 30` (0/absent = until cleared),
  `"brightness": 0-100` (temporary, restored on clear). Applies an override; returns `led` object.
* `DELETE /api/led` → clear override.
* `POST /api/led/mode` `{"mode":"auto|maintenance|test|rainbow|wifi|off"}` → persists `ledMode`.
* `POST /api/led/brightness` `{"brightness":80}` → persists.
* `POST /api/led/identify` → 3 blinks.

### 7.4 Actions & discovery

* `POST /api/action` `{"action":"restart"|"chamberLight"|"workLight"|"pushall"|"rescanWifi"|"discover"|"reconnectMqtt", "on":true}`
  → `{"ok":true}`. `chamberLight/workLight` require `"on"`.
* `GET /api/printers` → `[{"ip":"..","usn":"..","model":"X1C"}]` (from discovery cache).
* `GET /api/wifi/scan` → `{"scanning":true}` (starts async scan) or
  `{"networks":[{"ssid":"..","bssid":"..","rssi":-50,"channel":6,"secure":true}]}` sorted by RSSI.
* `GET /api/info` → static: `{"fw","build","codename","chip","chipRev","cores","flashSize","sketchSize","sketchFree","sdk","pins":{"r":19,"g":18,"b":21,"ww":22,"cw":23},"libs":{...}}`.
* `POST /api/update` multipart `firmware` → OTA; `{"ok":true}` then restart after 1.5 s.

### 7.5 WebSocket `/ws`

Server → client: the `/api/status` object. Client → server (optional): `{"cmd":"led","hex":..}` same
semantics as `POST /api/led`; `{"cmd":"clearLed"}`. Push at 1 Hz when clients connected, plus
immediately when `printerStateDirty` or the LED output changed (coalesced to ≥ 200 ms).

## 8. External MQTT & Home Assistant (`mqttpublish.h`)

Config keys: `mqttExtEnabled` (false), `mqttExtHost`, `mqttExtPort` (1883), `mqttExtUser`,
`mqttExtPass`, `mqttExtBaseTopic` (default `blled/<host>`), `mqttExtIntervalSec` (10),
`haDiscovery` (true), `haPrefix` (`homeassistant`). Client id `BLLED-<mac6>`. Non-TLS only.
Runs inside `mqttTask` via `mqttPublishLoop()`; reconnect back-off 5 s → 60 s.

Topics (base = `mqttExtBaseTopic`):
* `<base>/availability` retained `online` / LWT `offline`
* `<base>/status` retained, the `/api/status` object, every `mqttExtIntervalSec` **and** within 1 s of a change
* `<base>/led` retained, the `led` object, on change
* `<base>/light` retained, HA JSON-light state shape `{"state":"ON|OFF","brightness":0-255,"color_mode":"rgb","color":{"r":..,"g":..,"b":..},"effect":"solid"}`
  (ON = override active; brightness scaled from 0–100; colour = override colour, or current output when OFF)
* `<base>/light/set` subscribed, HA JSON-light command shape (`state`, optional `brightness`, `color`, `effect`) → override on / clear on OFF.
  HA's JSON light schema cannot use templates on a nested payload — see `docs/HA-DISCOVERY.md` (binding for the HA work).
* `<base>/set` subscribed; JSON with any of: `{"hex"|"r,g,b,ww,cw","effect","durationSec"}` (override),
  `{"clear":true}`, `{"mode":"auto|..."}`, `{"brightness":n}`, `{"chamberLight":true}`, `{"identify":true}`
* `<base>/cmd` subscribed, plain payloads `ON`/`OFF` (override white / clear) for simple automations

HA discovery (retained, published on connect and when `host` changes; removed with empty payload when
disabled). Device block: identifiers `["blled_<mac>"]`, name `<host>`, manufacturer `DutchDeveloper`,
model `BLLED`, sw_version fw, configuration_url `http://<ip>/`. Entities (object_id `blled_<mac>_<key>`), classic per-entity discovery with `~` shorthand and abbreviated keys (each payload < 500 B; only the light carries the full `dev` block, the others `{"ids":..}` only):
* `light` (`schema: json`, brightness, `sup_clrm:["rgb"]`, `effect` + `fx_list`) with `stat_t: ~/light`, `cmd_t: ~/light/set`, `name: null` (main entity)
* `select` LED mode; `number` brightness (0–100)
* `sensor`: stage name, gcode state, progress (%), remaining (min), layer, total layers, nozzle/bed/chamber temp (°C, device_class temperature), LED reason, HMS highest, wifi rssi (device RSSI)
* `binary_sensor`: printer connected (connectivity), door open (door), chamber light (light), finish active
* `button`: identify, pushall, restart
All entity `state_topic`s point at `<base>/status` with `value_template`s so the payload count stays small.

## 9. Web UI (`src/www/`)

Single page `index.html` (+ `app.js`, `style.css`; all inlined into `www.h` by the build, **no
external CDN**; must work offline on a phone). Vanilla JS (no framework), ES2018, responsive from
360 px to desktop, dark theme by default with light preference via `prefers-color-scheme`. Uses
`/api/*` only; live data via `/ws` with a polling fallback (`/api/status` every 2 s).

Navigation (tabs on desktop, bottom nav on mobile):

1. **Dashboard** — live: printer card (model, gcode state + stage name badge, progress ring, layer,
   remaining time, temps with targets, fans, door/chamber light/SD chips, job name, HMS list with
   severity badges and Bambu wiki links, `hmsIgnore` quick-add), LED card (strip preview showing
   actual output colour + effect animation, reason text, mode selector, brightness slider, override
   colour picker with WW/CW and "apply for N minutes" + clear, identify), device card (WiFi RSSI
   meter, IP/mDNS, uptime, heap, MQTT states), timers (finish / inactivity countdowns).
2. **LED Behaviour** — mode (auto/maintenance/test/rainbow/wifi/off with description), brightness,
   fade time, effect speed, follow chamber light + running colour, maintenance colour, test colour,
   printing visual, preheat visual.
3. **Print Events** — finish indication (colour, effect, exit by door/timer + minutes), inactivity
   timeout, control chamber light, door double-close toggle, printer type (P1 → lidar defaults),
   lidar stage colours (14, 1, 8, 9, 10).
4. **Errors & Alerts** — error detection master switch, error effect, pause effect, colours for pause,
   first-layer error, nozzle clog, HMS serious, HMS fatal, HMS common (optional), filament runout,
   front cover, nozzle temp, bed temp, HMS ignore list editor (one per line, validated format).
5. **Connection** — WiFi (SSID/password with scan picker, BSSID pin + rescan-strongest, controller
   name), Printer (IP with discovery picker, serial, access code, model auto-detected), Web UI auth,
   External MQTT (enable, host/port/user/pass, base topic, interval, HA discovery + prefix, connection
   state). Shows "restart required" banner with a restart button after saving network keys.
6. **System** — firmware version/build info, OTA upload with progress, backup download / restore
   upload, factory reset (confirm), restart, debug toggles (verbose/changes/MQTT), link to
   `/webserial` log, link to docs.

Every setting: label, `?` tooltip (hover + tap), and an inline description where non-obvious. Colour
controls: one compound `ColorField` component = RGB picker + WW + CW sliders + live swatch that
composites RGB and white channels. Saving: per-tab "Save" (PUT only that tab's keys), unsaved-changes
indicator, toast feedback, disabled while in flight. Tooltips text lives in one JS map keyed by config
key so it is easy to review.

`wifiSetup.html` (captive portal at `/wifi`, AP mode): SSID scan + password + controller name +
printer IP/serial/access code + discovery; posts `PUT /api/config` then `POST /api/action restart`.

`tools/mock_server.py` (stdlib `http.server` + a tiny WS implementation, or `websockets` if
available) serves `src/www/` and implements every endpoint in section 7 with fake but realistic data
(cycle through printing → finish; random HMS) so the UI can be developed and screenshot-tested
without hardware. `python3 tools/mock_server.py --port 8080`.

## 10. Versioning, docs

* `custom_version = 3.0.0`; codename stays `Balder`.
* `docs/API.md` — human-readable copy of section 7/8 with examples (api workstream).
* `docs/manual.md` — rewritten for the new UI groups (ui workstream reuses tooltip texts).
* `docs/CHANGELOG.md` — every user-visible change and every removed/renamed key.
