---
title: Getting started
nav_order: 2
---

# Getting started
{: .no_toc }

From an unflashed board to a strip that follows your printer, in about ten minutes.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

{: .warning }
> ## The first v3 install must be done over a USB cable
>
> This is true for a **brand-new board** and for a board **already running v2.x**.
> v3 uses a larger application partition than v2, so the v2 firmware-update page cannot install
> it — it will refuse the image, or accept it and fail to boot. There is no way around this from
> the browser.
>
> **Flash once over USB. Every update after that is over the air** from
> *System → Firmware update*, using the `.bin.ota` file.

---

## 1. What you need

| | |
|---|---|
| **The board** | A BLLED controller, or any plain ESP32 (`esp32dev`) wired to a 12 V RGB + warm/cold-white strip. See [Hardware](reference/hardware.md). |
| **A USB data cable** | Many cheap USB cables carry power only. If the board never appears as a serial port, try another cable before anything else. |
| **A computer** | Chrome or Edge for the browser flasher, or Python for `esptool`. On Windows you may need the driver for the board's USB-serial chip (CP2102 or CH340) before a port appears. |
| **The printer's IP address** | Printer screen → *Settings → Network*. Give the printer a DHCP reservation so it does not move. |
| **The printer's serial number** | Printer screen → *Settings → Device*. |
| **The printer's LAN access code** | Printer screen → *Settings → Network → LAN Only Mode*. Eight characters. |
| **The printer in LAN mode** | BLLED reads the printer's local MQTT feed. Enable **LAN Only Mode**, or **Developer mode / LAN-only MQTT** on newer firmware, or no reports will arrive. |

Your WiFi must have a **2.4 GHz** band. The ESP32 has no 5 GHz radio, and if your router hides
both bands behind one SSID the join can fail — give the 2.4 GHz band its own name if you can.

## 2. Flash the firmware over USB

The image you want is `firmware/esp32dev/BLLC_V3.0.0.bin` from the repository. It is a **complete**
image — bootloader, partition table and application — which is why it goes to address `0x0`.

### With a browser

1. Connect the board to your computer with the USB data cable.
2. Open an ESP Web Tools flasher page such as <https://esp.huhn.me> in **Chrome or Edge**
   (Firefox and Safari cannot talk to serial ports).
3. Click **Connect** and pick the board's serial port from the browser's dialog.
4. Choose the file `firmware/esp32dev/BLLC_V3.0.0.bin` and set the address to **`0x0`**.
5. Erase, then flash. It takes under a minute.

The repository also carries an ESP Web Tools manifest at `firmware/manifest-v3.json` if you are
hosting your own flasher page.

### From the command line

```bash
pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
           write_flash 0x0 firmware/esp32dev/BLLC_V3.0.0.bin
```

On Windows the port looks like `COM5`; on macOS like `/dev/cu.usbserial-0001`. If the flash fails
part-way, drop the baud rate to `460800`, and hold the board's **BOOT** button while it starts if
your board does not reset itself automatically.

## 3. Power up and watch the strip

Give the board 12 V (a USB cable alone powers the ESP32 but not the strip). The LEDs walk through a
short boot sequence, and the colour tells you exactly how far start-up got:

| Colour | Meaning |
|---|---|
| All channels on | The firmware is running. |
| **Red** | Mounting the file system. |
| **Orange** | Connecting to WiFi. |
| **Blue** | Web server up. |
| **Cyan** | Connecting to the printer's MQTT. |
| Normal colours | Running. |
| **Pink** | No WiFi is configured (or it could not be joined) — the setup access point is open. |

On a fresh install the strip goes **all-on → red → orange → pink** and stays pink, because there
are no WiFi credentials yet. That is what you want at this stage. The controller is now serving an
open access point called **`BLLED_AP`**.

{: .note }
> The default brightness after a fresh install is **20 %**, so an undersized power supply does not
> sag. If the strip looks dim, that is why — raise it later on the LED Behaviour tab.

## 4. Join `BLLED_AP`

On your phone or laptop, open the WiFi list and join **`BLLED_AP`**. There is no password.

Your device will test its new connection, find that the internet is missing, and offer a
**"Sign in to network"** notification — tap it and the setup page opens in a mini-browser. BLLED
answers the connectivity-probe URLs that Android, iOS, macOS, Windows and Firefox use, which is
what makes that prompt appear.

If no prompt appears (some devices are stubborn, and an HTTPS-only browser cannot be intercepted),
just open **`http://192.168.4.1`** in a normal browser while still joined to `BLLED_AP`.

More detail, and what to do when it will not co-operate: [Captive portal](guides/captive-portal.md).

## 5. Fill in the setup page

<p>
<img src="{{ site.baseurl }}/screenshots/wifisetup-1280.png" alt="The BLLED setup page, with a WiFi scan list, controller name, and printer IP, serial number and access code fields (captured from the repository mock server, because the setup page only exists while the controller is in AP mode)" style="max-width:100%">
</p>

1. **WiFi** — pick your 2.4 GHz network from the scan list (or type the name), then the password.
2. **Controller name** — optional but useful. It becomes the mDNS name, so a controller named
   `blled` answers at `http://blled.local`. Letters, digits and hyphens only.
