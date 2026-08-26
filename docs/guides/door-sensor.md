---
title: Door sensor
parent: Guides
nav_order: 7
---

# Door sensor
{: .no_toc }

Several BLLED features depend on the printer telling it that the door moved. This page explains
exactly what the printer reports, why it sometimes reports nothing, and what BLLED does about it.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## The printer has one door switch

There is **one** switch, at the **front door edge**. It is a small plunger or reed switch that the
door presses when it closes.

{: .warning }
> **The top lid has no sensor at all.** Lifting the glass lid does not register anywhere, and no
> amount of configuration will change that. If you want the strip to react when you open the
> printer, it has to be the front door.

P1-series printers have **no door sensor whatsoever**. Turn on the
[P1 switch](../using/print-events.md#printer-type) and BLLED will stop waiting for door events it
will never get.

## What depends on it

| Feature | What it needs |
|---|---|
| [Finish indication ending on "door"](../using/print-events.md#leave-the-finish-colour) | A door open or close after the print finishes |
| [Door double-close toggle](../using/print-events.md#door-double-close-toggles-the-leds) | Two closes within two seconds |
| Inactivity timer reset | Any door edge |
| Chamber light on door open | Any door edge, and *Also control the chamber light* |
| The **Door** chip on the dashboard and the Home Assistant `Door` binary sensor | The reported state |

## "Door: not reported"

Until the printer has reported a door change at least once, the dashboard chip reads
**Door: not reported** and stays muted. In the API, `printer.doorKnown` is `false`.

This is not an error — it is simply "we have not seen the door move yet". It becomes a problem when
it never changes, which happens when:

- the door does not actually press the switch when closed (misaligned door, worn magnet, a bent
  plunger, or something taped over it),
- the switch itself has failed,
- you only ever open the **lid**,
- you have a P1, which has no switch.

## BLLED handles it for you

{: .note }
> **If the printer never reports a door change, the finish indication ends by timer automatically.**

You cannot get stranded with a green strip forever waiting for a door event that will never come.
BLLED notices that the door has never been reported and falls back to the timer, using the
*After (minutes)* value from the Print Events tab, even when the exit mode says Door.

The double-close gesture has no such fallback — there is nothing to fall back to. It simply never
fires.

## Checking the switch

1. Open the [Dashboard](../using/dashboard.md) on a phone and stand at the printer.
2. Open the door. The **Door** chip should flip within a second.
3. Close it. It should flip back.
4. If nothing happens, find the switch at the door edge and **press it by hand**. If pressing it
   flips the chip, the switch works and the door is not reaching it — that is a mechanical
   adjustment. If pressing it does nothing, the switch or its wiring has failed.

You can watch the same thing from the command line:

```bash
watch -n1 "curl -s http://192.168.1.50/api/status | jq '.printer | {doorOpen, doorKnown}'"
```

`doorKnown: false` means it has never been reported. `doorKnown: true` with a `doorOpen` that never
changes means it is reported but stuck.

## If the switch is not usable

- Set [*Leave the finish colour*](../using/print-events.md#leave-the-finish-colour) to **Timer**
  explicitly, so the behaviour is intentional rather than a fallback.
- Turn off the **door double-close toggle**, so nobody wonders why it never works.
- If you want to keep the finish colour until you acknowledge it, use the Home Assistant light
  entity, or a `mosquitto_pub` on `<base>/cmd`, as your "I have collected it" button.
