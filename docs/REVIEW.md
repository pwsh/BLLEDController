# BLLEDController v2.3.0 — Code Review Findings

Review of upstream `5137779` (v2.3.0) by reading every source file, cross-checked against a live X1C
(15.8 KB full `push_status` every ~1 s) and the community issue tracker. Severity: **C**ritical /
**H**igh / **M**edium / **L**ow. "Fix" column references the v3 rework module that resolves it
(see `ARCHITECTURE.md`).

## Concurrency & blocking (the root cause of several "LEDs stuck" reports)

| # | Sev | Finding | Fix |
|---|-----|---------|-----|
| 1 | C | `ParseCallback()` runs inside the FreeRTOS `mqttTask` and calls `updateleds()` → `tweenToColor()` (blocking ≈256 ms of `delay(1)`), while `loop()` on the other task also calls `updateleds()`/`ledsloop()`. Shared globals (`currentRed…`, `printerVariables`, `printerConfig.*_update` latches) are mutated from two tasks with no lock. `printerVariables.gcodeState`/`parsedHMSlevel` are `String`s — cross-task `String` assignment can corrupt the heap. | leds.h engine is main-loop-only; MQTT task writes `printerState` under `stateMutex` and sets `printerStateDirty`. |
| 2 | C | `controlChamberLight()` calls `mqttClient.publish()` from the **main loop** while `mqttClient.loop()` runs in `mqttTask`. PubSubClient is not thread-safe (shared TX buffer/socket). | Command queue: main loop `mqttEnqueue()`, MQTT task drains. |
| 3 | H | `bblSearchPrinters()` blocks `loop()` for **≈5.5 s every 10 s** (`delay(250)`×2 + 5000 ms receive loop) → main loop runs <50 % of the time; finish/door 6 s windows, WS pushes, inactivity timers all jitter by up to 5.5 s. Also runs even when the printer IP is known and reachable. | Non-blocking discovery state machine; periodic only when IP unknown/MQTT down; on-demand from API. |
| 4 | H | `tweenToColor()` "tween" is broken: `redStep = (target-current)/255` is integer division → 0 for any delta < 255, so it loops 256× doing nothing, then snaps. `delay(stepTime)` truncates 1.96 → 1 ms. Net effect: a blocking 256 ms no-op then a hard cut. | Time-based non-blocking fade in `ledTick()`. |
| 5 | H | `handleWiFiScan()` calls the **synchronous** `WiFi.scanNetworks()` inside an AsyncTCP handler (can take 2–5 s, trips the async watchdog / drops the STA link). | Async scan (`scanNetworks(true)`) + poll endpoint. |
| 6 | M | `loop()` WiFi-reconnect branch runs every iteration while disconnected: logs spam, `WiFi.reconnect()` twice then a **blocking** `scanNetwork()`+`connectToWifi()` (≥20 s) on every subsequent iteration. | Back-off timer, non-blocking reconnect. |
| 7 | M | `LogSerial` (WebSerial → AsyncWebSocket `textAll`) is written from the MQTT task; AsyncWebSocket is only safe from the async task/main loop. | Log from MQTT task via ring buffer flushed by main loop, or `Serial`-only in task. |
| 8 | L | `ledsloop()` ends in `delay(10)`; `RGBCycle()` is driven by loop rate, so its speed depends on whatever else blocks. | Time-based effects. |

## Logic errors

