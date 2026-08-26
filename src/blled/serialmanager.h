#ifndef _BLLEDSERIAL_MANAGER
#define _BLLEDSERIAL_MANAGER

// ---------------------------------------------------------------------------
// serialmanager.h -- provisioning over the USB serial port.
//
// Accepts a single JSON line with either the v3 key names or the upstream ones:
//   {"wifiSSID":"..","wifiPass":"..","printerIP":"..","accessCode":"..","serialNumber":".."}
//   {"ssid":"..","pass":"..","printerip":"..","printercode":"..","printerserial":".."}
// Every field is copied with strlcpy after a NULL check (REVIEW #20: upstream
// strcpy'd straight from possibly-absent JSON fields into 9/16 byte buffers).
//
// Threading: main loop only.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>

#include "types.h"
#include "filesystem.h"

void setupSerial()
{
    // (upstream had `while (!Serial);` here, which is a no-op on the ESP32)
    // Bound readStringUntil() so a partial line cannot stall the main loop.
    Serial.setTimeout(50);
}

// Copy doc[key] (or doc[legacy]) into dst when present and non-empty.
static bool serialCopyField(JsonDocument &doc, const char *key, const char *legacy, char *dst, size_t size)
{
    const char *value = doc[key] | (const char *)NULL;
    if (value == NULL && legacy != NULL)
        value = doc[legacy] | (const char *)NULL;
    if (value == NULL || value[0] == '\0')
        return false;
    strlcpy(dst, value, size);
    return true;
}

void serialLoop()
{
    if (Serial.available() <= 0)
        return;

    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() == 0)
        return;

    JsonDocument doc;
    if (deserializeJson(doc, input))
        return; // not JSON - ignore (keeps the port usable for logging)

    bool changed = false;
    changed |= serialCopyField(doc, "wifiSSID", "ssid", globalVariables.SSID, sizeof(globalVariables.SSID));
    changed |= serialCopyField(doc, "wifiPass", "pass", globalVariables.APPW, sizeof(globalVariables.APPW));
    changed |= serialCopyField(doc, "printerIP", "printerip", printerConfig.printerIP, sizeof(printerConfig.printerIP));
    changed |= serialCopyField(doc, "accessCode", "printercode", printerConfig.accessCode, sizeof(printerConfig.accessCode));
    changed |= serialCopyField(doc, "serialNumber", "printerserial", printerConfig.serialNumber, sizeof(printerConfig.serialNumber));
    changed |= serialCopyField(doc, "host", NULL, globalVariables.Host, sizeof(globalVariables.Host));

    if (!changed)
        return;

    Serial.printf("SSID %s\nPrinter %s (%s)\n", globalVariables.SSID, printerConfig.printerIP,
                  printerConfig.serialNumber);
    validateConfig();
    saveConfig();
    Serial.println(F("Restarting Device"));
    restartRequested = true;
    restartRequestMs = millis();
}

#endif
