---
title: Using BLLED
nav_order: 3
has_children: true
---

# Using BLLED

The web interface is one page with six sections — tabs across the top on a desktop, a bar along the
bottom on a phone. It lives at `http://blled.local/` (or whatever you named the controller) or at
the controller's IP address.

Every setting carries a **?** button with a plain-language explanation. Hover it, tap it, or focus
it with the keyboard. The same texts are collected in the [Settings reference](../manual.md).

<p>
<img src="../screenshots/dashboard-375.png" alt="The dashboard on a phone, with the section bar along the bottom" width="260">
</p>

## The six sections

| Tab | What lives there |
|---|---|
| [Dashboard](dashboard.md) | What the printer is doing, what the LEDs are doing and why, plus the live controls |
| [LED Behaviour](led-behaviour.md) | Mode, brightness, fade, effect speed, the base colours, and the visualisations |
| [Print Events](print-events.md) | Finish indication, idle timeout, chamber light, door gesture, P1 switch, lidar stages |
| [Errors & Alerts](errors-alerts.md) | Which faults change the colour, which colour, and which codes to ignore |
| [Connection](connection.md) | WiFi, printer, web login, external MQTT and Home Assistant |
| [System](system.md) | Firmware updates, backup and restore, factory reset, logs |

## Saving changes

The Dashboard's controls apply **immediately** — mode, brightness, override, identify. Nothing to
save.

The four configuration tabs each have their own **Save** button and send only that tab's settings.
The button area counts your unsaved changes, the tab itself grows a dot, and **Revert** throws the
edits away and reloads what the controller actually has.

Changing anything under **Connection** needs a restart. The page raises an orange banner with a
**Restart now** button; the controller never reboots by itself for a settings change.

Password fields show `(unchanged)` when a password is already stored. Type in one only to replace
it — leaving it alone keeps the stored password.
