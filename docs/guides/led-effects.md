---
title: LED effects & visualisations
parent: Guides
nav_order: 4
---

# LED effects & visualisations
{: .no_toc }

Why the strip is the colour it is, what each effect looks like, and what every state does out of
the box.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## The priority ladder

The LED engine re-evaluates whenever something changes, walks a fixed list of rules from the top,
and **the first rule that matches wins**. Nothing blends between rules; there is exactly one
winner, and its name is the **reason** string on the dashboard.

| # | Rule | Wins when |
|---|---|---|
| 1 | LED mode: Off | The mode is Off |
| 2 | Manual override | An override from the UI, API, WebSocket or MQTT is active |
| 3 | Identify | You pressed Identify |
| 4–7 | Maintenance / Test / WiFi strength / Rainbow | The mode is one of those |
| 8 | Boot / WiFi colour | Still starting up |
| 9 | Toggled off via door | The door double-close gesture switched the strip off |
| 10 | **Errors** | Filament runout, front cover, nozzle or bed temperature fault, or an alert at Fatal / Serious (or Common, if enabled) |
| 11 | **Pause** | User or G-code pause, first-layer failure, nozzle clog |
| 12 | Printer offline | The printer's MQTT has been down longer than the offline timeout |
| 13 | Chamber light off | *Follow the chamber light* is on and the printer's light is off |
| 14 | Lidar stages | The printer is in stage 14, 1, 8, 9 or 10 |
| 15 | Idle timeout | The inactivity timer expired |
| 16 | Finish indication | A print just finished and the indication is still running |
| 17 | Preheating | The heaters are coming up to temperature |
| 18 | Printing | An actual print is running |
| 19 | Idle | Everything else: idle, failed, preparing, slicing |

Two consequences worth internalising:

- **An override beats the printer.** If you set one and forget it, the strip stops reacting. The
  dashboard says *Manual override* and shows the countdown.
- **Errors beat printing.** A serious alert turns the strip red even mid-print, which is the point.

## The effects

| Effect | What it looks like | Where you can pick it |
|---|---|---|
| **Solid** | No animation. | Everywhere |
| **Breathe** | A gentle sine pulse, dipping to about a quarter brightness. About 3 s per cycle at effect speed 5. Reads as "waiting" or "still working". | Finish, error, pause, and *while printing* |
| **Blink** | 50 % duty, about 0.75 s per cycle at speed 5. | Finish, error, pause |
| **Fast blink** | About 0.3 s per cycle at speed 5. Loud. | Finish, error, pause |
| **Rainbow** | Hue rotation, RGB channels only, one cycle in 60 s to 6 s. Ignores every colour setting. | LED mode only |

**Effect speed** (1–10, default 5) scales all of them. **Fade time** (default 500 ms) is separate:
it is the cross-fade when the *target colour* changes, and it applies whether or not an effect is
running.

Global **brightness** is applied last, after the colour and after the effect — so 0 % really is off
on every channel.

## The two visualisations

These modify the *running* colour rather than replacing it.

### Progress blend (while printing)

The running colour is blended towards the **finish colour** in proportion to the print percentage.
At 0 % it is your running colour, at 100 % it has become the finish colour. The print visibly
ripens.

With the defaults — white running, green finish — a print goes from white through pale green to
green.

### Heat-up blend (while preheating)

Selected as *While preheating → Heat-up blend*. It is deliberately not a linear blend, because on a
strip with white channels a linear blend looks white almost immediately.

1. **Cold colour alone.** The white channels are off and the cold colour (default orange `#FF6A00`)
   carries the strip on its own, starting at around 40 % of its brightness.
2. **Brightening.** As the heaters warm, that brightness ramps up to full.
3. **Blending.** Only over the **last ~15 % before target** does it fade into the running colour.

So the strip is unmistakably **orange while heating** and **white once ready**, with the change
happening at the end where you can see it.

It is keyed on the **slowest heater**: whichever of the nozzle and the bed is furthest from its
target drives the whole thing. The strip only goes white when *both* are actually at temperature.

{: .note }
> If the heat-up blend never seems to appear, the usual cause is that the print starts from an
> already-hot machine — there is no heat-up to show. The other is a running colour that is close to
> the cold colour, so the transition is invisible. See
> [Troubleshooting](../troubleshooting.md).

## Every state and its default

| What the printer is doing | Default colour | Default effect | Setting |
|---|---|---|---|
| Idle, printing, homing, preheating | Warm + cold white, full | Solid | Running / idle colour |
| Preheating, with the heat-up blend on | Orange `#FF6A00` → running colour | Solid | Cold (heat-up) colour |
| Print finished | Green `#00FF00` | Solid | Finish colour |
| Paused by you or G-code | Blue `#0000FF` | Solid | Pause colour |
| First-layer inspection failed (stage 34) | Blue `#0000FF` | Solid | First-layer colour |
| Nozzle clog (stage 35) | Blue `#0000FF` | Solid | Nozzle clog colour |
| Printer alert — Fatal | Red `#FF0000` | Solid | Fatal colour |
| Printer alert — Serious | Red `#FF0000` | Solid | Serious colour |
| Printer alert — Common | Orange `#FFA500` | Solid | **Off by default** |
| Filament runout (stage 6) | Red `#FF0000` | Solid | Filament runout colour |
| Front cover falling (stage 17) | Red `#FF0000` | Solid | Front cover colour |
| Nozzle temperature fault (stage 20) | Red `#FF0000` | Solid | Nozzle temperature colour |
| Bed temperature fault (stage 21) | Red `#FF0000` | Solid | Bed temperature colour |
| Cleaning nozzle tip (stage 14) | Off | — | Lidar stage colour |
| Auto bed levelling (stage 1) | Dim blue `#000055` | — | Lidar stage colour |
| Calibrating extrusion (stage 8) | Off | — | Lidar stage colour |
| Scanning bed surface (stage 9) | Off | — | Lidar stage colour |
| Inspecting first layer (stage 10) | Off | — | Lidar stage colour |
| Printer's chamber light off | Off | — | Follow the chamber light |
| Printer offline for 30 s | Off | — | Offline timeout |
| Idle for 60 minutes | Off | — | Switch off when idle |
| Toggled off by the door gesture | Off | — | Door double-close |
| Maintenance mode | Warm + cold white, full | Solid | Maintenance colour |
| Test mode | `#3F3CFB` | Solid | Test colour |
| Booting / joining WiFi | Orange `#FFA500` | Solid | Boot / WiFi colour |
| Setup access point | Pink | Solid | Fixed |

Every one of those colours has separate RGB and warm/cold-white channels, so "white" above means
*both white channels at 255 with RGB at zero*.

## Boot colours

These are not configurable (apart from the orange WiFi one) and they are the fastest diagnostic you
have:

all channels on → **red** (file system) → **orange** (WiFi) → **blue** (web server) →
**cyan** (printer MQTT) → normal. **Pink** means the setup access point is open.

---

Where each rule lives in the firmware, and the exact maths of the fades and effects:
[Architecture §4](../ARCHITECTURE.md#4-led-engine-ledsh).