3. **Printer** — press **Discover** to search the network for Bambu printers, or type the IP by
   hand. Discovery only sees printers on the network you are about to join.
4. **Serial number** and **LAN access code** — exactly as they appear on the printer screen. The
   serial number is the MQTT topic and it also tells BLLED which model you have; a typo means no
   reports at all.
5. **Save.**

## 6. The controller restarts and joins your network

Watch the strip: **orange** (joining WiFi) → **blue** (web server) → **cyan** (printer MQTT) →
your normal running colour. `BLLED_AP` disappears, so re-join your own WiFi if your phone does not
do it by itself.

Now open the interface:

- `http://blled.local` — or `http://<the name you chose>.local`
- or the IP address, which your router will show in its client list

{: .note }
> Some networks block mDNS, and Android in particular is unreliable with `.local` names. If the
> name does not resolve, use the IP address. It is worth giving the controller a DHCP reservation
> too.

## 7. First look at the dashboard

![The BLLED dashboard]({{ site.baseurl }}/screenshots/dashboard-1280.png)

The **LED output** card at the top shows the colour the hardware is emitting right now, and — the
useful part — the **reason** the engine chose it: *"Printing (stage 0)"*, *"Chamber light off"*,
*"Printer alert: serious"*, *"Manual override"*. Whenever the strip is not doing what you expect,
that line tells you which rule won.

Below it, the **printer card** shows state and stage, the progress rings, the layer gauge,
temperatures, fans, and status chips for the door, chamber light, SD card and AMS. The
**controller card** shows WiFi signal, addresses, uptime and both MQTT connections.

The defaults reproduce classic BLLED behaviour, so you can stop here:

| | Default |
|---|---|
| Running / idle colour | Warm + cold white at full |
| Brightness | 20 % |
| Follow the printer's chamber light | On |
| Finish indication | On, green, cleared when you open the door |
| Switch off when idle | On, after 60 minutes |
| React to printer alerts | On |
| Lidar stage colours | On (mostly dark, so the sensor is not blinded) |

Everything is explained tab by tab in [Using BLLED](using), and every setting carries a **?**
button with the same explanation.

## 8. Optional extras

- **Lock the interface.** *Connection → Web interface*: set a user name and password. It then
  protects every route, the API included. If you lock yourself out, see
  [Serial provisioning & password recovery](guides/serial-provisioning.md).
- **Home Assistant.** *Connection → External MQTT*: point it at your broker and leave discovery on.
  See [Home Assistant](guides/home-assistant.md).
- **Automate it yourself.** [REST & WebSocket API](guides/api.md) and
  [MQTT topics & payloads](guides/mqtt.md).
- **Two printers?** [Multiple controllers](guides/multiple-controllers.md).

---

## Upgrading from v2

Same hardware, same wiring, same default colours — but the first v3 install still has to come over
USB, and it is worth taking two minutes to back up first.

### 1. Back up the v2 configuration

In the v2 interface: **Backup & Restore → Download**. Keep the JSON file somewhere safe.

{: .warning }
> The backup contains your WiFi password and the printer's access code in plain text. Treat the
> file like a password.

### 2. Flash v3 over USB

Follow [step 2](#2-flash-the-firmware-over-usb) above. The v2 OTA page cannot do this: v3's
application image is around 1.37 MB, and the v2 partition table only has a 1.31 MB application
slot. Uploading the v3 image there fails.

Your existing configuration file survives the flash if you do **not** erase the whole chip — but
erase is the safer, more predictable route, and that is what the backup is for.

### 3. Settings are migrated automatically

v3 reads a v2 configuration file and converts it on first boot, and it does the same when you
restore a v2 backup through *System → Backup & restore*. Among other things:

- the five old mode checkboxes (`maintMode`, `discoMode`, `showtestcolor`, `debugwifi`) collapse
  into one **LED mode**,
- timers stored in milliseconds become **minutes**,
- `replicatestate` becomes **follow chamber light**,
- `doorSwitch` becomes **lidar stage colours**,
- `ssid` / `appw` / `printerIp` / `HTTPUser` become `wifiSSID` / `wifiPass` / `printerIP` /
  `webUser`.

The complete key map is in the [Changelog](CHANGELOG.md#configuration-keys), and
[Backup & restore](guides/backup-restore.md) explains what the file contains.

### 4. From now on, update over the air

*System → Firmware update* takes the **`.bin.ota`** file — `firmware/esp32dev/BLLC_V3.0.0.bin.ota`,
not the full `.bin`. Settings survive the update.

### Behaviour worth knowing about

- The strip no longer freezes on an old error after a print is cancelled.
- Brightness 0 % really is off, on every channel.
- The door double-close toggle stays off until the next door interaction or the next print,
  instead of any MQTT message waking it up.
- The old form endpoints (`/submitConfig`, `/submitWiFi`), the GET factory reset and the
  `/config.json` route that returned your WiFi password in plain text are gone. `/getConfig`,
  `/configfile.json`, `/printerList`, `/update` and `/configrestore` still work as aliases.
