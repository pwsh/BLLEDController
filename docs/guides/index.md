---
title: Guides
nav_order: 4
has_children: true
---

# Guides

Task-shaped pages: how to wire BLLED into something else, and how the less obvious features
actually work.

## Connecting it to other things

- [Home Assistant](home-assistant.md) — enable the broker, what the 22 entities are, an example
  automation
- [REST & WebSocket API](api.md) — `curl` your way around the controller
- [MQTT topics & payloads](mqtt.md) — what BLLED publishes and what it will accept

## Understanding what you are looking at

- [LED effects & visualisations](led-effects.md) — the priority ladder, the effects, and a table of
  every state and its default colour
- [Layer progress](layer-progress.md) — how the inner ring is estimated, and where the estimate
  breaks down
- [Door sensor](door-sensor.md) — the one switch the printer has, and the "not reported" state
- [Captive portal](captive-portal.md) — how the setup prompt appears, and what to do when it does
  not

## Housekeeping

- [Multiple controllers](multiple-controllers.md) — naming, `blled2.local`, and why settings do not
  sync
- [Backup & restore](backup-restore.md) — what is in the JSON, and restoring a v2 file
- [Serial provisioning & password recovery](serial-provisioning.md) — configuring over USB, and
  getting back in
