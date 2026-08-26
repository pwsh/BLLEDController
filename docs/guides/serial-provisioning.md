---
title: Serial provisioning & password recovery
parent: Guides
nav_order: 10
---

# Serial provisioning & password recovery
{: .no_toc }

The USB serial port accepts a couple of JSON commands. They are the way in when the network is not
an option — and the gentle way back when you have locked yourself out.

---

## Connecting

Plug the board into a computer with a USB **data** cable and open a serial terminal at
**115200 baud**.

```bash
pio device monitor                    # if you have PlatformIO
screen /dev/ttyUSB0 115200            # macOS / Linux
```

Anything that can send a line of text works — `screen`, `minicom`, the Arduino IDE's serial
monitor, PuTTY. The port is `COMx` on Windows and `/dev/cu.usbserial-*` on macOS.

The same port carries the log, so turning on *Log state changes* on the
[System](../using/system.md#debug-logging) tab makes this a useful place to watch the controller
think.

## Provisioning over USB

Send **one JSON line** with the settings you want. The controller saves them and restarts.

```json
{"wifiSSID":"MyHomeWiFi","wifiPass":"…","printerIP":"192.168.1.60","accessCode":"12345678","serialNumber":"00M09A123456789"}
```

That is the whole first-time setup without ever joining `BLLED_AP` — handy on a bench, and handy
when you are configuring several boards in a row.

You can send a subset; keys you leave out are not touched.

{: .note }
> One line, then Enter. Most terminals send exactly that. If nothing happens, check that your
> terminal is appending a newline and that the baud rate really is 115200.

## Recovering a forgotten web password

{: .warning }
> If you have set a web-interface user and password and lost them, **every** route is locked —
> including the factory reset button, which needs the password.

Send this line instead:

```json
{"resetAuth":true}
```

The controller **clears the user name and password** and restarts. Nothing else is touched: your
WiFi, the printer's details, and every colour you have set survive.

That is almost always what you want. A factory reset would also work, but it throws away the whole
configuration and drops you back into `BLLED_AP`.

## When the serial port is the only option

- **You forgot the web password** — `{"resetAuth":true}`, as above.
- **The controller is on a network you no longer have** — send new WiFi credentials over serial
  rather than hunting for the setup AP.
- **`BLLED_AP` never appears** — check the log at 115200 baud; it will say what start-up did.
- **The device does not boot at all** — the log is the only thing that will tell you why. A wrong
  firmware image flashed over OTA lands you here, and the fix is to re-flash over USB (see
  [Getting started](../getting-started.md#2-flash-the-firmware-over-usb)).

## Keep the secrets out of your shell history

The provisioning line contains your WiFi password and the printer's access code. If you script it,
mind where the file ends up, and remember that a terminal scrollback is a file too.
