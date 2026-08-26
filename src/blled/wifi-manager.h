#ifndef _BLLEDWIFI_MANAGER
#define _BLLEDWIFI_MANAGER

// ---------------------------------------------------------------------------
// wifi-manager.h -- station connect ladder, AP/captive portal, async scan
//
// Responsibilities
//   * connectToWifi(): the upstream connect ladder (BSSID -> SSID -> open ->
//     BSSID) used ONCE from setup(); blocking there is fine.
//   * wifiLoop(): non-blocking reconnect with back-off (REVIEW #6) -- two quick
//     WiFi.reconnect() attempts, then at most one rescan per 60 s.
//   * wifiScanRequest()/wifiScanResultsJson(): asynchronous scan for the API
//     (REVIEW #5: the synchronous scan inside an async handler tripped the
//     watchdog).  A scan can also be started to re-pin the strongest BSSID.
//   * startAPMode() + the captive-portal DNS server.
//
// Threading
//   Main loop only, except wifiScanRequest()/wifiScanResultsJson() which the
//   async web task may call (they only touch the WiFi scan API, which is
//   itself task-safe, and never block).
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <DNSServer.h>

#include "types.h"
#include "logSerial.h"
#include "filesystem.h"

DNSServer dnsServer;
IPAddress apIP(192, 168, 4, 1);

#define WIFI_QUICK_RETRIES 2
#define WIFI_RETRY_MS 5000UL      // between quick reconnect attempts
#define WIFI_RESCAN_MS 60000UL    // between full rescans while disconnected
#define WIFI_SCAN_MAX_AGE_MS 60000UL
#define WIFI_BOOT_MAX_CHECKS 30     // x 2 s = ~60 s before falling back to AP mode

static int connectionAttempts = 1;
static int wifimode = 0;
static uint8_t bssid[6] = {0};

static int wifiReconnectCount = 0;
static unsigned long wifiLastAttemptMs = 0;
static unsigned long wifiLastRescanMs = 0;
static bool wifiScanPending = false;
static bool wifiScanForBssid = false;
static unsigned long wifiScanStartedMs = 0;

const char *wl_status_to_string(wl_status_t status)
{
    switch (status)
    {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "UNKNOWN";
    }
}

static int str2mac(const char *mac, uint8_t *values)
{
    if (mac == NULL)
        return 0;
    return (6 == sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                        &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]))
               ? 1
               : 0;
}

// ---------------------------------------------------------------------------
// Asynchronous scan
// ---------------------------------------------------------------------------

// Start an async scan (safe from the async web task). Idempotent while running.
void wifiScanRequest()
{
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING)
        return;
    if (wifiScanPending && (millis() - wifiScanStartedMs) < WIFI_SCAN_MAX_AGE_MS)
        return;
    WiFi.scanDelete();
    WiFi.scanNetworks(true /* async */, true /* show hidden */);
    wifiScanPending = true;
    wifiScanStartedMs = millis();
}

// {"scanning":true} while a scan is in flight, otherwise the sorted result list.
void wifiScanResultsJson(JsonDocument &doc)
{
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING || (n < 0 && wifiScanPending))
    {
        doc["scanning"] = true;
        return;
    }
    if (n < 0)
    {
        wifiScanRequest();
        doc["scanning"] = true;
        return;
    }

    // sort indices by RSSI, strongest first
    int order[32];
    int count = (n > 32) ? 32 : n;
    for (int i = 0; i < count; i++)
        order[i] = i;
    for (int i = 1; i < count; i++)
    {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && WiFi.RSSI(order[j]) < WiFi.RSSI(key))
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < count; i++)
    {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"] = WiFi.SSID(order[i]);
        net["bssid"] = WiFi.BSSIDstr(order[i]);
        net["rssi"] = WiFi.RSSI(order[i]);
        net["channel"] = WiFi.channel(order[i]);
        net["secure"] = (WiFi.encryptionType(order[i]) != WIFI_AUTH_OPEN);
    }
}

// Pick the strongest BSSID for the configured SSID out of a finished scan.
static void wifiApplyStrongestBssid(int16_t n)
{
    int bestRSSI = -200;
    String bestBSSID = "";
    for (int16_t i = 0; i < n; i++)
    {
        if (WiFi.SSID(i) == globalVariables.SSID && WiFi.RSSI(i) > bestRSSI)
        {
            bestRSSI = WiFi.RSSI(i);
            bestBSSID = WiFi.BSSIDstr(i);
        }
    }
    if (bestBSSID.length() == 0)
    {
        LogSerial.println(F("[WiFi] Rescan: configured SSID not found"));
        return;
    }
    // REVIEW #19: upstream compared a char* with a String's char* (never equal).
    if (strcmp(printerConfig.BSSID, bestBSSID.c_str()) == 0)
    {
        LogSerial.printf("[WiFi] Strongest AP unchanged (%s, %d dBm)\n", printerConfig.BSSID, bestRSSI);
        return;
    }
    strlcpy(printerConfig.BSSID, bestBSSID.c_str(), sizeof(printerConfig.BSSID));
    LogSerial.printf("[WiFi] Pinned strongest AP %s (%d dBm)\n", printerConfig.BSSID, bestRSSI);
    configDirty = true;
}

// Poll a running scan; called from the main loop.
static void wifiScanPoll()
{
    if (!wifiScanPending)
        return;
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING)
        return;
    wifiScanPending = false;
    if (n < 0)
        return;
    LogSerial.printf("[WiFi] Scan complete: %d networks\n", (int)n);
    if (wifiScanForBssid)
    {
        wifiScanForBssid = false;
        wifiApplyStrongestBssid(n);
    }
}