| # | Sev | Finding | Fix |
|---|-----|---------|-----|
| 9 | H | **HMS severity uses the *last* array entry, not the most severe.** `for (hms…) parsedHMSlevel = severity(entry)` → `[Serious, Info]` yields "Info" → no red. | Track highest severity across all non-ignored entries. |
| 10 | H | `overridestage` (set from specific HMS codes to 6/17/20/21/10) is only reset to 999 inside the *debug-logging* branch (`if (debuging||debugingchange) … overridestage = 999`) or on `RUNNING`. With change-logging off, a cleared HMS leaves the error colour **until reboot** — matches issues #57 "door should cancel error", #39 "idle timeout permanent". | Reset override whenever HMS list has no matching code, and on IDLE/FINISH/FAILED. |
| 11 | H | `hex2rgb()`: `while (hex.length() != 6) hex += "0"` is an **infinite loop** for any input longer than 6 chars after `#` (e.g. `#3F3CFBAA`, `#ffffff\n`) → watchdog reset; reachable via `/submitConfig` and backup restore. Also `strcpy(color.RGBhex, …)` into `char[8]` overflows for the same inputs. | Bounds-checked parser. |
| 12 | H | `loadFileSystem()`: `strcpy(dst, json["key"])` with a missing key passes `NULL` → crash → **boot loop** after restoring a partial/hand-edited backup or after a firmware adds keys. `buf` is not NUL-terminated and `deserializeJson(json, buf.get())` reads it as a C-string. | Table-driven load with defaults; `deserializeJson(json, buf, size)`; validate restored JSON before writing. |
| 13 | M | `RGBCycle()` computes `cos()`·255 → **negative** values cast to `int` and passed to `ledcWrite` (wraps to huge duty), and ignores `brightness`. | `(cos+1)/2·255·brightness`. |
| 14 | M | `AutoGrowBufferStream::_len` is `uint16_t` but `MAX_BUFFER_SIZE` is 65536: the guard `_len+1 > 65536` can never fire; `_len++` silently wraps at 65535. `get_string()` writes `_buffer[_len]=0` one past the allocation when `_len == buffer_size`. `flush()` reallocs down on every message (heap churn at 1 msg/s). | `size_t` length, keep capacity, reserve +1. |
| 15 | M | `updateleds()` "Idle" branch requires `millis()-inactivityStartms < inactivityTimeOut` even when `inactivityEnabled == false` → after the (disabled) timeout elapses in idle, no branch matches and the LEDs stop responding to idle state. | Single evaluation function with explicit idle handling. |
| 16 | M | `handleSubmitWiFi()` sets `globalVariables.Host = ""` if `host` is absent; `setupWebserver()` then does `while(1) delay(500)` when `MDNS.begin("")` fails → **hard hang at boot**. | Default host, never hang on mDNS failure. |
| 17 | M | `loop()`: `dnsServer.processNextRequest()` is inside `if (globalVariables.started)`, but `started` is never set in AP mode → captive-portal DNS never answers. | Move out of `started` guard. |
| 18 | M | Brightness and WW/CW/mins from `/submitConfig` are not clamped (`ledcWrite` with >255 wraps; brightness >100 over-drives). Issue #69 (brightness 0 %) — `int(x*0.0)` = 0 works, but `RGBCycle` ignores it and `tweenToColor` early-returns when the "already at colour" check passes, so mode switches at 0 % can be missed. | Validation layer in config apply. |
| 19 | M | `scanNetwork()`: `if (printerConfig.BSSID == bestBSSID.c_str())` compares **pointers** — always false. | `strcmp`. |
| 20 | M | `serialLoop()` uses `strcpy` from JSON fields without null/length checks into `accessCode[9]`, `serialNumber[16]`, `printerIP[16]`. | `strlcpy` + `| ""` defaults. |
| 21 | L | Door "double close" detection compares `lastdoorClosems` to `lastdoorOpenms` < 2 s (comment says 6 s; log says 2 s). Finish exit waits for a door edge within a 6 s window. Inconsistent constants scattered across files. | Named constants in one place. |
| 22 | L | `updateleds()` tail: `if (doorSwitchTriggered) updateleds();` is unreachable (flag cleared earlier or returned before) — dead recursion. `handleUploadConfigFileData()` duplicates the `/configrestore` lambda and is never registered. `generateRandomString()` leaks and is unused. `mqttloop()` is dead (replaced by the task). Commented-out blocks throughout. | Removed. |
| 23 | L | `printLogs()` rate-limits with `memcmp` over the whole `COLOR` including the uninitialised `RGBhex[8]` of stack temporaries → comparison is effectively random. | Compare channels only. |
| 24 | L | `types.h` wraps C++ structs containing `String` in `extern "C"`; globals defined in headers (works only because there is a single TU). | Plain C++; keep single-TU but drop `extern "C"`. |
| 25 | L | `saveFileSystem()` stores milliseconds under the key `finishTimerMins`; `/getConfig` divides by 60000. Works, but misleading and a trap for backup editing. | Store minutes under `finishTimerMins`. |
| 26 | L | `mqttTask()`: code after `vTaskDelete(NULL)` is unreachable. `setupSerial()`'s `while(!Serial);` is a no-op on ESP32. | Removed. |

## Security

