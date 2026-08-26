---
title: Hardware
parent: Reference
nav_order: 2
---

# Hardware
{: .no_toc }

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## The board

A plain **ESP32** (`esp32dev` — an ESP32-D0WD class chip, 4 MB flash, two cores) switching five
12 V channels through MOSFETs.

Ready-made BLLED boards, enclosures and wiring diagrams are at
[dutchdevelop.com/blled](https://www.dutchdevelop.com/blled). Any ESP32 dev board wired the same
way runs the same firmware.

## The strip

**Common-anode RGB + CCT (warm white / cold white) analog strip.**

{: .warning }
> **Not addressable.** WS2812 / NeoPixel / SK6812 strips will not work — there is no data line and
> no per-pixel control anywhere in this design. Five PWM channels drive the whole strip as one
> light.

Five channels, all 5 kHz PWM at 8-bit resolution:

| Channel | GPIO |
|---|---|
| Red | 19 |
| Green | 18 |
| Blue | 21 |
| Warm white | 22 |
| Cold white | 23 |

The live pin map is also in `GET /api/info`, and on the System tab:

```bash
curl -s http://192.168.1.50/api/info | jq .pins
# { "r": 19, "g": 18, "b": 21, "ww": 22, "cw": 23 }
```

Because the warm and cold white channels are separate, "white" in BLLED is a mix you choose. The
default running colour is both whites at 255 with RGB at zero — a neutral white with no tint.

## Power

**12 V.** Size the supply for your strip's total draw and add headroom.

{: .warning }
> 24 V will misbehave. The board and the strip are both 12 V parts.

A few practical notes:

- The default brightness after a fresh install is **20 %**, deliberately, so an undersized supply
  does not sag on first boot before you have looked at anything.
- Cheap 12 V strips get noticeably warm above about **70 %** brightness. If the LEDs flicker or the
  printer's chamber camera blooms, turn the brightness down before you start changing colours.
- A USB cable powers the ESP32 but **not** the strip. During flashing you will see the board boot
  with no light at all until 12 V is present.
- Flicker at high brightness is usually the supply, not the firmware — PWM is at 5 kHz, well above
  anything you would see.

## Wiring check

Switch the LED mode to **Test**. The default test colour is a saturated blue-violet (`#3F3CFB`)
using only the RGB channels. If you see **white**, your warm/cold white lines are wired where the
RGB lines should be.

**Maintenance** mode is the opposite check: both whites at full, RGB at zero.

## Which printers

X1, X1C, X1E, P1P, P1S and A1. The hardware is identical for all of them; the differences are in
what the printer reports:

| | X1 series | P1 series / A1 |
|---|---|---|
| Micro Lidar stages | Yes | **No** |
| Door switch | Yes — one, at the front door edge | **No** |
| Chamber temperature | Yes | Usually not reported |

Turn on the [P1 switch](../using/print-events.md#printer-type) on a P1 or A1 and BLLED stops waiting
for the things that machine cannot report.

## Serial port

**115200 baud.** It carries the log, and it accepts provisioning and password-recovery commands —
see [Serial provisioning](../guides/serial-provisioning.md).

## Building the firmware yourself

Requires [PlatformIO Core](https://platformio.org/) and Python 3. The platform is pinned to
[pioarduino](https://github.com/pioarduino/platform-espressif32) (Arduino core 3.3.x); the official
`platformio/espressif32` platform is frozen at core 2.x and will not build v3.

```bash
git clone https://github.com/pwsh/BLLEDController -b v3-rework
cd BLLEDController
pio run                  # -> .firmware/BLLC_V3.0.0.bin (full image) and .bin.ota (OTA)
pio run -t upload        # flash over USB
pio device monitor       # 115200 baud
```

`pre_build.py` gzips everything in `src/www/` into `src/www/www.h` before each build — keep the
compressed total under about 60 kB. `merge_firmware.py` produces the merged image that gets flashed
at offset `0x0`.

For UI work without hardware, `python3 tools/mock_server.py` serves the whole API with simulated
printer data at `http://localhost:8080`. See the [UI developer notes](../UI.md).
