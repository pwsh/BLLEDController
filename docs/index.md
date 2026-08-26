---
title: Home
nav_order: 1
description: "BLLED v3 — LED lighting that tells you what your Bambu Lab printer is doing."
---

# BLLED
{: .no_toc }

An ESP32 controller that drives a 12 V LED strip inside your **Bambu Lab** printer and colours it
according to what the printer is actually doing — printing, preheating, paused, finished, out of
filament, or in trouble. It listens to the printer's own local MQTT feed, so nothing is polled and
nothing goes through the cloud.
{: .fs-6 .fw-300 }

[Install it](getting-started.md){: .btn .btn-primary .mr-2 }
[Take the tour](using){: .btn }

![The BLLED dashboard, moments after a print finished: 100 % on the outer ring, 108 of 108 layers, the printer cooling down]({{ site.baseurl }}/screenshots/dashboard-1280.png)

---

## What it does

- **Follows the printer.** White while idle and printing, blue when paused, green when a print
  finishes, red for filament run-out and serious printer alerts, dark during the Micro Lidar
  stages so the sensor is not blinded. Every colour is yours to change.
- **Shows progress at a glance.** Optional visualisations blend the running colour toward the
  finish colour as the print advances, and turn the strip orange while the machine heats up.
- **Follows the chamber light** — and can drive it. Switch the printer's light off in the app and
  BLLED goes dark with it. Close the door twice quickly to toggle the strip by hand.
- **Runs on your phone.** A live dashboard shows what the LEDs are doing *and why*, and every
  setting carries a plain-language explanation.
- **Talks to everything else.** REST and WebSocket APIs, your own MQTT broker, and Home Assistant
  auto-discovery with 22 entities and no YAML.

## The first install needs a USB cable

{: .warning }
> **v3 cannot be installed over the air — not even from v2.**
> v3 uses a bigger application partition than v2 did, so the v2 update page will refuse the image.
> The first v3 install, on a brand-new board *or* on a board running v2.x, is a one-time USB flash.
> Every update after that is a normal over-the-air update from the System tab.

## Get going in three steps

1. **Flash over USB.** Connect the board, open a web flasher (or run `esptool`) and write
   `firmware/esp32dev/BLLC_V3.0.0.bin` at address `0x0`.
2. **Join `BLLED_AP`.** The strip turns pink and the controller opens its own network. Your phone
   offers a *Sign in to network* prompt; tap it, or open `http://192.168.4.1`.
3. **Fill in the setup page.** Your WiFi, then the printer's IP, serial number and LAN access code.
   Save — the controller restarts and starts following the printer.

[Full installation walkthrough](getting-started.md){: .btn .btn-outline }

## Where to go next

| If you want to… | Read |
|---|---|
| Install BLLED, or upgrade a v2 board | [Getting started](getting-started.md) |
| Understand a tab or a setting in the web interface | [Using BLLED](using) |
| Wire it into Home Assistant, MQTT, or your own scripts | [Guides](guides) |
| Work out why the strip is the wrong colour | [Troubleshooting](troubleshooting.md) |
| Look something up precisely | [Reference](reference) |

## Which printers

X1, X1C, X1E, P1P, P1S and A1. The printer must be in **LAN mode**, or have **Developer mode /
LAN-only MQTT** enabled on newer firmware, so that BLLED can subscribe to its report topic.

P1-series machines have no Micro Lidar and no door sensor; there is a switch on the
[Print Events](using/print-events.md) tab that tells BLLED to skip the features that depend on them.

## Hardware, in short

A plain ESP32 (`esp32dev`) switching five 12 V channels through MOSFETs, driving a common-anode
**RGB + warm/cold-white analog strip** — not an addressable WS2812 strip.
See [Hardware](reference/hardware.md) for the GPIO table and power notes. Ready-made boards and
wiring diagrams are at [dutchdevelop.com/blled](https://www.dutchdevelop.com/blled).
