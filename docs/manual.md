# BLLED user manual

BLLED drives a five-channel (R, G, B, warm white, cold white) LED strip inside a Bambu Lab
printer and colours it according to what the printer is doing. This manual covers the v3
web interface: one page with six sections, reachable at `http://<controller-name>.local/`
or at the controller's IP address.

Every setting in the interface carries a `?` button with the same explanation you will find
below — hover it, tap it, or focus it with the keyboard.

---

## First-time setup

A controller with no WiFi credentials starts its own access point called **BLLED-Setup**.
Join it with a phone or laptop; the captive portal opens the setup page (also at
`http://192.168.4.1/wifi`). You need four things:

1. **Your 2.4 GHz WiFi name and password.** The ESP32 has no 5 GHz radio. If your router
   presents both bands under one name, give the 2.4 GHz band its own SSID.
2. **The printer's IP address.** Give it a DHCP reservation so it does not move.
3. **The printer's serial number** — printer screen: *Settings → Device*.
4. **The LAN access code** — printer screen: *Settings → Network → LAN Only Mode*.

Save, and the controller restarts and joins your network. From then on the full interface
lives at `http://BLLED.local/` (or whatever you named it).

---

## Reading the dashboard

The dashboard is live — it updates over a WebSocket about once a second, and falls back to
polling if the socket drops. The indicator next to the BLLED logo reads *live*, *polling*,
*reconnecting* or *no data*.

**LED output** shows a preview strip painted with the colour the hardware is actually
emitting: the RGB channels composited with the warm- and cold-white channels, animated with
the same breathe/blink/rainbow curves the firmware uses. Under it, the exact channel values
and the **reason** the LED engine picked this colour — "Printing (stage 0)", "Chamber light
off", "Printer alert: serious", "Manual override" and so on. When the strip is not doing what you
expect, that line tells you which rule won.

The same card holds three live controls, which apply immediately and are not part of any
save button:

* **Mode** — the same six modes as the LED Behaviour tab, switched straight away.
* **Brightness** — takes effect as you drag it.
* **Manual override** — force any colour on the strip for a number of minutes (0 = until you
  press Clear). Useful for trying a colour before committing to it, and **Identify** blinks
  the strip white three times so you can tell two controllers apart.

**The printer card** shows the progress ring, G-code state and stage name, layer count, job
name, nozzle/bed/chamber temperatures with their target markers, the four fan speeds, and
chips for the door, chamber light, work light, SD card, print type, speed level, printer WiFi
and AMS tray with its humidity level — shown on Bambu's A (driest) to E (wettest) scale; the printer
actually sends an index 1–5 where 5 is dry, which is what the API returns.

**The controller card** shows WiFi signal, addresses, uptime, free heap, both MQTT
connections and the finish/inactivity countdowns.

