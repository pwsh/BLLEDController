# Screenshots

Images used by the BLLED v3 documentation site.

| File | Shows |
|---|---|
| `dashboard-1280.png` | Dashboard tab, desktop width — LED output card, printer card and controller card for a print that has just finished. |
| `dashboard-375.png` | The same Dashboard on a phone-width screen, with the bottom tab bar. |
| `dashboard-light-1280.png` | Dashboard rendered in the light colour scheme (`prefers-color-scheme: light`). |
| `override-1280.png` | Dashboard while a manual override is active — orange at 90 %, with the "Override active, … left" countdown running. |
| `led-1280.png` | LED Behaviour tab, desktop — mode selector, brightness/fade/effect speed, the four base colours and the printing/preheat visualisations. |
| `led-375.png` | LED Behaviour tab on a phone-width screen. |
| `tooltip-1280.png` | LED Behaviour tab with the `?` help bubble for **Brightness** open. |
| `events-1280.png` | Print Events tab, desktop — finish indication, idle & door handling, printer type and the lidar stage colours. |
| `events-375.png` | Print Events tab on a phone-width screen. |
| `alerts-1280.png` | Errors & Alerts tab, desktop — detection toggles, pause colours, fault colours and the HMS ignore list. |
| `alerts-375.png` | Errors & Alerts tab on a phone-width screen. |
| `connection-1280.png` | Connection tab, desktop — WiFi, printer (IP / serial / LAN access code), web interface login and external MQTT. |
| `connection-375.png` | Connection tab on a phone-width screen. |
| `system-1280.png` | System tab, desktop — firmware info, OTA update, backup & restore, debug switches and maintenance actions. |
| `system-375.png` | System tab on a phone-width screen. |
| `wifisetup-1280.png` | The captive-portal first-time setup page (`/wifi`), desktop width. |
| `wifisetup-375.png` | The captive-portal first-time setup page (`/wifi`) on a phone-width screen. |

## Notes

All screenshots were captured from a live BLLED controller attached to a real
Bambu Lab X1C, except `wifisetup-1280.png` and `wifisetup-375.png`, which come
from `tools/mock_server.py` — the `/wifi` route only exists while the controller
is in its setup access-point mode.

Personal data has been replaced with placeholder values before capture:

| Real value | Shown as |
|---|---|
| WiFi SSID | `MyHomeWiFi` |
| Controller IP | `192.168.1.50` |
| Printer IP | `192.168.1.60` |
| Printer serial number | `00M09A123456789` |
| MAC / BSSID addresses | `A4:CF:12:34:56:78` |
| LAN access code | `12345678` (and masked in the UI anyway) |

The controller host name `BLLEDX1C` and the job name `plate_2.gcode` are not
personal data and are shown as they were.
