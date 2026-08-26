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

* HTTP Basic authentication is sent in the clear; the device does not do TLS for its own web server.
* The printer's TLS certificate is not verified (`setInsecure()`), as in v2.
