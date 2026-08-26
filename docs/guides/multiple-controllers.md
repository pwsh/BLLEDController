---
title: Multiple controllers
parent: Guides
nav_order: 8
---

# Multiple controllers
{: .no_toc }

Two printers, two BLLEDs. Everything works, but a few things need distinct names.

---

## Give each one its own name

The **controller name** on the [Connection](../using/connection.md#controller-name) tab is the one
setting you must change on the second unit. It does three jobs at once:

- the **mDNS hostname** — `blled2` answers at `http://blled2.local`,
- the **DHCP name** your router shows in its client list,
- the default **MQTT topic prefix** — `blled/blled2`.

Letters, digits and hyphens only. Changing it needs a restart, and it re-publishes the Home
Assistant discovery messages under the new name.

Sensible pairs: `blled-x1c` and `blled-p1s`, or `blled-left` and `blled-right`. Naming them after
the printer beats naming them `blled1` and `blled2`, because the dashboard shows the name and you
will be reading it in a hurry.

{: .warning }
> **Two controllers with the same name will fight over `blled.local`.** mDNS has no tie-breaker
> that you would enjoy debugging. Rename the second one *before* you put it on the network, or use
> IP addresses until you have.

## Which one am I looking at?

Press **Identify** on the dashboard. The strip blinks white three times. There is also a
Home Assistant button entity that does the same thing, and:

```bash
curl -X POST http://192.168.1.51/api/led/identify
```

## Settings do not sync

{: .warning }
> Each controller keeps its **own** configuration. There is no shared state, no master, no sync.
> Changing brightness on one does nothing to the other.

To make two controllers behave alike, copy the configuration:

1. On the first: *System → Backup & restore → Download*.
2. Edit the JSON and change the per-device keys — `host`, and the printer's `printerIP`,
   `serialNumber` and `accessCode`. Leave `wifiSSID` and `wifiPass` alone if they share a network.
3. On the second: *System → Backup & restore → Upload*.

See [Backup & restore](backup-restore.md) for what is in the file.

{: .note }
> Restoring **replaces every setting**, including WiFi and the printer's identity. That is why you
> edit the file first. Restoring an unedited backup makes the second controller a clone that talks
> to the first controller's printer.

## Two printers, one MQTT broker

If both controllers publish to the same broker, give each an explicit **base topic** — or leave it
empty and let it default to `blled/<controller name>`, which is already unique once the names are.

```
blled/blled-x1c/status
blled/blled-p1s/status
```

Home Assistant discovery is keyed on the MAC address, so the two devices never collide there even
if you forget. But the entity *names* come from the controller name, so `blled` and `blled` would
give you two identically-named devices and a bad afternoon.

## Two controllers, one printer

Perfectly fine — for instance a strip above the plate and one behind the chamber. Both subscribe to
the same printer report topic and both react identically. Give them different names, and expect the
printer's chamber-light control to be fought over if you enable *Also control the printer's chamber
light* on both. Enable it on one.
