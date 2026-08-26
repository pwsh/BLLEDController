---
title: Troubleshooting
nav_order: 5
---

# Troubleshooting
{: .no_toc }

Symptom, cause, fix.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## Start here: two things that tell you almost everything

### The boot colours

The strip narrates its own start-up. Whatever colour it is stuck on is where start-up stopped.

| Colour | Reached | If it stays here |
|---|---|---|
| All channels on | Firmware running | The board is alive |
| **Red** | Mounting the file system | Flash trouble — re-flash over USB |
| **Orange** | Connecting to WiFi | It never joined. Wrong password, or a 5 GHz-only SSID |
| **Blue** | Web server up | It got past WiFi; the interface should answer |
| **Cyan** | Connecting to the printer's MQTT | Wrong IP, serial or access code, or the printer is not in LAN mode |
| Normal colours | Running | |
| **Pink** | Setup access point is open | No usable WiFi configuration |

### The reason line

The [Dashboard](using/dashboard.md) shows the **reason** the LED engine chose the current colour —
*"Chamber light off"*, *"Manual override"*, *"Printer offline"*, *"Toggled off via door"*. The
engine takes the first matching rule from a fixed ladder, and that line names the winner. When the
strip is not doing what you expect, read it first.

The live log (*System → Log console*, or `http://<ip>/webserial`) shows every one of those
decisions as it happens.

---

## Installing and updating

