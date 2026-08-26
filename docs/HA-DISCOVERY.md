# Home Assistant MQTT Discovery — Implementation Reference for ESP32 (`blled`)

**Sources:** [MQTT integration / Discovery](https://www.home-assistant.io/integrations/mqtt/) · [Light (MQTT)](https://www.home-assistant.io/integrations/light.mqtt/) · [Select (MQTT)](https://www.home-assistant.io/integrations/select.mqtt/) · [Number (MQTT)](https://www.home-assistant.io/integrations/number.mqtt/) · [Sensor (MQTT)](https://www.home-assistant.io/integrations/sensor.mqtt/) · [Binary sensor (MQTT)](https://www.home-assistant.io/integrations/binary_sensor.mqtt/) · [Button (MQTT)](https://www.home-assistant.io/integrations/button.mqtt/)

## 1. Discovery topic format: classic vs. device-based — **use classic, per-entity discovery**

- Classic: `homeassistant/<component>/[<node_id>/]<object_id>/config` — one retained message per entity. `<object_id>` must be `[a-zA-Z0-9_-]`.
- Device-based (new in 2024.x): `homeassistant/device/<id>/config` — **one** retained message holding `dev`, `o`, and a `cmps` map of every entity's config keyed by an id, each with a `p` (platform) key. This is what HA's own docs example shows:

```json
{
  "dev": {"ids": "ea334450945afc", "name": "Kitchen"},
  "o": {"name": "bla2mqtt", "sw": "2.1"},
  "cmps": {
    "some_unique_component_id1": {"p": "sensor", "unique_id": "temp01ae_t"}
  },
  "state_topic": "sensorBedroom/state",
  "qos": 2
}
```

**Recommendation for this device: classic per-entity discovery.** Reasoning:
- Device-based discovery shares `dev`/`o` once, but still needs a full config block per entity inside `cmps`. For ~20 entities the *combined single payload* runs several KB — far over your 1024-byte PubSubClient buffer. You'd have to transiently bump `MQTT_MAX_PACKET_SIZE` to 4–8 KB just for one boot-time publish, then shrink it back, which is fragile and wastes RAM on a constrained MCU.
- With classic discovery, each config (see §3 below) is 150–450 bytes even *with* the full `dev` block repeated in every message (measured examples below) — comfortably inside your normal 1024-byte buffer, no special-casing needed.
- Retained per-entity messages are also easier to individually update/clear (§6) without re-publishing the whole device description.
- Trade-off you accept: ~20 retained publishes at boot instead of 1, and the `dev` block bytes are duplicated 20×. This is the right trade for a small buffer; it would be the wrong trade if you had hundreds of entities or a large heap.

If you ever raise the buffer permanently (e.g. move to a board with more RAM), device-based discovery becomes attractive again since it also lets you delete the *whole device* with one empty retained publish.

## 2. Abbreviation keys used below

| Full key | Abbrev | Full key | Abbrev |
|---|---|---|---|
| `unique_id` | `uniq_id` | `state_topic` | `stat_t` |
| `command_topic` | `cmd_t` | `value_template` | `val_tpl` |
| `command_template` | `cmd_tpl` | `device` | `dev` |
| `availability_topic` | `avty_t` | `payload_available`/`payload_not_available` | `pl_avail`/`pl_not_avail` |
| `device_class` | `dev_cla` | `unit_of_measurement` | `unit_of_meas` |
| `state_class` | `stat_cla` | `icon` | `ic` |
| `entity_category` | `ent_cat` | `origin` | `o` |
| `payload_press` | `pl_prs` | `payload_on`/`payload_off` | `pl_on`/`pl_off` |
| `supported_color_modes` | `sup_clrm` | `effect_list` | `fx_list` |
| `suggested_display_precision` | `sug_dsp_prc` | `~` (topic shorthand) | `~` |

`dev` sub-keys: `ids` (identifiers), `name`, `mf` (manufacturer), `mdl` (model), `sw` (sw_version), `hw` (hw_version), `cu` (configuration_url), `via_device`.
`o` sub-keys: `name` (required), `sw` (sw_version), `url` (support_url).

**`~` shorthand**: define `"~": "blled/host1"` once in the payload; any topic field whose value is `"~/something"` gets `~` expanded to that prefix. This is what keeps the examples below small — it replaces repeating `blled/host1/` in `stat_t`, `cmd_t`, `avty_t`.

## 3. Minimal entity JSON (measured byte sizes with `~` shorthand, all comfortably under 500 bytes)

### 3a. Light — **cannot be state-templated from nested JSON; needs its own flat state topic**

Confirmed from the docs: the JSON light schema (`"schema": "json"`) does **not** support `value_template`/`state_value_template` to pull state out of a larger/nested payload — HA parses the state-topic payload directly against its own fixed schema, and `command_template` is likewise **not available** in this schema (fields are always sent as HA's own JSON shape). So:

- **Add a dedicated state topic** `blled/<host>/light` that your firmware publishes (derived from `led{}` at publish time, not via HA templating) in exactly this shape:
```json
{"state":"ON","brightness":180,"color_mode":"rgb","color":{"r":255,"g":120,"b":0},"effect":"rainbow"}
```
  - `state`: `"ON"`/`"OFF"`. `brightness`: 0–255 (not 0–100 — scale it). `color_mode` must be one of `supported_color_modes`. `color.r/g/b`: 0–255. `effect`: must match one of `effect_list` or omit the key.
- **Add a dedicated command topic** `blled/<host>/light/set` (not the shared `/set`) — HA will publish the *same* fixed shape to it, unprompted and untemplated, e.g. `{"state":"ON","brightness":200,"color":{"r":255,"g":100,"b":50}}` (only changed fields included). Your firmware must parse this shape directly on that topic; it cannot be rewritten into your own `{"mode":...}` shape by a template.

Discovery config (`homeassistant/light/blled_host1_light/config`, 431 bytes):
```json
{"~":"blled/host1","name":null,"uniq_id":"blled_host1_light","stat_t":"~/light","cmd_t":"~/light/set","schema":"json","brightness":true,"sup_clrm":["rgb"],"effect":true,"fx_list":["solid","rainbow","breathe","chase"],"avty_t":"~/avail","dev":{"ids":"blled-host1","name":"BLLed host1","mf":"DIY","mdl":"ESP32-BLLed","sw":"1.0.0","cu":"http://host1.local/"},"o":{"name":"blled-fw","sw":"1.0.0","url":"https://github.com/you/blled"}}
```
`"name": null` + `has_entity_name: true` (implicit for discovered entities) makes this the device's "main" light entity, named after the device itself with no suffix.

### 3b. Select — LED mode, wraps into your existing JSON `/set` command (276 bytes)
```json
{"~":"blled/host1","name":"Mode","uniq_id":"blled_host1_mode","stat_t":"~/status","val_tpl":"{{ value_json.led.mode }}","cmd_t":"~/set","cmd_tpl":"{\"mode\":\"{{ value }}\"}","options":["off","manual","auto","printer","effect"],"avty_t":"~/avail","dev":{"ids":"blled-host1"}}
```
Only one entity needs the full `dev` block spelled out (name/mf/mdl/sw/cu) — HA merges device info across all discovery messages sharing the same `ids`, so the rest can send `"dev":{"ids":"blled-host1"}` only, saving bytes (as done here and below).

### 3c. Number — brightness 0–100, wraps into `/set` (289 bytes)
```json
{"~":"blled/host1","name":"Brightness","uniq_id":"blled_host1_brightness","stat_t":"~/status","val_tpl":"{{ value_json.led.brightness }}","cmd_t":"~/set","cmd_tpl":"{\"brightness\":{{ value }}}","min":0,"max":100,"step":1,"unit_of_meas":"%","avty_t":"~/avail","dev":{"ids":"blled-host1"}}
```

### 3d. Sensor — nozzle temperature, nested `val_tpl` (297 bytes)
```json
{"~":"blled/host1","name":"Nozzle Temperature","uniq_id":"blled_host1_nozzletemp","stat_t":"~/status","val_tpl":"{{ value_json.printer.nozzleTemp | default(0) }}","dev_cla":"temperature","unit_of_meas":"°C","stat_cla":"measurement","sug_dsp_prc":1,"avty_t":"~/avail","dev":{"ids":"blled-host1"}}
```
**Missing/null handling**: per the sensor docs, "if the template throws an error, the current state will be used instead" — so a hard Jinja error (e.g. `printer` key entirely absent) is silently swallowed and the sensor freezes at its last value. A JSON `null` for `nozzleTemp` renders as the *string* `"None"`, which is not a valid float and HA will log a warning and mark the sensor `unknown`. Use `| default(0)` (or `| default(none, true)` to render `unavailable`/`unknown` instead of `0`) around every nested accessor. Do **not** rely on availability alone for per-key nulls — availability only flips the whole entity block offline/online, not individual keys.

### 3e. Binary sensors — door / connectivity / light (each ~150–215 bytes)
```json
{"~":"blled/host1","name":"Door","uniq_id":"blled_host1_door","stat_t":"~/status","val_tpl":"{{ 'ON' if value_json.printer.doorOpen else 'OFF' }}","dev_cla":"door","avty_t":"~/avail","dev":{"ids":"blled-host1"}}
```
Same pattern for `dev_cla: "connectivity"` on `value_json.printer.connected`, and a plain (no device_class, or `dev_cla: "light"`) one on `value_json.printer.chamberLight`.

### 3f. Button — plain-text command topic (151 bytes)
```json
{"~":"blled/host1","name":"Test Effect","uniq_id":"blled_host1_testfx","cmd_t":"~/cmd","pl_prs":"TEST","avty_t":"~/avail","dev":{"ids":"blled-host1"}}
```
`cmd_t` here is your plain-text `blled/<host>/cmd` topic — button entities don't require JSON, `pl_prs` is published verbatim.

## 4. `dev` and `o` blocks — required/recommended fields

- `dev.ids` is the only hard requirement (a stable string or array, e.g. `"blled-host1"`); without it entities won't group under one device.
- `mf`, `mdl`, `sw`, `cu` are optional but strongly recommended — `cu` (configuration_url) is what puts a gear-icon link to `http://<host>.local/` on the device page.
- `o` (origin) — the docs mark this as effectively mandatory in current HA (soft-required: your integration/firmware identifies itself; `name` is required, `sw`/`url` optional). Include it at least on one message; HA merges origin info like it merges device info, so you don't strictly need to repeat the full block on every entity, but repeating `o.name` costs almost nothing and is safest across HA versions.
- `via_device` is not relevant here (single physical device, no bridge topology).

## 5. Availability + LWT

Use one shared, dedicated topic, not the status topic:
- `avty_t`: `blled/<host>/avail` (or via `~/avail` as above)
- Firmware sets this as the PubSubClient **Last Will**: `client.connect(clientId, user, pass, "blled/<host>/avail", 1, true, "offline")`, then immediately after a successful connect, publishes retained `"online"` to the same topic.
- Defaults match HA's built-in defaults (`pl_avail: "online"`, `pl_not_avail: "offline"`) so you can omit both keys entirely — saves bytes in every one of the 20 configs.
- Do **not** point `avty_t` at `blled/<host>/status`: that topic's payload is JSON, not the plain `online`/`offline` string HA expects for availability by default (you'd need `avty_tpl` on every entity, which only adds bytes and coupling).

## 6. Removing entities / `unique_id` changes

- To delete one entity: publish an **empty, retained** payload to its exact discovery topic (`homeassistant/<component>/<object_id>/config` → empty string, retained). HA removes the entity and its registry entry.
- To delete a whole device (device-based discovery only): empty retained payload to `homeassistant/device/<id>/config` removes all its components at once.
- **`unique_id` changes are dangerous**: if you republish the *same discovery topic* with a *different* `uniq_id`, HA treats it as a brand-new entity — the old entity registry entry is orphaned (stays around, becomes `unavailable`, doesn't auto-delete) since nothing tells HA to remove it. Best practice: never change `unique_id` for a logical entity once shipped; if you must, publish an empty payload to the *old* discovery topic first (or manually delete the orphaned entity from Settings → Devices & Services → Entities in HA).
- Also note (relevant given your current HA version): recent HA releases deprecated using `object_id` in the discovery payload to control the resulting `entity_id` — that mechanism is being removed around HA Core 2026.4 in favor of an explicit `"default_entity_id"` key. Since you're not setting `object_id` in the payload (only in the discovery *topic*, which is different and unaffected), this shouldn't touch you, but avoid adding `object_id` as a payload key for new entities — use `default_entity_id` if you ever need to pin an entity_id.

## 7. Gotchas checklist

- **Template errors on missing keys**: a Jinja error (undefined key) leaves the sensor's *previous* state untouched rather than erroring visibly — good for resilience, bad for debugging (failures are silent unless you check HA logs at debug level for the `mqtt` component). Always guard nested `value_json.x.y` access with `is defined`/`| default(...)`.
- **`value_json` on `null`**: renders as the string `"None"` if not filtered — breaks numeric sensors (HA logs "not a number" style warnings and marks the entity `unknown`).
- **No documented hard payload-size cap** from Home Assistant's own MQTT discovery docs (no "4096 bytes" limit is stated anywhere in the current MQTT integration page). The real constraints are (a) your PubSubClient buffer (default 256 B, yours raised to ~1024 B) on the *publish* side, and (b) broker-configured max packet size. Community reports describe practical trouble on some ESP+MQTT stacks above roughly 4000 characters, but that's not an HA-imposed limit — it's TCP fragmentation / MCU heap pressure. Keep every payload well under 1 KB regardless, as done above.
- **`has_entity_name` / device-prefixed naming**: discovered entities default to `has_entity_name: true`. Setting an entity's `"name"` to `null` (as used for the light in 3a) makes it the device's "main" entity — it takes the device's own name with no suffix in the UI. All other entities get `"<Device Name> <name>"` automatically; you do not need to prefix names yourself (e.g. use `"name": "Nozzle Temperature"`, not `"BLLed host1 Nozzle Temperature"`).
- **JSON light schema is rigid**: no `value_template`/`state_value_template`/`command_template` support — this is the one component where you cannot template around a nested/custom payload shape; you must speak HA's exact flat JSON on both directions (§3a). This is the main design fork for your firmware: either add the two extra topics (`.../light`, `.../light/set`) as recommended, or drop the `light` domain entirely and expose RGB/mode/brightness as separate `select`/`number` entities (loses the HA "light" card UI with color wheel, but keeps everything on your existing generic `/set` topic).
