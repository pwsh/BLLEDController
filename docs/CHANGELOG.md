# Changelog

## 3.0.0 (unreleased) — "Balder"

Complete rework of the controller firmware. See `docs/ARCHITECTURE.md` for the design and
`docs/REVIEW.md` for the defect list this release closes.

### Behaviour changes (core firmware)

**LEDs**

* The LED engine is now non-blocking and time-based. Colour changes fade over `fadeMs`
  (default 500 ms) instead of the old "tween", which was a 256 ms blocking no-op followed by a hard
  cut. PWM registers are only written when a channel value actually changes.
* Colour decisions are re-evaluated at ≥10 Hz and immediately on any printer report, instead of only
  when an MQTT message happened to arrive. Timers (finish, inactivity, door) no longer jitter by up
  to 5.5 s while printer discovery was running.
* **Brightness 0 now switches every channel off** in every mode, including rainbow.
* **Rainbow** is time-based, respects the brightness setting and no longer writes negative duty
  cycles (which previously wrapped to full brightness).
* **HMS severity** now uses the most severe entry in the report. Previously the *last* entry won, so
  `[Serious, Info]` showed no error at all.
* An HMS-derived stage override (front cover, filament runout, nozzle/bed temperature, first-layer
  inspection) is now cleared as soon as the code disappears from the HMS list and on every
  IDLE/FINISH/FAILED/RUNNING transition. Previously it could stay red until the next reboot unless
  change-logging was switched on.
* Idle handling: with the inactivity timeout **disabled**, the LEDs keep following the printer. In
  v2 the idle branch silently stopped matching once the (disabled) timeout had elapsed and the LEDs
  froze.
* Paused-stage colours: stage 34 (first layer error) and stage 35 (nozzle clog) are now checked
  before the generic pause rule, so their configured colours are actually reachable.
* New effects for the error, pause and finish states: `solid | breathe | blink | fastblink`
  (`errorEffect`, `pauseEffect`, `finishEffect`, all default `solid`) with a shared `effectSpeed`
  (1–10, default 5).
* New `printingVisual`: `solid` (default), `progress` (blends `runningColor` → `finishColor` with
  print progress) or `breathe`.
* New `preheatVisual`: `solid` (default) or `tempglow` (brightness follows the nozzle/bed
  temperature ratio, with a red tint below 30 %).
* New override/identify API used by the REST/WS/MQTT layers: a colour+effect can be applied for a
  number of seconds or until cleared, and `identify` blinks the strip white three times.
* Stage 7 (heating hotend) now shows the running colour like stage 2 (preheating bed).
* AP/setup mode keeps the upstream pink indicator; other boot phases show `wifiColor`.

**Printer connection**

* `pushall` is requested on every (re)connect and `get_version` once, so P1/A1 printers — which only
  push deltas — show the correct state immediately after a controller reboot. Repeat `pushall`
  requests are rate-limited to one per 5 minutes, plus one if `gcode_state` is still unknown 10 s
  after connecting.
* Chamber-light commands are queued and published by the MQTT task. Previously the main loop
  published directly into the same `PubSubClient` the task was using, which could corrupt the
  connection.
* `work_light` is parsed as well as `chamber_light`, and the `flashing` mode is recognised.
* The full printer state is now tracked and exposed: progress, remaining time, layer/total layers,
  job name, print type, print error, speed level, SD card, nozzle/bed/chamber temperatures and
  targets, the four fan speeds (converted from the `"0".."15"` report strings to per cent), the
  printer's own WiFi RSSI, AMS presence/active tray/tray colour/humidity, and the full HMS list with
  severity and module.
* The HMS ignore list is normalised once when the configuration is loaded instead of four times per
  HMS entry per message.
* Per-message MQTT debug output goes to the USB serial port only; WebSerial is used for
  connect/disconnect and error lines only.

**Network / discovery**

* WiFi reconnect is non-blocking: two quick reconnect attempts, then at most one rescan per minute.
  It no longer logs on every loop iteration or blocks for 20 s per attempt.
* The WiFi scan used by the setup page is asynchronous. `GET /wifiScan` answers `{"scanning":true}`
  while a scan is in flight.
* "Use the strongest access point" now actually works — the BSSID comparison in v2 compared
  pointers and was always false.
* Printer discovery is a non-blocking state machine: at boot, on demand, and every 5 minutes **only**
  while the printer MQTT connection is down or no printer IP is configured (v2 blocked the main loop
  for ~5.5 s every 10 s, always). Discovered printers now carry a model name derived from the serial.
* The printer IP is only auto-updated from discovery when the new `printerAutoIp` option is on
  (default on).

**System**

* A failing mDNS start no longer hangs the boot forever (v2: `while(1) delay(500)` when the
  configured host name was empty — reachable by submitting the WiFi form without a name).