| Symptom | Cause | Fix |
|---|---|---|
| **The v2 update page rejects the v3 image** — "won't fit", "bad size", or it flashes and never boots | v3 uses a larger application partition than v2. The v2 OTA slot is about 1.31 MB; the v3 image is about 1.37 MB | There is no software workaround. **Flash v3 once over USB** at address `0x0`, and OTA works normally from then on. See [Upgrading from v2](getting-started.md#upgrading-from-v2) |
| The OTA upload on v3 is rejected | You uploaded the full `.bin` instead of the `.bin.ota` | The System tab wants **`BLLC_V3.0.0.bin.ota`**. The plain `.bin` is a complete flash image for USB at `0x0` |
| The board does not appear as a serial port | Charge-only USB cable, or a missing driver | Try a different cable first — it is the usual culprit. On Windows install the CP2102 or CH340 driver for your board |
| Flashing fails part-way | Baud rate too high, or the board is not entering the bootloader | Drop to `--baud 460800`; hold the **BOOT** button while it starts |
| After an OTA the device does not boot | An image built for a different board | Re-flash over USB |

## WiFi and reaching the interface

| Symptom | Cause | Fix |
|---|---|---|
| **The strip stays pink** | No WiFi credentials, wrong password, or the network was unreachable at boot | Join `BLLED_AP` and re-enter the WiFi details. If credentials *are* stored, the controller keeps retrying in the background and restarts by itself when the network returns |
| The strip stays **orange** | It never finished joining WiFi | Check the SSID and that it is **2.4 GHz** — the ESP32 has no 5 GHz radio. A combined-band SSID can fail to join |
| `BLLED_AP` appears but nothing responds | You are on the AP but the page did not open | Open **`http://192.168.4.1`** directly, with the `http://` prefix. See [Captive portal](guides/captive-portal.md) |
| No "Sign in to network" prompt | Probe interception failed, or the browser is HTTPS-only | Same fix: `http://192.168.4.1` in a normal browser tab |
| **`blled.local` does not resolve** | Some networks block mDNS; Android is unreliable with `.local` | Use the IP address from your router. A DHCP reservation makes it stable |
| Two controllers, one name | Both are claiming `blled.local` | Rename one on the [Connection](using/connection.md#controller-name) tab. See [Multiple controllers](guides/multiple-controllers.md) |
| The interface asks for a password you do not have | A web login is set | Send `{"resetAuth":true}` over USB serial at 115200 baud — it clears only the login. See [Serial provisioning](guides/serial-provisioning.md) |

## The printer connection

| Symptom | Cause | Fix |
|---|---|---|
| **The strip goes off after about 30 seconds and the dashboard says the printer is offline** | The printer's MQTT is not reachable. That 30 s is the *offline timeout* doing its job | Check the IP, serial number and access code on the [Connection](using/connection.md#printer) tab; check the printer is awake |
| Printer offline, and everything on the Connection tab looks right | **The printer is not in LAN mode.** Newer firmware requires **Developer mode / LAN-only MQTT** before third-party clients may subscribe | Enable *LAN Only Mode*, or *Developer mode*, on the printer screen. Without it the printer accepts the connection and sends nothing |
| It worked, then stopped | The access code was regenerated — a printer firmware update can do this — or the printer changed IP | Re-enter the access code. Leave *Follow the printer if its IP changes* on so an address change fixes itself |
| Connected, but nothing ever changes | P1 and A1 printers only send *changes*. v3 asks for a full state on connect, but a printer firmware update can need a nudge | Press **Refresh printer state** on the dashboard (it sends a `pushall`) |
| Connected, and the dashboard is missing layer or chamber data | Not every model reports every field | Nothing to fix. The chamber temperature is null on printers without a chamber sensor; layer counts depend on the file |

## Colours you did not ask for

| Symptom | Cause | Fix |
|---|---|---|
| **Red, and it will not clear** | An active printer alert (HMS) at Serious or Fatal | Open the alert list on the dashboard and read the code. Press **+ ignore** on a nuisance code, or turn off *React to printer alerts and errors*. See [Errors & Alerts](using/errors-alerts.md) |
| Occasional red or orange for no obvious reason | *Also react to Common advisories* got switched on — AMS humidity warnings and similar fire often | Turn it off, or give Common its own distinct colour |
| The strip is **dark** and the reason says *Chamber light off* | *Follow the printer's chamber light* is on and the printer's light is off | Switch the printer's light on, or turn the option off on [LED Behaviour](using/led-behaviour.md#follow-the-printers-chamber-light) |
| Dark, and the reason mentions idle | The inactivity timeout fired | Open the door or start a print. Raise or disable *Switch off when idle* |
| Dark, and the reason says *Toggled off via door* | The door double-close gesture switched it off | Close the door twice again, or turn the gesture off |
| **The light drops out mid-print** | Lidar stage colours — stage 10, first-layer inspection, also fires mid-print, and defaults to dark | Give stage 10 a colour on [Print Events](using/print-events.md#the-stage-colours) |
| Everything is dimmer than expected | Default brightness after a fresh install is **20 %** | Raise it on LED Behaviour or straight from the dashboard |
| Colours look wrong — white where you expected blue | Warm/cold white wired where RGB should be | Switch to **Test** mode; the default test colour makes miswiring obvious |
| The strip stopped reacting entirely | A manual override is active. The dashboard reason says *Manual override* | Press **Clear**, or `curl -X DELETE http://<ip>/api/led` |

## Features that never trigger

| Symptom | Cause | Fix |
|---|---|---|
| **The finish colour never ends, or the door toggle never works, and the dashboard says *Door: not reported*** | The printer's door switch is not being actuated. There is one switch, at the front door edge — **the top lid has no sensor** | Press the switch by hand and watch the chip. If that works, the door is not reaching it (mechanical). BLLED already falls back to ending the finish indication **by timer** when the door is never reported. Full detail: [Door sensor](guides/door-sensor.md) |
| Door features never work on a P1 | P1-series printers have no door sensor and no lidar | Turn on the [P1 switch](using/print-events.md#printer-type), and set the finish exit mode to **Timer** |
| **The preheat colour is never visible** | The print starts from an already-hot machine, so there is no heat-up to show | Nothing to fix — it appears on a cold start |
| The heat-up blend appears but is not obvious | The cold colour is too close to the running colour, or the print reaches target almost immediately | Pick a cold colour that contrasts sharply. Remember the blend into the running colour only happens over the **last ~15 %** before target, and is keyed on the **slowest** heater. See [LED effects](guides/led-effects.md#heat-up-blend-while-preheating) |
| The finish colour never appears | *Signal a finished print* is off, or the print did not reach `FINISH` (a cancelled print does not count) | Check the setting on [Print Events](using/print-events.md#finish-indication) |

## The web interface itself

| Symptom | Cause | Fix |
|---|---|---|
| **The page is blank on a phone, or behaves like an older version** | A cached `app.js` from a previous firmware build | Hard-reload the page (pull-to-refresh is not enough; use the browser menu's *Reload* / clear site data). Each build stamps the assets with its own **ETag**, so this normally resolves itself the first time you load after an update |
| The dashboard says **no data** | The controller is unreachable or rebooting | The page keeps retrying by itself. If it persists, check the boot colour |
| The indicator says **polling** instead of **live** | The WebSocket could not be opened — a proxy, or four clients are already connected | Harmless; the page polls instead. Close other open tabs |
| Changes do not stick | You did not press **Save** on that tab, or the tab needs a restart | Each tab saves separately. Connection settings raise a *restart required* banner |
| The password field shows `(unchanged)` | A password is stored | Leave it alone to keep it; type a new one to replace it |

## Getting more detail

1. **System → Log console** (or `http://<ip>/webserial`) with *Log state changes* on. Every state
   change with the reason for each LED decision.
2. **Verbose log** for more, but it is chatty — the printer reports every second.
3. **Log printer MQTT reports** to inspect the raw parsed reports. USB serial only.
4. `curl -s http://<ip>/api/status | jq` for the exact state the firmware is working from, and
   `curl -s http://<ip>/api/info | jq` for the build details worth quoting in a bug report.
