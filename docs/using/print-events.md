---
title: Print Events
parent: Using BLLED
nav_order: 3
---

# Print Events
{: .no_toc }

What happens when a print finishes, when nothing happens for a while, and what the strip does
during the printer's calibration stages.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

<p>
<img src="{{ site.baseurl }}/screenshots/events-1280.png" alt="The Print Events tab" style="max-width:100%">
</p>

---

## Finish indication

### Signal a finished print

Switches the strip to the finish colour when a print completes, so you can see from across the room
that the plate is ready. Turn it off if you would rather the LEDs simply go back to the normal
running colour. Default: **on**.

### Finish colour

Default **green** `#00FF00`. Something clearly different from your running colour works best — the
whole point is to be noticeable from the doorway.

### Finish effect

| Effect | Character |
|---|---|
| **Solid** | Calm. |
| **Breathe** | Draws the eye without being annoying. |
| **Blink** | Hard to miss if the printer is out of sight. |
| **Fast blink** | Louder still. |

Blinking for a long finish timeout will irritate everyone in the room. Default: **Solid**.

### Leave the finish colour

How the finish colour ends.

- **Door open/close** — it stays lit until you actually come and collect the print. This is the
  default.
- **After a timer** — it clears after a fixed number of minutes.

{: .note }
> **P1-series printers have no door sensor, so use Timer there** — or turn on the
> [P1 switch](#printer-type) below, which forces it.
>
> And if your printer simply never reports a door change — the switch is not being actuated — the
> finish indication ends **by timer automatically**, so you cannot get stuck with a green strip
> forever. See [Door sensor](../guides/door-sensor.md).

### After (minutes)

How long the finish colour stays on before the strip returns to normal, when the exit mode is
Timer. Ignored in Door mode. Default: **10 minutes**.

## Idle & door

### Switch off when idle

Turns the LEDs off after the printer has been idle for a while, so the strip is not burning all
night. Any activity from the printer — a new print, a door event, a temperature change — brings the
light straight back. Default: **on**.

### After (minutes)

Minutes of printer inactivity before the LEDs switch off. The timer restarts on any change in the
printer's report and on every door open or close, so it only fires when the machine is genuinely
untouched. Default: **60 minutes**.

### Also control the printer's chamber light

Lets BLLED drive the printer's own chamber light over MQTT: **on** when a print starts or the door
opens, **off** when the inactivity timeout fires or the door gesture switches the LEDs off.

Handy when BLLED and the chamber light should behave as one lamp. Leave it off if you control the
chamber light from Home Assistant or the Bambu app. Default: **off**.

{: .note }
> This option only gates the *automatic* behaviour. The dashboard, the API and Home Assistant can
> always toggle the printer's light explicitly, whether or not this is on.

### Door double-close toggles the LEDs

Closing the door twice within two seconds toggles the LEDs on or off — a physical light switch that
needs no phone. Useful during a timelapse, or when a bright chamber annoys you at night.

P1 printers have no door sensor, so this never triggers there. Default: **on**.

When the gesture has switched the strip off, the dashboard's reason line reads *"Toggled off via
door"*. It stays off until the next door interaction or the next print.

### Go dark when the printer is offline for

How long the strip keeps its last colour after the printer's MQTT connection drops, before going
dark. A few seconds of grace avoids flicker on brief WiFi hiccups; longer values keep the light on
through a printer reboot. Default: **30 seconds**.

## Printer type

### P1-series printer (no lidar, no door sensor)

Tells BLLED you have a P1-series machine. P1 printers have no Micro Lidar and no door sensor, so
with this on:

- the lidar stage colours are never applied — the running colour simply stays on,
- the finish colour always ends by **timer**,
- the door double-close gesture is ignored.

Default: **off**. Turn it on for a P1P or P1S.

## Lidar stages

During bed levelling, nozzle cleaning, extrusion calibration, bed scanning and first-layer
inspection, the X1's **Micro Lidar** is taking measurements, and bright external light can disturb
it. That is why the default colours here are mostly dark: the strip gets out of the way.

### Use dedicated colours for lidar stages

When enabled, the stage colours below are used instead of the running colour. P1 printers have no
lidar — leave this off there. Default: **on**.

### The stage colours

| Stage | What the printer is doing | Default |
|---|---|---|
| **14** | Cleaning the nozzle tip | Off — so the lidar sees a dark chamber |
| **1** | Auto bed levelling | Dim blue `#000055`. This is the longest lidar stage and the one most affected by stray light |
| **8** | Calibrating extrusion / flow | Off |
| **9** | Scanning the bed surface | Off |
| **10** | Inspecting the first layer | Off |

{: .note }
> **Stage 10 also fires mid-print.** If you dislike the light dropping out partway through a job,
> give stage 10 a colour instead of leaving it dark.

<p>
<img src="{{ site.baseurl }}/screenshots/events-375.png" alt="The Print Events tab on a phone" width="260">
</p>
