---
title: Layer progress
parent: Guides
nav_order: 5
---

# Layer progress
{: .no_toc }

The dashboard shows three progress indicators. Two of them come straight from the printer. One is
BLLED's own estimate, and it is worth knowing where it comes from.

---

## What you are looking at

<p>
<img src="{{ site.baseurl }}/screenshots/dashboard-1280.png" alt="The printer card at the end of a print: the outer ring at 100 %, the layer gauge at 108 of 108" style="max-width:100%">
</p>

<p class="fs-3"><em>Captured at the end of a print, so both rings are full. During a print the outer ring tracks the percentage and the inner one cycles once per layer.</em></p>

| Indicator | Where it comes from |
|---|---|
| **Outer ring** (thick) | The print percentage, exactly as the printer reports it |
| **Inner ring** (thin) | **BLLED's estimate** of progress *within the current layer* |
| **Vertical gauge** | The layer number as a fraction of the total, both reported by the printer |

The percentage and the time remaining sit in the middle of the rings; a `print % / this layer
(est.)` legend sits underneath, and the gauge's caption reads `layer / total layers`.

## How the inner ring is estimated

**The printer does not report progress within a layer.** It reports which layer it is on, and it
reports that only when the layer changes.

So BLLED times it:

1. Every time the reported layer number increases, it records how long that layer took.
2. It keeps a running average of recent layer times.
3. The inner ring shows *time since the current layer began* divided by *that average*.

That is the whole mechanism. It is a timed extrapolation from the layer transitions, not data from
the printer.

## Where the estimate is wrong

- **The first layer has no ring at all.** There is no previous layer to time, so the ring is empty
  until the first transition happens. On a first layer that takes several minutes, this looks like
  something is broken. It is not.
- **A layer far longer than average overshoots.** The ring reaches 100 % and sits there until the
  layer actually ends. A big solid infill layer after a run of sparse ones does this reliably.
- **A layer far shorter than average barely moves** before jumping to the next.
- **Pauses inflate it.** Time spent paused, changing filament or waiting for you counts as time
  spent on the layer.
- **Variable-layer-height prints wander**, because the average is over layers of different sizes.

Treat the inner ring as "roughly where in this layer we are", not as a measurement.

## When there is no layer data at all

If the printer is idle, or has not sent a layer count, the inner ring and the gauge both sit at
zero and the caption reads **no layer data**. Some printers and some file types simply never report
a total layer count; the outer print ring still works.

## In the API

The relevant fields of `GET /api/status`:

```json
"layer": 44,
"totalLayers": 108,
"layerProgress": 62,
"layerAvgSec": 41
```

- `layer` / `totalLayers` — from the printer.
- `layerProgress` — the estimate, 0–100, or **`-1`** when it is unavailable (first layer, idle, no
  layer data).
- `layerAvgSec` — the current average layer time in seconds, which is what the estimate divides by.

`-1` is not zero. If you build your own display, check for it.

---

The rendering details — which SVG rings, which CSS custom properties, why the animation costs the
compositor and not the main thread — are in the
[UI developer notes](../UI.md#layer-progress-on-the-dashboard).
