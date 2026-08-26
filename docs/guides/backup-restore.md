---
title: Backup & restore
parent: Guides
nav_order: 9
---

# Backup & restore
{: .no_toc }

The whole configuration is one flat, human-readable JSON file. You can download it, edit it, and
push it back.

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## Making a backup

*System → Backup & restore → **Download***, or:

```bash
curl -s http://192.168.1.50/api/config/backup -o blled-backup.json
```

{: .warning }
> **The backup contains secrets in plain text** — your WiFi password, the printer's LAN access
> code, the web-interface password and the MQTT broker password. Treat the file exactly as you
> would treat those passwords. Do not commit it to a repository or paste it into an issue.
>
> (`GET /api/config`, by contrast, redacts them. The *backup* endpoint deliberately does not, so
> that a restore actually restores.)

Take one before a factory reset, before flashing v3 over v2, and before you start experimenting.

## Restoring

*System → Backup & restore → **Upload***, or:

```bash
curl -X POST -H 'Content-Type: application/json' \
     --data-binary @blled-backup.json http://192.168.1.50/api/config/restore
```

{: .warning }
> A restore **replaces every setting** — it is not a merge — and the controller restarts about a
> second and a half later. WiFi credentials and the printer's identity are included, so restoring
> the wrong file moves the controller onto a different network and a different printer.

## What is in the file

Flat keys, one per setting, with the names used throughout the [Settings reference](../manual.md).

```json
{
  "wifiSSID": "MyHomeWiFi",
  "wifiPass": "…",
  "BSSID": "",
  "host": "blled",
  "printerIP": "192.168.1.60",
  "printerAutoIp": true,
  "serialNumber": "00M09A123456789",
  "accessCode": "12345678",
  "webUser": "",
  "webPass": "",

  "ledMode": "auto",
  "brightness": 20,
  "fadeMs": 500,
  "effectSpeed": 5,
  "followChamberLight": true,
  "printingVisual": "solid",
  "preheatVisual": "solid",

  "runningRGB": "#000000", "runningWW": 255, "runningCW": 255,
  "finishRGB": "#00FF00",  "finishWW": 0,    "finishCW": 0,

  "finishIndication": true,
  "finishEffect": "solid",
  "finishExitMode": "door",
  "finishTimerMins": 10,
  "inactivityEnabled": true,
  "inactivityMins": 60,
  "controlChamberLight": false,
  "doorToggleEnabled": true,
  "offlineTimeoutSec": 30,
  "isP1Printer": false,
  "lidarStagesEnabled": true,

  "errorDetection": true,
  "errorEffect": "solid",
  "pauseEffect": "solid",
  "hmsCommonEnabled": false,
  "hmsIgnoreList": "",

  "mqttExtEnabled": false,
  "mqttExtHost": "", "mqttExtPort": 1883,
  "mqttExtUser": "", "mqttExtPass": "",
  "mqttExtBaseTopic": "", "mqttExtIntervalSec": 10,
  "haDiscovery": true, "haPrefix": "homeassistant",

  "debugChanges": true, "debugVerbose": false, "debugMqtt": false
}
```

### Colours are three keys each

Every colour is stored as `<name>RGB` (a hex string), `<name>WW` and `<name>CW` (0–255 for the warm
and cold white channels). The names are `running`, `maintenance`, `test`, `wifi`, `preheat`,
`finish`, `pause`, `firstLayer`, `nozzleClog`, `hmsSerious`, `hmsFatal`, `hmsCommon`,
`filamentRunout`, `frontCover`, `nozzleTemp`, `bedTemp`, and `stage1`, `stage8`, `stage9`,
`stage10`, `stage14`.

### The ignore list

`hmsIgnoreList` is one string. Entries are normalised on save: upper-cased, hyphens turned into
underscores, whitespace stripped, and separated by commas.

## Editing a backup by hand

It is a plain JSON file, so this is entirely reasonable — it is the fastest way to clone a setup
onto a second controller. Change the per-device keys (`host`, `printerIP`, `serialNumber`,
`accessCode`) and leave the rest.

Unknown keys are ignored and out-of-range values are clamped or rejected, so a typo cannot brick
the device — but it can silently not do what you meant. Check the interface after a restore.

If you only want to change a few settings, `PUT /api/config` with a partial object is safer than a
full restore, because it does not touch anything you did not send. See the
[API guide](api.md#change-settings-put-apiconfig).

## v2 backups are accepted

A configuration file saved from **v2.x** restores into v3 and is migrated on the way in — the same
migration that runs when v3 boots for the first time on a board that had v2 on it.

Among the conversions: the five old mode flags become one `ledMode`, timers in milliseconds become
minutes, `replicatestate` becomes `followChamberLight`, `doorSwitch` becomes `lidarStagesEnabled`,
and `ssid` / `appw` / `printerIp` / `HTTPUser` / `HTTPPass` become `wifiSSID` / `wifiPass` /
`printerIP` / `webUser` / `webPass`.

The complete key map is in the [Changelog](../CHANGELOG.md#configuration-keys).

{: .note }
> The migration runs in one direction only. A v3 backup will not restore into v2.

## Factory reset

*System → Factory reset* deletes the configuration file entirely — WiFi credentials, access code
and web login included — and reboots into the `BLLED_AP` setup network. It needs the web password,
if you have set one.

```bash
curl -X POST http://192.168.1.50/api/config/reset
```

Take a backup first if you want to come back.
