---
title: Connection
parent: Using BLLED
nav_order: 5
---

# Connection
{: .no_toc }

WiFi, the printer, the web login, and the optional link to your own MQTT broker.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

<p>
<img src="{{ site.baseurl }}/screenshots/connection-1280.png" alt="The Connection tab" style="max-width:100%">
</p>

{: .warning }
> Everything on this tab except the MQTT block needs a **restart** to take effect. Saving raises an
> orange banner with a **Restart now** button. The controller never reboots by itself for a
> settings change.

---

## WiFi

### Network (SSID)

The name of the **2.4 GHz** network the controller joins. The ESP32 has no 5 GHz radio, so if your
router hides the 2.4 GHz band behind one combined SSID the join can fail — give the 2.4 GHz band
its own name.

The field offers a **scan**: press it, wait a couple of seconds, and pick from the list of networks
the controller can actually see (which is more useful than the list your phone sees).

### Password

Stored in plain text in the configuration file on the device — and therefore in backups too, so
treat a backup like a password. Leave the field blank to keep the existing password; it shows
`(unchanged)` when one is stored.

### Pin to access point (BSSID)

Pins the controller to one specific access point by MAC address instead of letting it roam. Useful
in a mesh network where the ESP32 keeps clinging to a distant node. Leave it empty unless you have
that problem.

### Re-scan for the strongest AP on next connect

On the next connect, scan and join the strongest access point for this SSID instead of the pinned
BSSID. It is a one-shot request — it is not stored.

### Controller name

The controller's name, and it is used in three places at once:

- the **mDNS hostname**, so `blled` answers at `http://blled.local`,
- the **DHCP name** your router shows in its client list,
- the default **external-MQTT topic prefix**.

Letters, digits and hyphens only. Changing it needs a restart and re-publishes the Home Assistant
discovery messages.

Running two controllers? See [Multiple controllers](../guides/multiple-controllers.md).

## Printer

### Printer IP address

The Bambu printer's address on your LAN. Give the printer a DHCP reservation or a static lease — if
it moves, BLLED loses MQTT until you update this (or until the setting below finds it again).

The **Discover** button runs an SSDP search for Bambu printers on the network and offers what it
finds.

### Follow the printer if its IP changes

Keeps the printer IP up to date automatically: when discovery sees **your serial number** at a new
address, BLLED follows it. Leave it on unless you have two printers and want to be certain BLLED
never re-points itself. Default: **on**.

### Serial number

Printed on the machine and shown under *Settings → Device* on the printer's screen. It is the MQTT
topic, and it also tells BLLED your model (X1C, P1S, A1…). It must match **exactly** or no reports
will arrive.

### LAN access code

The eight-character code from the printer's network settings screen. It is the MQTT password.

Regenerating it on the printer — or, sometimes, a printer firmware update — invalidates the old
one. Re-enter it here if the printer suddenly stops reporting.

## Web interface

### User name and password

Optional HTTP Basic authentication for the web interface. Leave **both** empty to keep the UI open
on your LAN.

Once set, it protects **every** route: the pages, the API, the WebSocket, the firmware upload and
the backup download. In setup-AP mode everything is open regardless, so that the captive portal can
work.

Leave the password blank to keep the current one; clear the **user name** to disable authentication
entirely.

{: .warning }
> Basic authentication travels in the clear — the controller does not do TLS for its own web
> server. Treat BLLED as a trusted-LAN device.
>
> If you lock yourself out, a factory reset gets you back in, and so does the USB serial command in
> [Serial provisioning & password recovery](../guides/serial-provisioning.md) — which is gentler,
> because it clears only the login.

## External MQTT / Home Assistant

Off by default. This is a **second, plain (non-TLS) connection**, completely separate from the
printer's own MQTT link. It is how BLLED gets into Home Assistant, Node-RED, or anything else that
speaks MQTT.

Unlike the rest of this tab, these settings take effect **immediately** — no restart needed.

| Setting | Notes |
|---|---|
| **Publish to my own MQTT broker** | The master switch. Publishes everything BLLED knows, and accepts commands back. |
| **Broker host** | Hostname or IP of your broker — the machine running Mosquitto, for instance. Plain TCP only; TLS brokers are not supported. |
| **Port** | `1883`, the standard unencrypted MQTT port. |
| **User name** | Leave empty for an anonymous broker. |
| **Password** | Leave blank to keep the stored one. |
| **Base topic** | The prefix for every topic BLLED publishes. `blled/livingroom` gives `blled/livingroom/status` and `blled/livingroom/set`. Leave it empty to use `blled/<controller name>`. Change it if you run more than one BLLED on the same broker. |
| **Publish interval** | How often the full status object is republished even when nothing changed, in seconds. Changes are always published within a second regardless — this is just the heartbeat. Raise it if you log every message to disk. Default **10 s**. |
| **Home Assistant auto-discovery** | Publishes discovery messages so the controller appears as a device with a light, sensors and buttons, without any YAML. Switching it off removes those entities from Home Assistant again. Default: **on**. |
| **Discovery prefix** | The topic prefix Home Assistant listens on. `homeassistant`, unless you deliberately changed it in your Home Assistant MQTT settings. |

The controller card on the dashboard shows whether the broker connection is actually up.

Next: [Home Assistant](../guides/home-assistant.md) and
[MQTT topics & payloads](../guides/mqtt.md).

<p>
<img src="{{ site.baseurl }}/screenshots/connection-375.png" alt="The Connection tab on a phone" width="260">
</p>
