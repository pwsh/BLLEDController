---
title: MQTT topics & payloads
parent: Guides
nav_order: 3
---

# MQTT topics & payloads
{: .no_toc }

BLLED can publish everything it knows to **your own** broker, and take commands back. This is how
it reaches Home Assistant, Node-RED, or a shell script with `mosquitto_pub` in it.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## Two different MQTT connections

Do not confuse them:

| | Printer MQTT | External MQTT |
|---|---|---|
| Who | The Bambu printer | Your broker |
| Direction | BLLED subscribes to `device/<serial>/report` | BLLED publishes and subscribes |
| Transport | TLS on port 8883, user `bblp`, password = LAN access code | Plain TCP, usually port 1883 |
| Configurable | [Connection → Printer](../using/connection.md#printer) | [Connection → External MQTT](../using/connection.md#external-mqtt--home-assistant) |
| Default | Always on — it is the point of BLLED | **Off** |

This page is about the second one.

## The base topic

Everything hangs off `mqttExtBaseTopic`. Leave it empty and it defaults to
**`blled/<controller name>`** — a controller named `workshop` publishes under `blled/workshop`.

Set it explicitly if you run more than one BLLED against the same broker. See
[Multiple controllers](multiple-controllers.md).

## What BLLED publishes

| Topic | Retained | Payload |
|---|---|---|
| `<base>/availability` | yes | `online`, or `offline` through the MQTT Last Will |
| `<base>/status` | yes | The full `/api/status` object — every *publish interval*, **and** within a second of any change |
| `<base>/led` | yes | Just the `led` sub-object, on change (rate-limited to twice a second) |
| `<base>/light` | yes | Home Assistant's JSON-light state shape, on change |

```bash
mosquitto_sub -h 192.168.1.10 -v -t 'blled/workshop/#'
```

`<base>/status` is 1.5–2 kB. `<base>/light` looks like this:

```json
{"state":"ON","brightness":204,"color_mode":"rgb","color":{"r":255,"g":136,"b":0},"effect":"solid"}
```

where `state` is `ON` while a manual override is active, `brightness` is 0–255 (your 0–100 setting
scaled), and `color` is the override colour — or the engine's current output when `OFF`.

## What BLLED listens to

### `<base>/set` — the BLLED command shape

Any combination of these keys:

```json
{"hex":"#ff0000","ww":0,"cw":0,"effect":"breathe","durationSec":300,"brightness":50}
{"clear":true}
{"mode":"maintenance"}
{"brightness":80}
{"chamberLight":true}
{"identify":true}
```

The rules that matter:

- A **colour** — `hex`, or any of `r` / `g` / `b` / `ww` / `cw` — applies an **override**. A
  `brightness` sent alongside it is the override's temporary brightness.
- A **bare** `{"brightness":n}`, with no colour and no mode, **persists** the saved brightness
  instead.
- `mode` persists the LED mode.
- `durationSec: 0`, or no duration at all, means "until cleared".

```bash
mosquitto_pub -h 192.168.1.10 -t blled/workshop/set -m '{"hex":"#00ff00","durationSec":60}'
mosquitto_pub -h 192.168.1.10 -t blled/workshop/set -m '{"clear":true}'
```

### `<base>/cmd` — plain text

For simple automations, and what the Home Assistant buttons use.

| Payload | Effect |
|---|---|
| `ON` | White override until cleared |
| `OFF` | Clear the override |
| `IDENTIFY` | Three white blinks |
| `PUSHALL` | Ask the printer for a full state report |
| `RESTART` | Reboot the controller |

```bash
mosquitto_pub -h 192.168.1.10 -t blled/workshop/cmd -m ON
```

### `<base>/light/set` — Home Assistant's shape

Home Assistant publishes its own fixed JSON-light shape here; it cannot be templated, which is why
the light needs its own topic pair.

```json
{"state":"ON","brightness":200,"color":{"r":255,"g":100,"b":50},"effect":"solid"}
{"state":"OFF"}
```

{: .warning }
> `{"state":"OFF"}` **clears the override** — the LEDs go back to whatever the automatic logic
> wants. It does not force them dark. For that, send `{"mode":"off"}` to `<base>/set`.

## Connection behaviour

Plain TCP only; TLS brokers are not supported. The client id is `BLLED-<last 3 bytes of the MAC>`,
and reconnection backs off from 5 s to 60 s.

Enabling, disabling or changing the broker settings takes effect **immediately** — no restart. The
`mqtt.external` block in `/api/status` reports the live connection state, and so does the
controller card on the dashboard.

## Home Assistant discovery

With discovery on, BLLED also publishes retained configuration messages under
`<haPrefix>/<component>/blled_<mac6>_<key>/config` on every broker connect. That is covered in
[Home Assistant](home-assistant.md); the exact payload design is in
[Home Assistant discovery notes](../HA-DISCOVERY.md).

---

Full details, including every published field: [API reference §7](../API.md#7-external-mqtt-broker).