// Request an async scan whose result re-pins printerConfig.BSSID.
void wifiRescanStrongest()
{
    wifiScanForBssid = true;
    wifiScanRequest();
}

// ---------------------------------------------------------------------------
// Connect / AP mode
// ---------------------------------------------------------------------------

// Blocking connect ladder -- setup() only.
bool connectToWifi()
{
    Serial.println(F("-------------------------------------"));
    WiFi.mode(WIFI_STA);
    delay(10);
    WiFi.disconnect();

    if (strlen(printerConfig.BSSID) == 0 || !str2mac(printerConfig.BSSID, bssid))
    {
        if (strlen(printerConfig.BSSID) > 0)
            Serial.println(F("Parsing MAC address failed, connecting by SSID only"));
        WiFi.begin(globalVariables.SSID, globalVariables.APPW);
        wifimode = 1;
    }
    else
    {
        WiFi.begin(globalVariables.SSID, globalVariables.APPW, 0, bssid);
        wifimode = 0;
    }

    wl_status_t status = WiFi.status();
    int totalChecks = 0;
    while (status != WL_CONNECTED)
    {
        // Give up after ~60 s across all ladder modes and fall back to the setup
        // AP (main.cpp retries STA periodically while the AP is up).
        if (++totalChecks > WIFI_BOOT_MAX_CHECKS)
        {
            Serial.println(F("WiFi: giving up for now (timeout)"));
            return false;
        }
        if (connectionAttempts > 10)
        {
            WiFi.disconnect();
            if (wifimode == 0)
            {
                WiFi.begin(globalVariables.SSID, globalVariables.APPW);
                wifimode = 1;
                Serial.println(F("Attempting to connect without a specific BSSID"));
            }
            else if (wifimode == 1)
            {
                WiFi.begin(globalVariables.SSID);
                wifimode = (strlen(printerConfig.BSSID) == 0) ? 0 : 2;
                Serial.println(F("Attempting to connect to an open network (no password)"));
            }
            else
            {
                WiFi.begin(globalVariables.SSID, globalVariables.APPW, 0, bssid);
                wifimode = 0;
                Serial.println(F("Attempting to connect using the stored BSSID"));
            }
            connectionAttempts = 1;
        }

        Serial.printf("Connecting to WIFI.. check #%d / 10   SSID: %s", connectionAttempts, globalVariables.SSID);
        if (strlen(printerConfig.BSSID) > 0)
            Serial.printf("   BSSID: %s", printerConfig.BSSID);
        Serial.println();

        if (status != WiFi.status())
        {
            status = WiFi.status();
            Serial.printf("Wifi Status: %s\n", wl_status_to_string(status));
            if (status == WL_NO_SSID_AVAIL)
            {
                Serial.println(F("Bad WiFi credentials"));
                return false;
            }
            // WL_DISCONNECTED is the normal transient state right after
            // WiFi.begin(); upstream bailed to AP mode here (stranding the
            // device after a router reboot). Keep trying until the timeout.
            if (status == WL_CONNECT_FAILED)
                Serial.println(F("Connect failed (wrong password?) - retrying"));
        }
        delay(2000);
        connectionAttempts++;
    }

#ifdef ESP32
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
#endif

    // !!! The next three lines are parsed by wifiSetup.html - keep the format.
    Serial.print(F("IP_ADDRESS:"));
    Serial.print(WiFi.localIP());
    Serial.println(F("\n         "));

    Serial.printf("Connected to '%s'  BSSID %s  RSSI %d dBm\n",
                  globalVariables.SSID, WiFi.BSSIDstr().c_str(), (int)WiFi.RSSI());
    Serial.print(F("BLLED Controller IP Address:     "));
    Serial.println(WiFi.localIP());
    Serial.print(F("Use a web browser to open 'http://"));
    Serial.print(WiFi.localIP());
    Serial.println(F("/' for the setup page"));
    return true;
}

void startAPMode()
{
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_AP);
    delay(500);
    WiFi.softAP("BLLED_AP");
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    dnsServer.start(53, "*", apIP);
    globalVariables.apMode = true;

    Serial.print(F("[WiFiManager] AP started on IP: "));
    Serial.println(WiFi.softAPIP());
}

// Non-blocking reconnect handling; call from the main loop while in STA mode.
void wifiLoop()
{
    wifiScanPoll();

    if (globalVariables.apMode || WiFi.getMode() == WIFI_AP)
        return;

    if (WiFi.status() == WL_CONNECTED)
    {
        if (wifiReconnectCount != 0)
        {
            LogSerial.println(F("[WiFi] Reconnected"));
            wifiReconnectCount = 0;
        }
        return;
    }

    unsigned long now = millis();
    if (wifiReconnectCount < WIFI_QUICK_RETRIES)
    {
        if ((now - wifiLastAttemptMs) < WIFI_RETRY_MS)
            return;
        wifiLastAttemptMs = now;
        wifiReconnectCount++;
        LogSerial.printf("[WiFi] Link down (%s) - reconnect attempt %d\n",
                         wl_status_to_string(WiFi.status()), wifiReconnectCount);
        WiFi.reconnect();
        return;
    }

    // Still down: rescan for the strongest AP, but at most once a minute.
    if ((now - wifiLastRescanMs) < WIFI_RESCAN_MS)
        return;
    wifiLastRescanMs = now;
    LogSerial.println(F("[WiFi] Still down - rescanning for the strongest AP"));
    wifiRescanStrongest();
    WiFi.disconnect();
    WiFi.begin(globalVariables.SSID, globalVariables.APPW);
}

#endif
