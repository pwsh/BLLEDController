---
title: Flash firmware
nav_order: 3
---

# Flash the firmware from your browser
{: .no_toc }

This page flashes **BLLED v3.0.0** straight from the browser — no downloads, no command line.
It works in **Chrome, Edge or Opera** on a computer (the browser needs Web Serial, which Firefox,
Safari and phones do not have). Use it for a **new board** and for the **one-time upgrade from
v2.x**; every later update goes over the air from the *System* tab.

{: .warning }
> Flashing erases the board's configuration. If it is a v2 board you want to keep the settings of,
> download a backup first (*Backup & Restore → Download*) — v3 restores and migrates v2 backups.

## 1. Connect the board

Plug the board into the computer with a **USB data cable** (many cables are charge-only). On
Windows you may need the driver for the board's USB-serial chip (CP2102 or CH340) before a port
shows up.

## 2. Click Install

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
<div style="margin:1.5rem 0">
  <esp-web-install-button manifest="{{ site.baseurl }}/firmware/manifest.json">
    <button slot="activate" class="btn btn-primary fs-5">Install BLLED v3.0.0</button>
    <span slot="unsupported">Your browser does not support Web Serial — use Chrome or Edge on a computer, or the command line below.</span>
    <span slot="not-allowed">This page must be opened over HTTPS for the flasher to work.</span>
  </esp-web-install-button>
</div>

1. Pick the board's serial port in the dialog that opens.
2. Choose **Install** (it offers to erase the flash — say yes for a v2 board or a used ESP32).
3. Wait about a minute. When it finishes the board reboots on its own.

The flasher loads its firmware image from this site
(<code>{{ site.baseurl }}/firmware/BLLC_V3.0.0.bin</code>, a full image written at address 0),
so what you install is exactly the file published with this documentation.

## 3. Set it up

After the reboot the strip cycles white → red → orange and settles on **pink**: the board is
waiting for you on its own WiFi network **`BLLED_AP`**. Continue with
[Getting started, step 3]({{ site.baseurl }}/getting-started/#3-power-up-and-watch-the-strip) —
join `BLLED_AP`, follow the "Sign in to network" prompt (or open `http://192.168.4.1`) and enter your
WiFi and printer details.

## Command-line alternative

If you cannot use a Web-Serial browser:

```
pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 BLLC_V3.0.0.bin
```

(`COM3`-style port names on Windows; download `BLLC_V3.0.0.bin` from
[the repository](https://github.com/pwsh/BLLEDController/tree/v3-rework/firmware/esp32dev).
If the write fails with "serial data stream stopped", retry at `--baud 115200`.)