* A missing, truncated or hand-edited configuration file can no longer crash the device or cause a
  boot loop: every key has a default and the parser is bounds-checked.
* Colour parsing (`hex2rgb`) accepts any input. `#3F3CFBAA`, `#ffffff\n` or an empty value used to
  hang the device in an infinite loop and overflow an 8-byte buffer.
* Serial provisioning uses bounds-checked copies and accepts both the old and the new key names.
* OTA upload now requires authentication (it was completely unauthenticated).
* Config restore authenticates **before** the first byte is written and validates the uploaded JSON
  before it replaces the live configuration.
* `/config.json` and `/getConfig` never return the WiFi or web-UI password.
* The captive-portal DNS server now answers in AP mode (it was behind a flag that is never set in AP
  mode, so the portal never redirected).
* Boot WiFi connect no longer aborts to AP mode on the first transient `WL_DISCONNECTED` (upstream
  stranded the device in AP mode after a router reboot); it keeps trying for ~60 s first.
* **Captive portal:** the OS connectivity probes (`/generate_204`, `/hotspot-detect.html`,
  `/connecttest.txt`, `/ncsi.txt`, `/canonical.html`, `/success.txt`, …) are answered with an
  absolute redirect to `http://192.168.4.1/wifi`, so phones and laptops show the "sign in to
  network" prompt and land on the setup page automatically. DNS TTL lowered to 30 s.
* First hardware run fixes: the first printer report is treated as the current state, not a
  transition (a printer that finished earlier no longer lights the finish colour at boot, and the
  initial door position is not counted as a door edge); `/api/status` no longer references a
  stack buffer for `led.reason` (occasional garbage bytes in the response).
* Wording: "HMS" is now "printer alert" in the UI, manual and README (with HMS — Bambu's Health
  Management System — named where codes are involved); the dashboard shows AMS humidity as Bambu's
  A–E level with a tooltip instead of the raw 1–5 index.
* The preheat visual (`preheatVisual=tempglow`, "Heat-up blend") is now a real colour change: it
  blends from a new `preheatColor` (default orange `#FF6A00`) into the running colour as the
  slowest heater approaches its target. The first version only dimmed the white, which was invisible.
* The X1C sets the `home_flag` "door" bit when either the front door or the top lid is open (verified
  live), so the UI, tooltips and HA entity now say "door / lid".
* Any RUNNING stage the ladder does not specifically handle now counts as printing (a real X1C
  reports stage 54 while heating a 120 °C bed, which used to fall through to "No rule"), and the
  preheat visual keys on "a heater is still below its target" rather than on stage 2/7 only.
* Static web assets are served with a per-build `ETag` and `Cache-Control: no-cache` (browser
  revalidates, gets a 304): a phone can no longer keep a script from an older firmware after an
  OTA update. `pre_build.py` refuses to build if a `.js` file has a syntax error (needs `node`).
* Serial provisioning accepts `{"resetAuth":true}` to clear a forgotten web-UI password.
* **AP-mode recovery:** when the device fell back to the setup AP although credentials exist, it
  retries the station connection in the background every 2 minutes (AP+STA for 30 s) and restarts
  into normal mode as soon as it connects. The captive portal stays reachable throughout.
* `isP1Printer` now has real semantics instead of rewriting colours in the browser: lidar stage
  colours are never applied, the finish indication always ends by timer, and the door double-close
  gesture is ignored.

### Configuration keys

The configuration file `/blledconfig.json` stays flat and human-editable. **Old files are migrated
automatically on first boot** and re-saved in the new format; backups made with v2 can be restored.

| v2 key | v3 key | Note |
|---|---|---|
| `ssid` | `wifiSSID` | |
| `appw` | `wifiPass` | |
| `HTTPUser` / `HTTPPass` | `webUser` / `webPass` | |
| `bssi` | `BSSID` | |
| `printerIp` | `printerIP` | |
| `maintMode`, `discoMode`, `showtestcolor`, `debugwifi` | `ledMode` | one enum: `auto`, `maintenance`, `test`, `rainbow`, `wifi`, `off` |
| `replicatestate` | `followChamberLight` | |
| `finishindication` | `finishIndication` | |
| `finishColor` | `finishRGB` | with `finishWW` / `finishCW` as before |
| `finishExit` (bool) | `finishExitMode` | `door` or `timer` |
| `finishTimerMins` (milliseconds) | `finishTimerMins` (**minutes**) | values > 1000 are migrated |
| `inactivityTimeOut` (milliseconds) | `inactivityMins` (**minutes**) | |
| `errordetection` | `errorDetection` | |
| `doorSwitch` | `lidarStagesEnabled` | |
| `p1Printer` | `isP1Printer` | |
| `debuging` | `debugVerbose` | |
| `debugingchange` | `debugChanges` | |
| `mqttdebug` | `debugMqtt` | |
| `firstlayerRGB/WW/CW` | `firstLayerRGB/WW/CW` | |
| `nozzleclogRGB/WW/CW` | `nozzleClogRGB/WW/CW` | |
| `finish_check` | *removed* | runtime state, was never configuration |
| `webpagePassword` | *removed* | unused |

