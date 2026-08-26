---
title: Home Assistant
parent: Guides
nav_order: 1
---

# Home Assistant
{: .no_toc }

BLLED can appear in Home Assistant as a device with a light, sensors and buttons, with no YAML at
all.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## What you need

- An MQTT broker (Mosquitto, the Home Assistant add-on is fine) reachable from BLLED on **plain
  TCP**. TLS brokers are not supported.
- The MQTT integration set up in Home Assistant and pointed at that broker.

BLLED's link to your broker is a **second, separate** connection. It has nothing to do with the TLS
connection it keeps to the printer.

## 1. Enable the broker

On the [Connection](../using/connection.md#external-mqtt--home-assistant) tab, open **External
MQTT / Home Assistant**:

1. Switch on **Publish to my own MQTT broker**.
2. Enter the broker's **host** and **port** (`1883`), and a **user name** and **password** if it
   needs them.
3. Leave **Base topic** empty to get `blled/<controller name>`, or set your own.
4. Leave **Home Assistant auto-discovery** on, with the prefix `homeassistant`.
5. Save. This block takes effect immediately — no restart.

The controller card on the dashboard will show the broker connection as connected. Home Assistant
should pick up a new device called after your controller within a few seconds.

Or do it over the API:

```bash
curl -X PUT -H 'Content-Type: application/json' -d '{
  "mqttExtEnabled": true,
  "mqttExtHost": "192.168.1.10", "mqttExtPort": 1883,
  "mqttExtUser": "blled", "mqttExtPass": "secret",
  "mqttExtBaseTopic": "blled/workshop",
  "mqttExtIntervalSec": 10,
  "haDiscovery": true, "haPrefix": "homeassistant"
}' http://192.168.1.50/api/config
```

## 2. What appears — 22 entities

| Kind | Name | Source |
|---|---|---|
| **Light** | *(the device name — this is the main entity)* | Turning it **on** applies a manual override colour; **off** returns the strip to automatic. Brightness, RGB and effects included. |
| **Select** | LED mode | Auto / Maintenance / Test / Rainbow / WiFi / Off |
| **Number** | Brightness | 0–100 % |
| Sensor | Stage | The printer's stage name |
| Sensor | G-code state | `RUNNING`, `PAUSE`, `FINISH`, `FAILED`… |
| Sensor | Progress | % |
| Sensor | Remaining | minutes |
| Sensor | Layer | |
| Sensor | Total layers | |
| Sensor | Nozzle temperature | °C |
| Sensor | Bed temperature | °C |
| Sensor | Chamber temperature | °C |
| Sensor | LED reason | The same reason string the dashboard shows |
| Sensor | Printer alert level | `None`, `Common`, `Serious`, `Fatal` |
| Sensor | WiFi signal | dBm, diagnostic |
| Binary sensor | Printer connected | connectivity, diagnostic |
| Binary sensor | Door | door |
| Binary sensor | Chamber light | light |
| Binary sensor | Finish indication | Is the finish colour showing right now |
| Button | Identify | Three white blinks |
| Button | Refresh printer state | Sends a `pushall`, diagnostic |
| Button | Restart controller | config |

{: .note }
> **The light entity is an override, not a switch for the strip.** Turning it **off** clears the
> override and hands the strip back to the automatic logic — it does not force the LEDs dark. To
> actually go dark, set the **LED mode** select to `Off`.

{: .note }
> **The Door binary sensor is only as good as the printer's switch.** If the printer never reports
> a door change, this entity never changes either. See [Door sensor](door-sensor.md).

## 3. Check what was published

```bash
mosquitto_sub -h 192.168.1.10 -v -t 'homeassistant/+/blled_+/config'
mosquitto_sub -h 192.168.1.10 -v -t 'blled/workshop/#'
```

Discovery messages are retained and re-published on every broker connect, and whenever the
controller name, the discovery prefix or the base topic changes.

Turning **Home Assistant auto-discovery** off publishes an empty retained payload to each of those
topics once, which is how Home Assistant is told to remove the entities again.

## 4. Example automation

Flash the strip red for ten minutes when the printer raises a fatal alert:

```yaml
automation:
  - alias: "Flash the printer LEDs red on a fatal printer alert"
    trigger:
      - platform: state
        entity_id: sensor.blled_hms_highest_severity
        to: "Fatal"
    action:
      - service: mqtt.publish
        data:
          topic: blled/workshop/set
          payload: '{"hex":"#ff0000","effect":"fastblink","durationSec":600}'
```

A gentler one — turn the workshop lamp on when a print finishes, and clear BLLED's finish colour
when you acknowledge it:

```yaml
automation:
  - alias: "Print finished"
    trigger:
      - platform: state
        entity_id: binary_sensor.blled_finish_indication
        to: "on"
    action:
      - service: light.turn_on
        target: { entity_id: light.workshop }
```

The entity ids above depend on your controller's name — check them in Home Assistant before you
copy them.

## Without discovery

You do not have to use discovery. Switch it off and drive the topics directly — everything BLLED
publishes and accepts is in [MQTT topics & payloads](mqtt.md).

## Implementation notes

If you are curious about *why* the discovery payloads look the way they do — the abbreviated keys,
the `~` shorthand, why the light needs its own flat state topic, the `| default(...)` guards — that
is written up in [Home Assistant discovery notes](../HA-DISCOVERY.md), and the entity table with
its exact `uniq_id` suffixes is in the [API reference](../API.md#8-home-assistant-discovery).
