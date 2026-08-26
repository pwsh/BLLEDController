---
title: System
parent: Using BLLED
nav_order: 6
---

# System
{: .no_toc }

Firmware information, over-the-air updates, backups, the reset buttons and the live log.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

<p>
<img src="{{ site.baseurl }}/screenshots/system-1280.png" alt="The System tab" style="max-width:100%">
</p>

---

## Firmware information

Version and codename, build date, chip type and revision, core count, flash size, the space the
running sketch uses and how much is free, the SDK version, the GPIO pin map and the library
versions. It is the first thing to quote when you report a problem.

## Firmware update (OTA)

Upload a new firmware image over WiFi. Settings survive the update.

{: .warning }
> Use the **`.bin.ota`** file — `firmware/esp32dev/BLLC_V3.0.0.bin.ota` — not the full `.bin`.
> The full `.bin` is a complete flash image (bootloader + partition table + application) meant for
> USB flashing at address `0x0`; the `.ota` file is just the application.
>
> A rejected or corrupt image is refused with a reason and the running firmware is untouched. An
> image built for a different board will flash but not boot, and then you need a USB cable.

The upload is authenticated like everything else, so if you have set a web login you will need it
here.

{: .note }
> **Coming from v2?** This page cannot help you. The v2 firmware's own update page cannot install
> v3 at all — v3 needs a bigger application partition. Flash v3 once over USB, and *then* this page
> works for every future update. See [Upgrading from v2](../getting-started.md#upgrading-from-v2).

## Backup & restore

**Download** saves the complete configuration as a JSON file. **Upload** restores one you saved
earlier.

Restoring **replaces every setting** — it is not a merge — and restarts the controller. A backup
made with v2 is accepted and migrated automatically.

{: .warning }
> The backup contains your WiFi password and the printer's LAN access code in plain text. Treat the
> file like a password.

Details, including what the keys mean: [Backup & restore](../guides/backup-restore.md).

## The buttons

**Restart controller** reboots the ESP32. The LEDs go dark for a few seconds and the page
reconnects by itself.

**Reconnect printer MQTT** forces a fresh connection to the printer. Try it if the printer card
says disconnected but the printer is plainly reachable.

**Refresh printer state** asks the printer for a full report (`pushall`). P1 and A1 printers only
send *changes*, so if the dashboard looks connected but stale, this is the nudge.

**Factory reset** deletes the configuration file — WiFi credentials, access code and web login
included — and reboots into the `BLLED_AP` setup network. Take a backup first if you want to come
back. It needs the web password, if you have set one.

## Debug logging

| Switch | Default | What it does |
|---|---|---|
| **Log state changes** | **On** | Logs only when something actually changes: stage transitions, LED decisions, door events, connection changes. This is the useful one to leave on — it is quiet when the printer is quiet. |
| **Verbose log** | Off | Logs everything to the serial console and the web log. Very chatty (the printer reports every second), so use it while chasing a problem and turn it off afterwards. |
| **Log printer MQTT reports** | Off | Logs the filtered contents of every printer report. **USB serial only** — never the web log, because it is far too much traffic for a WebSocket. For diagnosing parsing problems. |

## The log console

The live log opens at `/webserial`, or from the button on this tab. It shows every state change
with the **reason** for each LED decision, which makes it the fastest way to understand a colour
you did not expect. Turn on *Log state changes* to make it useful.

<p>
<img src="{{ site.baseurl }}/screenshots/system-375.png" alt="The System tab on a phone" width="260">
</p>
