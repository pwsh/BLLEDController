---
title: Errors & Alerts
parent: Using BLLED
nav_order: 4
---

# Errors & Alerts
{: .no_toc }

Which faults change the colour of the strip, which colour they use, and how to silence the ones you
have decided to live with.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

<p>
<img src="{{ site.baseurl }}/screenshots/alerts-1280.png" alt="The Errors and Alerts tab" style="max-width:100%">
</p>

---

## First: what is HMS?

**HMS** is Bambu's **Health Management System** — the printer's own diagnostics. When something is
wrong, or merely worth mentioning, the printer raises an HMS message with a code that looks like
`HMS_0300_1200_0002_0001`. You have seen these on the printer's screen and in Bambu Studio.

The BLLED interface calls them **printer alerts**, because that is what they are to you. The codes
are unchanged, and the dashboard links each one to its page on the Bambu wiki.

Every alert carries a severity:

| Severity | Meaning | Default reaction |
|---|---|---|
| **Fatal** | The printer has stopped and needs you. | Red |
| **Serious** | Something needs attention, but the printer usually keeps going. | Red |
| **Common** | An advisory — an AMS humidity warning, a dirty-lidar hint. | **Ignored by default** |

## Detection

### React to printer alerts and errors

The master switch for every colour on this tab. When it is off, printer alerts, pauses and error
stages are ignored, and the strip just keeps showing the normal running colour.

Turn it off if you find the red interruptions more annoying than useful. Default: **on**.

### Error effect

The animation used for all *error* colours — printer alerts, filament runout, front cover, and the
temperature faults. **Blink** is the loudest and is genuinely useful for a fatal error you must
notice. Default: **Solid**.

### Pause effect

The animation used for the *pause* colours — user pause, G-code pause, first-layer error, nozzle
clog. **Breathe** reads as "waiting for you" without shouting. Default: **Solid**.

## Pause colours

A pause is not a fault, which is why these default to blue rather than red.

| Setting | When it shows | Default |
|---|---|---|
| **Paused by user or G-code** | You pressed pause, or an `M400` / G-code pause ran | Blue `#0000FF` |
| **First-layer inspection failed** | The printer paused because first-layer inspection failed (stage 34). Give it its own colour if you want to tell "come and look at the plate" apart from an ordinary pause | Blue `#0000FF` |
| **Nozzle clog** | The printer reports a nozzle-clog pause (stage 35) | Blue `#0000FF` |

## Fault colours

| Setting | When it shows | Default |
|---|---|---|
| **Printer alert — Fatal** | The most severe active alert is Fatal. Pair it with a blinking error effect if the machine is out of earshot | Red `#FF0000` |
| **Printer alert — Serious** | The most severe active alert is Serious | Red `#FF0000` |
| **Also react to Common advisories** | Off by default, because these are frequent and mostly harmless. Enable it with a distinct colour if you want to see advisories without confusing them with real faults | **Off** |
| **Printer alert — Common** | Only when the switch above is on. Pick something clearly not your fatal/serious red | Orange `#FFA500` |
| **Filament runout** | The printer paused because filament ran out (stage 6, or the matching alert code) | Red `#FF0000` |
| **Front cover falling** | The printer reports the front cover falling off or missing (stage 17) | Red `#FF0000` |
| **Nozzle temperature fault** | A nozzle temperature malfunction pause (stage 20) — a genuine hardware fault, not a hint | Red `#FF0000` |
| **Bed temperature fault** | A heat-bed temperature malfunction pause (stage 21) | Red `#FF0000` |

Only the **most severe** active alert decides the colour. If a Fatal and a Serious alert are both
present, you get the fatal colour.

## Ignored alert codes

Printer alert codes that should never change the LED colour — one per line, in the form
`HMS_0300_1200_0002_0001`.

Use it for the nuisance code your printer reports constantly: a known AMS quirk, or a sensor you
have already decided to live with. Adding it here stops it turning the strip red, without switching
off error detection entirely.

The quickest way to add one is the **+ ignore** button next to the message on the
[Dashboard](dashboard.md#printer-alerts--errors) — it appends the code and saves immediately.

{: .note }
> Codes are normalised for you: case is ignored, hyphens become underscores, and you can separate
> entries with newlines, commas or semicolons.

## Red you did not ask for

If the strip is red and will not clear:

1. Open the alert list on the Dashboard and read the actual code — it may be a real fault.
2. If it is a nuisance, press **+ ignore** on it.
3. If several unrelated advisories are firing, check whether *Also react to Common advisories* got
   switched on.
4. As a last resort, turn off *React to printer alerts and errors*.

More symptoms and fixes: [Troubleshooting](../troubleshooting.md).

<p>
<img src="{{ site.baseurl }}/screenshots/alerts-375.png" alt="The Errors and Alerts tab on a phone" width="260">
</p>