New keys: `printerAutoIp`, `fadeMs`, `effectSpeed`, `printingVisual`, `preheatVisual`,
`maintenanceRGB/WW/CW`, `finishEffect`, `errorEffect`, `pauseEffect`, `doorToggleEnabled`,
`offlineTimeoutSec`, `hmsCommonEnabled`, `hmsCommonRGB/WW/CW`, and the external-broker block
(`mqttExtEnabled`, `mqttExtHost`, `mqttExtPort`, `mqttExtUser`, `mqttExtPass`, `mqttExtBaseTopic`,
`mqttExtIntervalSec`, `haDiscovery`, `haPrefix`).

Default colours are unchanged: running and maintenance warm+cold white 255, test `#3F3CFB`, finish
`#00FF00`, WiFi/boot `#FFA500`, bed levelling `#000055`, the other lidar stages off, pause /
first-layer / nozzle-clog `#0000FF`, HMS serious+fatal / filament runout / front cover / nozzle temp
/ bed temp `#FF0000`, and the new optional HMS "common" colour `#FFA500`.

### Removed / changed endpoints

* `controlChamberLight(on)` no longer silently does nothing when the `controlChamberLight` option is
  off — the option now only gates the *automatic* chamber-light behaviour, so the API and the UI can
  always toggle the light explicitly.
* `/factoryreset` accepts POST as well as GET; GET is kept only until the new UI ships.
* The `/wifiScan` response is asynchronous (see above).

### Known limitations

* **Upgrading from v2.x needs a USB flash once.** v3 uses the `min_spiffs` partition table (the
  image is ~1.37 MB, larger than the 1.31 MB app slot of the v2 default table), so the v2 OTA page
  cannot install it. Flash `.firmware/BLLC_V3.0.0.bin` (merged image, offset 0) with the web
  flasher or esptool; afterwards OTA works normally with `.firmware/BLLC_V3.0.0.bin.ota`.
* HTTP Basic authentication is sent in the clear; the device does not do TLS for its own web server.
* The printer's TLS certificate is not verified (`setInsecure()`), as in v2.

---

## API, WebSocket, external MQTT and Home Assistant (3.0.0)

### New: one status model everywhere