**Printer alerts & errors** lists every alert (Bambu's HMS — Health Management System — messages) the printer is reporting, worst
first, with a severity badge and a link to the Bambu wiki page for that code. The
**+ ignore** button on a message adds its code to the ignore list and saves immediately —
that is the quick way to silence a nuisance code that keeps turning the strip red.

---

## Saving changes

Each of the four configuration tabs has its own **Save** button and sends only that tab's
settings. The button area shows how many unsaved changes the tab is holding, the tab itself
gets a dot, and **Revert** throws your edits away and reloads what the controller has.

Changing anything under **Connection** needs a restart: the page raises an orange banner with
a **Restart now** button. The controller never restarts by itself for a settings change.

Password fields show `(unchanged)` when a password is already stored. Type in one only if
you want to replace it; leave it alone and the stored password is kept.

---

## LED Behaviour

Everything about the strip itself: what mode it runs in, how bright and how fast, and the four base colours.

### LED mode

`ledMode` — `auto` | `maintenance` | `test` | `rainbow` | `wifi` | `off`

Picks what the strip does. Auto follows the printer (the whole point of BLLED); Maintenance and Test hold a fixed colour for working on the machine or checking your wiring; Rainbow is decorative and looks good in timelapses; WiFi strength colours the strip by signal so you can find a good spot for the controller; Off kills the output without unplugging anything.

### Brightness

`brightness` — 0–100 %

Global output level, applied last to every channel. 0 % switches all five channels fully off, 100 % is full PWM duty. Cheap 12 V strips get noticeably warm above ~70 % — if the LEDs flicker or the printer's chamber camera blooms, turn this down before changing colours.

### Fade time

`fadeMs` — 0–5000 ms, default 500

How long the strip takes to cross-fade when the colour changes, in milliseconds. 500 ms feels smooth and hides the constant micro-changes during a print; 0 makes every state change a hard cut, which is what you want if you are filming the LEDs or debugging the state machine.

### Effect speed

`effectSpeed` — 1–10, default 5

Speed of the breathe / blink / rainbow animations, 1 (slow) to 10 (fast). At 5 a breathe cycle takes about 3 seconds and a blink about 0.75 s. Only affects animated effects — it does nothing while everything is set to Solid.

### Follow the printer's chamber light

`followChamberLight` — default on

Mirrors the printer's own chamber light: when you switch the light off in the Bambu app or on the screen, BLLED goes dark too, and comes back when you switch it on. Leave this on if you want one light switch for the whole printer; turn it off if BLLED is your only chamber light and should stay on regardless.

### Running / idle colour

`runningRGB` / `runningWW` / `runningCW` — default warm+cold white 255

The everyday colour — shown while printing, preheating, homing and while the printer sits idle. Warm white plus cold white at full is the neutral default; add RGB only if you want a tint. This is the colour you will be looking at 95 % of the time, so set it first.

### Maintenance colour

`maintenanceRGB` / `maintenanceWW` / `maintenanceCW` — default warm+cold white 255

Colour used while LED mode is Maintenance. Defaults to both whites at maximum for the brightest, most neutral working light, which is what you want when you are clearing a clog or re-seating a hotend.

### Test colour

`testRGB` / `testWW` / `testCW` — default `#3f3cfb`

Colour used while LED mode is Test. A saturated colour (default #3F3CFB) makes it obvious which of the five channels are actually wired: if you see white instead of blue your warm/cold white lines are swapped in.

### Boot / WiFi colour

`wifiRGB` / `wifiWW` / `wifiCW` — default `#ffa500`

Shown during boot while the controller is joining WiFi, and on the setup access point. Default orange. If the strip stays this colour, BLLED never finished connecting — check the Connection tab.

### While printing

`printingVisual` — `solid` | `progress` | `breathe`

How the running colour behaves during an actual print. Solid never changes; Progress blends the running colour towards the finish colour as the print advances, so a glance at the strip tells you roughly how far along it is; Breathe pulses gently so you can see at a distance that the machine is still working.

### While preheating

`preheatVisual` — `solid` | `tempglow` ("Heat-up blend")

How the strip looks while the bed or hotend is coming up to temperature. Solid shows the plain running colour. Heat-up blend starts from the cold colour below (default orange) and blends into the running colour as the slowest heater approaches its target, so the strip visibly warms up with the printer and only turns white once nozzle and bed are both at temperature.

### Cold (heat-up) colour

`preheatRGB` / `preheatWW` / `preheatCW` — default `#ff6a00`, shown only with Heat-up blend

The cold end of the heat-up blend: what the strip shows when the heaters have just switched on. As they warm up it fades into the running colour. Default orange — pick something clearly different from the running colour so the change is obvious across the room.

## Print Events

What the strip does around a print: the finish signal, the idle timeout, the door gesture and the lidar stages.

### Signal a finished print

`finishIndication` — default on

Switches the strip to the finish colour when a print completes, so you can see from across the room that the plate is ready. Turn off if you would rather the LEDs just go back to the normal running colour.

### Finish colour

`finishRGB` / `finishWW` / `finishCW` — default `#00ff00`

Colour shown after a successful print. Default green. Something clearly different from your running colour works best — the whole point is to be noticeable from the doorway.

### Finish effect

`finishEffect` — `solid` | `breathe` | `blink` | `fastblink`

Animation for the finish colour. Solid is calm; Breathe draws the eye without being annoying; Blink is hard to miss if the printer is out of sight. Blinking for a long finish timeout will irritate everyone in the room.

### Leave the finish colour

`finishExitMode` — `door` | `timer`

How the finish colour ends. Door waits until you open or close the printer door — it stays lit until you actually come and collect the print. Timer clears it after a fixed number of minutes. P1 printers have no door sensor, so use Timer there.

### After (minutes)

`finishTimerMins` — 0–999, default 10

Minutes the finish colour stays on before the strip returns to normal, when exit mode is Timer. Ignored in Door mode.

### Switch off when idle

`inactivityEnabled` — default on

Turns the LEDs off after the printer has been idle for a while, so the strip is not burning all night. Any activity from the printer — a new print, a door event, a temperature change — brings the light straight back.

### After (minutes)

`inactivityMins` — 0–999, default 60

Minutes of printer inactivity before the LEDs switch off. The timer restarts on any printer report change and on every door open/close, so it only fires when the machine is genuinely untouched.

### Also control the printer's chamber light

`controlChamberLight` — default off

Lets BLLED drive the printer's own chamber light over MQTT: on when a print starts or the door opens, off when the inactivity timeout fires or the door gesture switches the LEDs off. Handy when BLLED and the chamber light should behave as one lamp; leave off if you control the chamber light from Home Assistant or the app.

### Door double-close toggles the LEDs

`doorToggleEnabled` — default on

Closing the door twice within two seconds toggles the LEDs on or off — a physical light switch that needs no phone. Useful during a timelapse or when a bright chamber annoys you at night. P1 printers have no door sensor, so this never triggers there.

### Go dark when the printer is offline for

`offlineTimeoutSec` — 0–999 s, default 30

How long the strip keeps its last colour after the printer's MQTT connection drops, before going dark. A few seconds of grace avoids flicker on brief WiFi hiccups; longer values keep the light on through a printer reboot.

### P1-series printer

`isP1Printer` — default off

Tells BLLED you have a P1-series printer. P1 machines have no Micro Lidar and no door sensor, so with this on the lidar stage colours are never applied (the running colour stays on), the finish colour always ends by timer, and the door double-close gesture is ignored.

### Use dedicated colours for lidar stages

`lidarStagesEnabled` — default on

During bed levelling, nozzle cleaning, extrusion calibration, bed scanning and first-layer inspection the X1's Micro Lidar takes measurements, and bright external light can disturb it. When enabled, the stage colours below are used instead of the running colour (default: off/black) so the strip gets out of the way. P1 printers have no lidar — leave this off.

### 14 — Cleaning nozzle tip

`stage14RGB` / `stage14WW` / `stage14CW` — default off

Colour while the printer is cleaning the nozzle tip (stage 14). Default off, so the lidar sees a dark chamber.

### 1 — Auto bed levelling

`stage1RGB` / `stage1WW` / `stage1CW` — default off

Colour during auto bed levelling (stage 1). Default off — this is the longest lidar stage and the one most affected by stray light.

### 8 — Calibrating extrusion

`stage8RGB` / `stage8WW` / `stage8CW` — default off

Colour while calibrating extrusion / flow (stage 8). Default off.

### 9 — Scanning bed surface

`stage9RGB` / `stage9WW` / `stage9CW` — default off

Colour while scanning the bed surface (stage 9). Default off.

### 10 — Inspecting first layer

`stage10RGB` / `stage10WW` / `stage10CW` — default off

Colour while inspecting the first layer (stage 10). Default off. This stage also fires mid-print, so if you dislike the light dropping out during a print, set a colour here.

## Errors & Alerts

Which faults change the colour, and to what.

### React to printer alerts and errors

`errorDetection` — default on

Master switch for every alert colour on this tab. When off, printer alerts (Bambu's HMS — Health Management System — messages), pauses and error stages are ignored and the strip just keeps showing the normal running colour. Turn it off if you find the red interruptions more annoying than useful.

### Error effect

`errorEffect` — `solid` | `breathe` | `blink` | `fastblink`

Animation used for all error colours (printer alerts, filament runout, front cover, temperature faults). Blink is the loudest and is genuinely useful for a fatal error you must notice.

### Pause effect

`pauseEffect` — `solid` | `breathe` | `blink` | `fastblink`

Animation used for the pause colours (user pause, G-code pause, first-layer error, nozzle clog). Breathe reads as 'waiting for you' without shouting.

### Paused by user or G-code

`pauseRGB` / `pauseWW` / `pauseCW` — default `#0000ff`

Shown when the print is paused by you or by an M400/G-code pause. Default blue — deliberately not red, because a pause is not a fault.

### First-layer inspection failed

`firstLayerRGB` / `firstLayerWW` / `firstLayerCW` — default `#0000ff`

Shown when the printer pauses because first-layer inspection failed (stage 34). Default blue. Give it its own colour if you want to tell 'come and look at the plate' apart from an ordinary pause.

### Nozzle clog

`nozzleClogRGB` / `nozzleClogWW` / `nozzleClogCW` — default `#0000ff`

Shown when the printer reports a nozzle clog pause (stage 35). Default blue.

### Printer alert — Fatal

`hmsFatalRGB` / `hmsFatalWW` / `hmsFatalCW` — default `#ff0000`

Shown when the most severe active printer alert (HMS message) is Fatal — the printer has stopped and needs you. Default red; pair it with a blinking error effect if the machine is out of earshot.

### Printer alert — Serious

`hmsSeriousRGB` / `hmsSeriousWW` / `hmsSeriousCW` — default `#ff0000`

Shown when the most severe active printer alert (HMS message) is Serious — something needs attention but the printer usually keeps going. Default red.

### Also react to Common advisories

`hmsCommonEnabled` — default off

Also react to Common (advisory) printer alerts, such as an AMS humidity warning. Off by default because these are frequent and mostly harmless; enable it with a distinct colour if you want to see advisories without confusing them with real faults.

### Printer alert — Common

`hmsCommonRGB` / `hmsCommonWW` / `hmsCommonCW` — default `#ffa500`

Colour for Common (advisory) printer alerts when they are enabled. Default orange — pick something that is clearly not your fatal/serious red.

### Filament runout

`filamentRunoutRGB` / `filamentRunoutWW` / `filamentRunoutCW` — default `#ff0000`

Shown when the printer pauses because filament ran out (stage 6 / the matching alert code). Default red.

### Front cover falling

`frontCoverRGB` / `frontCoverWW` / `frontCoverCW` — default `#ff0000`

Shown when the printer reports the front cover falling off or missing (stage 17). Default red.

### Nozzle temperature fault

`nozzleTempRGB` / `nozzleTempWW` / `nozzleTempCW` — default `#ff0000`

Shown on a nozzle temperature malfunction pause (stage 20). Default red — this is a genuine hardware fault, not a hint.

### Bed temperature fault

`bedTempRGB` / `bedTempWW` / `bedTempCW` — default `#ff0000`

Shown on a heat-bed temperature malfunction pause (stage 21). Default red.

### Ignore list

`hmsIgnoreList` — comma-separated, normalised to upper case with `_` separators

Printer alert codes that should never change the LED colour, one per line, in the form HMS_0300_1200_0002_0001 (HMS is Bambu's Health Management System; the code is shown on the printer screen, in Bambu Studio and on the dashboard here). Use it for the nuisance code your printer reports constantly (a known AMS quirk, a sensor you have already decided to live with) so it stops turning the strip red. Add codes straight from the Dashboard with the '+ ignore' button.

## Connection

Network credentials and the printer link. **Everything except the external-MQTT block needs a restart**, which the page offers as a banner once you save.

### Network (SSID)

`wifiSSID`

Name of the 2.4 GHz WiFi network the controller joins. The ESP32 has no 5 GHz radio, so if your router hides the 2.4 GHz band behind one combined SSID the join can fail — give the 2.4 GHz band its own name. Changing this needs a restart.

### Password

`wifiPass` — write-only; returned as `********` when set

Password for that network. It is stored in plain text in the config file on the device (and in backups), so treat a backup like a password. Leave blank to keep the existing one.

### Pin to access point (BSSID)

`BSSID`

Pins the controller to one specific access point by MAC address instead of letting it roam. Useful in a mesh where the ESP32 keeps clinging to a distant node; leave empty unless you have that problem.

### Re-scan for the strongest AP on next connect

`rescanWiFiNetwork` — transient, not stored

On the next connect, scan and join the strongest access point for this SSID instead of the pinned BSSID. A one-shot request — it is not stored.

### Controller name

`host` — default `BLLED`

Controller name. It is the mDNS hostname (http://<name>.local), the DHCP name your router shows, and the default external-MQTT topic prefix. Letters, digits and hyphens only; changing it needs a restart and re-publishes Home Assistant discovery.

### Printer IP address

`printerIP`

The Bambu printer's IP address on your LAN. Give the printer a DHCP reservation or a static lease — if it moves, BLLED loses MQTT until you update this (or until auto-update finds it again).

### Follow the printer if its IP changes

`printerAutoIp` — default on

Keep the printer IP up to date automatically: when network discovery sees your serial number at a new address, BLLED follows it. Leave on unless you have two printers and want to be certain BLLED never re-points itself.

### Serial number

`serialNumber`

The printer's serial number, printed on the machine and shown under Settings on the printer's screen. It is the MQTT topic and it also tells BLLED your model (X1C, P1S, A1…). It must match exactly or no reports will arrive.

### LAN access code

`accessCode` — returned in full so you can verify it

The eight-character LAN access code from the printer's network settings screen. It is the MQTT password. Regenerating it on the printer, or a firmware update, invalidates the old one — re-enter it here if the printer stops reporting.

### User name

`webUser`

Optional user name for HTTP Basic authentication on this web interface. Leave both user and password empty to keep the UI open on your LAN. Once set, it protects every route including the API, firmware upload and backup download.

### Password

`webPass` — write-only; returned as `********` when set

Password for the web interface login. Leave blank to keep the current one; clear the user name to disable authentication entirely. If you lock yourself out, a factory reset (or the USB serial provisioning) is the way back in.

### Publish to my own MQTT broker

`mqttExtEnabled` — default off

Publishes everything BLLED knows to your own MQTT broker, and accepts commands back. This is how you get the controller into Home Assistant, Node-RED or any other automation. It is a second, plain (non-TLS) connection and is completely separate from the printer's own MQTT link.

### Broker host

`mqttExtHost`

Hostname or IP of your MQTT broker, e.g. the machine running Mosquitto. Plain TCP only — TLS brokers are not supported.

### Port

`mqttExtPort` — default 1883

Broker port. 1883 is the standard unencrypted MQTT port.

### User name

`mqttExtUser`

Broker user name. Leave empty for an anonymous broker.

### Password

`mqttExtPass` — write-only

Broker password. Leave blank to keep the stored one.

### Base topic

`mqttExtBaseTopic` — default `blled/<host>`

Prefix for every topic BLLED publishes, e.g. blled/livingroom gives blled/livingroom/status and blled/livingroom/set. Leave empty to use blled/<controller name>. Change it if you run more than one BLLED on the same broker.

### Publish interval

`mqttExtIntervalSec` — default 10 s

How often the full status object is republished even when nothing changed, in seconds. Changes are always published within a second regardless; this is the heartbeat. Raise it if you are logging every message to disk.

### Home Assistant auto-discovery

`haDiscovery` — default on

Publishes Home Assistant MQTT discovery messages so the controller appears as a device with a light, sensors and buttons without any YAML. Switching it off removes those entities from Home Assistant again.

### Discovery prefix

`haPrefix` — default `homeassistant`

Discovery topic prefix Home Assistant listens on. 'homeassistant' unless you deliberately changed it in your Home Assistant MQTT settings.

## System

Firmware information, updates, backups and the debug switches.

### Firmware update

`POST /api/update` (multipart field `firmware`)

Uploads a new firmware image over WiFi. Use the .bin built for this board; the wrong image will not boot and needs a USB cable to recover. Settings survive the update.

### Backup & restore

`GET /api/config/backup`, `POST /api/config/restore`

Download the complete configuration as a JSON file, or upload one you saved earlier. Restoring replaces every setting (it is not a merge) and restarts the controller.

### Log state changes

`debugChanges` — default on

Logs only when something actually changes: stage transitions, LED decisions, door events, connection changes. This is the useful one to leave on; it is quiet when the printer is quiet.

### Verbose log

`debugVerbose` — default off

Logs everything to the serial console and the web serial log. Very chatty — the printer sends a report every second — so use it while chasing a problem and turn it back off afterwards.

### Log printer MQTT reports

`debugMqtt` — default off

Logs the filtered contents of every printer MQTT report. Goes to the USB serial port only (never the web log) because it is far too much traffic for a WebSocket. For diagnosing parsing problems.

---

## Other things on the System tab

**Restart controller** reboots the ESP32; the LEDs go dark for a few seconds and the page
reconnects on its own.

**Reconnect printer MQTT** forces a fresh connection to the printer — try it if the printer
card says disconnected but the printer is plainly reachable.

**Factory reset** deletes the configuration file, including the WiFi credentials, the access
code and the web login, and reboots into the **BLLED-Setup** access point. Take a backup
first if you want to come back.

**Web serial log** opens the live log console at `/webserial`. Turn on *Log state changes*
above to make it useful.

---

## Troubleshooting

| Symptom | Where to look |
|---|---|
| Strip stays orange | The controller never joined WiFi — that is the boot/WiFi colour. Check the SSID and that it is 2.4 GHz. |
| Strip is dark and the reason says "Chamber light off" | *Follow the printer's chamber light* is on and the printer's own light is off. |
| Strip is dark and the reason mentions idle | The inactivity timeout fired. Open the door or start a print, or raise *Switch off when idle*. |
| Strip goes dark mid-print | Lidar stage colours (default off) during bed levelling, nozzle cleaning or first-layer inspection. Give stage 10 a colour if that bothers you. |
| Strip is red and will not clear | Check the printer-alert list on the dashboard; use **+ ignore** for a code you have decided to live with, or turn off *React to printer alerts and errors*. |
| Printer card says disconnected | Wrong IP, wrong serial number, or a regenerated access code. All three are on the Connection tab. |
| Dashboard says "no data" | The controller is unreachable or rebooting; the page keeps retrying. |

---

## The API

Everything the interface does is a plain HTTP call, so the controller automates well:
`GET /api/status`, `PUT /api/config` (partial JSON), `POST /api/led`, `POST /api/action`,
and a WebSocket at `/ws`. The controller can also publish to your own MQTT broker with Home
Assistant discovery — see the **External MQTT / Home Assistant** block on the Connection tab.
`docs/API.md` has the full contract.