| # | Sev | Finding | Fix |
|---|-----|---------|-----|
| 27 | C | `/update` (OTA firmware upload) has **no authentication** at all — any LAN client can flash arbitrary firmware. | Auth on all mutating routes. |
| 28 | H | `/configrestore` runs the file-upload body handler (which overwrites `/blledconfig.json`) **before** the auth check in the completion handler → unauthenticated config overwrite. | Check auth in the upload handler on `index == 0`. |
| 29 | H | `/config.json` returns the **WiFi password and web password in plaintext** (verified on a live device). `/submitWiFi`, `/wifiScan`, `/printerList` are unauthenticated in STA mode. | Never echo secrets; blank = unchanged. Auth everything in STA mode; AP mode open. |
| 30 | M | `/factoryreset` is a **GET** — a crafted `<img src>` on any page performs a factory reset (CSRF). Same for restart via config. | POST `/api/action`. |
| 31 | L | HTTP Basic over plain HTTP — inherent to the platform; documented. | — |

## Protocol / functionality gaps

| # | Sev | Finding | Fix |
|---|-----|---------|-----|
| 32 | H | No `pushing.pushall` after (re)connect. X1 pushes full state every second, but **P1/A1 only push deltas** → after a reboot the controller doesn't know `gcode_state`/`stg_cur`/light state until something changes (issue #26 "stg_cur not in message"). | `pushall` on connect (rate-limited 5 min). |
| 33 | M | Filter pulls `print_error`, `fail_reason`, `print_gcode_action`, `print_real_action`, `wifi_signal` into RAM every message but never reads them. | Either use (status API) or drop. |
| 34 | M | Only `chamber_light` is watched; `lights_report` also has `work_light` and mode `flashing` (seen live). | Parse both. |
| 35 | M | Nothing of the printer's rich state (progress, layers, temps, fans, job name, AMS, HMS list) is retained or exposed; WS payload is 7 fields; no REST status; no MQTT publish of own state; no Home Assistant integration (issue #10). | `printerState` + `/api/status` + external MQTT + HA discovery. |
| 36 | M | HMS ignore list is re-normalised (4 `String::replace`) **per HMS entry per message** (1 Hz on X1). | Normalise once on config load. |
| 37 | L | `ParseHMSSeverity` maps 1–4 but Common/Info are silently ignored — no way to show advisories (issue #41 wants them *not* red; a distinct optional colour is the better answer). | Optional `hmsCommon` colour, default off. |
| 38 | L | `mqttClient.setBufferSize(1024)` is irrelevant for payload (stream mode) but still bounds the *topic* and outbound publishes; fine today, documented. | — |

## Web UI

| # | Sev | Finding |
|---|-----|---------|
| 39 | M | `hasP1()` references `#finishdooroption`, which doesn't exist → `TypeError` aborts the handler halfway (P1 toggle leaves the form half-updated). |
| 40 | M | `showMessage()` sets `message*` attributes on `<body>` that no CSS renders → "Configuration loaded"/error messages are invisible. Two conflicting `hideMessage()` definitions. |
| 41 | L | Stray `>>` after the finishEndDoor input (line 186); `label for="stage10CW"` on the hmsFatalCW field; `user-scalable=no`; every colour has separate RGB + WW + CW numeric inputs with no preview; five mutually-exclusive "modes" are presented as independent toggles with JS un-checking the others; no tooltips/help; fixed 450 px layout; collapsible sections re-implement `<details>` with max-height hacks that break when inner content changes. |
| 42 | L | Setup page loads config via XHR and posts `application/x-www-form-urlencoded` with all ~100 fields; no partial update; no JSON API. |

## Build / repo hygiene

| # | Sev | Finding | Fix |
|---|-----|---------|-----|
| 43 | H | `platform = espressif32` unpinned. Official `platformio/espressif32` is frozen at Arduino core 2.0.17; on a machine with the maintained pioarduino platform (core 3.x) the build fails (`ledcSetup`, `tcpip_adapter_get_ip_info` in ESP32SSDP 1.2.1). | Pinned pioarduino 55.03.39 + core-3 LEDC API + ESP32SSDP 2.0.3. |
| 44 | L | 60+ historical `.bin` files (incl. ESP8266) committed in `firmware/`; `esphome.html` is an ESP Web Tools snippet, not ESPHome. README says `compress_html.py` but the script is `pre_build.py`. | Docs corrected; binaries left (release history). |