`GET /api/status` (`docs/API.md` §2) exposes everything the printer reports and everything the
controller decides — gcode state, stage + stage name, progress, layer/total layers, remaining time,
nozzle/bed/chamber temperatures with targets, all four fans, door/chamber-light/work-light/SD-card
flags, job name, print type, print error, AMS summary, the full HMS list with severity, module and
"ignored" flag — plus the LED decision (actual PWM output, effect, human-readable reason, override
state) the finish/inactivity timers, and both MQTT connection states.
The **identical object** is pushed over the WebSocket and published on the external MQTT broker, so
the UI, an automation and Home Assistant all see exactly the same data (`docs/REVIEW.md` #35).

v2 exposed none of this: the WebSocket payload was 7 fields and there was no REST status endpoint
at all. Temperatures are `null` when the printer has not reported them, rather than 0.

### New endpoints

| Endpoint | Purpose |
|---|---|
| `GET /api/status` | the full status model |
| `GET /api/config` | flat config JSON, secrets masked |
| `PUT /api/config` | **partial** merge, validated, reports `restartRequired` |
| `GET /api/config/backup` | download `blledconfig.json` (secrets included) |
| `POST /api/config/restore` | multipart restore, validated before it replaces anything |
| `POST /api/config/reset` | factory reset (was a **GET**) |
| `POST /api/led` / `DELETE /api/led` | manual colour/effect override with an optional duration |
| `POST /api/led/mode` | persist `ledMode` |
| `POST /api/led/brightness` | persist `brightness` |
| `POST /api/led/identify` | three white blinks to find the device |
| `POST /api/action` | `restart`, `chamberLight`, `workLight`, `pushall`, `rescanWifi`, `discover`, `reconnectMqtt` |
| `GET /api/printers` | SSDP discovery cache |
| `GET /api/wifi/scan` | asynchronous WiFi scan |
| `GET /api/info` | firmware/build/chip/flash/SDK/pins/library versions |
| `POST /api/update` | OTA firmware upload (now authenticated) |
| `WS /ws` | the status object at 1 Hz + immediate pushes on change; accepts `{"cmd":"led"}` / `{"cmd":"clearLed"}` |

All errors are `{"error":"..."}` with a real status code (400/401/404/413) instead of plain text.

### Removed endpoints

| Removed | Replacement |
|---|---|
| `POST /submitConfig` | `PUT /api/config` |
| `POST /submitWiFi` | `PUT /api/config` + `POST /api/action {"action":"restart"}` |
| `GET /factoryreset` | `POST /api/config/reset` |
| `GET /config.json` | `GET /api/config` (it leaked the WiFi and web passwords in plaintext) |
| `GET /wifiScan` | `GET /api/wifi/scan` |

Kept as aliases: `GET /getConfig`, `GET /configfile.json`, `GET /printerList`, `POST /update`,
`POST /configrestore`.

### Security fixes

* **`POST /api/update` (OTA) now requires authentication** — in v2 any LAN client could flash
  arbitrary firmware onto the device (`docs/REVIEW.md` #27, severity Critical).
* **Config restore authenticates before the first byte is written.** In v2 the upload body handler
  overwrote `/blledconfig.json` *before* the completion handler checked auth, so an unauthenticated
  client could replace the whole configuration (#28).
* **Secrets are never echoed.** `wifiPass`, `webPass` and `mqttExtPass` come back as `""` or
  `"********"`; sending `"********"` back means "unchanged", `""` means "clear" (#29).
* **No state-changing GET requests.** `GET /factoryreset` was a one-`<img src>` CSRF factory reset;
  it is gone (#30).
* **Authentication covers every route**, including static assets, the WebSocket handshake, the
  firmware upload and the config download. AP (setup) mode stays open so the captive portal works.
* `X-Content-Type-Options: nosniff` on every response.
* Known remaining gaps, documented rather than fixed: HTTP Basic travels in the clear (#31), and the
  MycilaWebSerial log socket `/webserialws` now carries the same HTTP Basic auth as `/webserial`.

### New: external MQTT broker + Home Assistant (issue #10)

An optional second MQTT client publishes to a broker of your choice (plain TCP, no TLS), driven
from the MQTT task. New config keys: `mqttExtEnabled` (off), `mqttExtHost`, `mqttExtPort` (1883),
`mqttExtUser`, `mqttExtPass`, `mqttExtBaseTopic` (default `blled/<host>`), `mqttExtIntervalSec`
(10), `haDiscovery` (on), `haPrefix` (`homeassistant`). Enabling, disabling or repointing the
broker takes effect immediately — no restart.

Topics (base = `mqttExtBaseTopic`):

* `<base>/availability` — retained `online`, LWT `offline`
* `<base>/status` — retained, the `/api/status` object, every `mqttExtIntervalSec` and within 1 s
  of any change
* `<base>/led` — retained, the `led` sub-object, on change
* `<base>/light` — retained, Home Assistant JSON-light state
* `<base>/set` — subscribed: `{"hex"|"r,g,b,ww,cw","effect","durationSec","brightness"}`,
  `{"clear":true}`, `{"mode":...}`, `{"brightness":n}`, `{"chamberLight":bool}`, `{"identify":true}`
* `<base>/cmd` — subscribed: `ON`, `OFF`, `IDENTIFY`, `PUSHALL`, `RESTART`
* `<base>/light/set` — subscribed: Home Assistant's own JSON-light command shape

**Home Assistant discovery** publishes 22 retained per-entity configs (one light as the device's
main entity, a mode select, a brightness number, 12 sensors, 4 binary sensors and 3 buttons) using
the `~` topic shorthand and abbreviated keys, so every payload stays under 500 bytes. Turning
`haDiscovery` off publishes empty retained payloads to the same topics once, which removes the
entities from Home Assistant. See `docs/HA-DISCOVERY.md` for the payload shapes and
`docs/API.md` §8 for the entity list.

The ~2 kB status payload is streamed with `beginPublish()`/`write()`/`endPublish()` rather than
buffered, so the second MQTT client only needs a 512-byte buffer.

### Other

* `GET /api/stages` was specified in `docs/ARCHITECTURE.md` §7.4 but is **not implemented** — it had
  no consumer, and stage names are already delivered as `status.printer.stageName`.
* The external-broker hook now runs on every MQTT task pass instead of only while the *printer*
  MQTT link is up, so Home Assistant keeps receiving status (and commands keep working) when the
  printer is switched off.
* `POST /api/led/brightness` and `POST /api/led` reject out-of-range values with 400 instead of
  silently clamping, so a broken client is visible. Config keys sent through `PUT /api/config` are
  still clamped, as documented in `docs/ARCHITECTURE.md` §5.
* `tools/test_api.sh` exercises every endpoint against a device or the mock server and prints each
  status code.
