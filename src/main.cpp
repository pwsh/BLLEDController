// ---------------------------------------------------------------------------
// BLLED v3 -- Bambu Lab LED Controller
//
// Single translation unit: every module is a header defining its own globals and
// is included exactly once, in dependency order (ARCHITECTURE.md §3).
//
// Threading (ARCHITECTURE.md §2)
//   * this file owns setup()/loop(): LED hardware, timers, LittleFS writes,
//     mDNS/SSDP, WiFi reconnect, printer discovery and the WebSocket push.
//   * mqttTask (mqttmanager.h) owns the printer MQTT client and writes
//     printerState under STATE_LOCK().
//   * the AsyncTCP task only reads/writes the config structs under the lock and
//     raises configDirty / ledDirty / restartRequested.
//   loop() never blocks: no delay() other than a 1 ms yield at the end.
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "./blled/types.h"
#include "./blled/stages.h"
#include "./blled/logSerial.h"
#include "./blled/filesystem.h"
#include "./blled/leds.h"
#include "./blled/mqttparsingutility.h"
#include "./blled/AutoGrowBufferStream.h"
#include "./blled/mqttmanager.h"
#include "./blled/wifi-manager.h"
#include "./blled/bblPrinterDiscovery.h"
#include "./blled/serialmanager.h"
#include "./blled/api.h"
#include "./blled/mqttpublish.h"
#include "./blled/web-server.h"
#include "./blled/ssdp.h"

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void setup()
{
    // The lock must exist before anything can touch the shared state.
    stateMutex = xSemaphoreCreateRecursiveMutex();

    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.printf("** BLLED starting, firmware version %s **\n", globalVariables.FWVersion);
    Serial.printf("   free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());

    applyConfigDefaults();

    // Boot colour sequence (same order/colours as upstream, now non-blocking).
    setupLeds();
    ledBootColor(100, 100, 100, 100, 100); // ALL CHANNELS ON

    ledBootColor(255, 0, 0, 0, 0); // RED - filesystem
    setupFileSystem();
    loadConfig();

    ledBootColor(printerConfig.wifiColor); // customisable, default ORANGE - wifi
    setupSerial();

    if (strlen(globalVariables.SSID) == 0 || strlen(globalVariables.APPW) == 0)
    {
        Serial.println(F("SSID or password missing - starting the setup access point."));
        ledBootColor(100, 0, 100, 0, 0); // PINK - AP mode
        startAPMode();
        setupWebserver();
        return;
    }

    if (!connectToWifi())
    {
        Serial.println(F("[WiFiManager] Not connected -> AP Mode"));
        ledBootColor(100, 0, 100, 0, 0); // PINK - AP mode
        startAPMode();
        setupWebserver();
        return;
    }

    Serial.println(F("[WiFiManager] Connected. Starting the web UI."));
    ledBootColor(0, 0, 255, 0, 0); // BLUE - web server
    setupWebserver();
    start_ssdp();

    ledBootColor(34, 224, 238, 0, 0); // CYAN - printer MQTT
    setupMqtt();

    Serial.printf("\n** BLLED Controller started (firmware %s) **\n\n", globalVariables.FWVersion);
    globalVariables.started = true;
    ledDirty = true;
}

// ---------------------------------------------------------------------------
// AP-mode recovery: when the boot-time connect failed (router down, DHCP slow)
// upstream stayed in AP mode until a power cycle. Every AP_RETRY_INTERVAL_MS we
// bring the station interface up next to the AP for AP_RETRY_WINDOW_MS; if it
// connects we restart into the normal STA boot path. The AP keeps serving the
// captive portal the whole time, so a user can still (re)configure.
// ---------------------------------------------------------------------------
#define AP_RETRY_INTERVAL_MS 120000UL
#define AP_RETRY_WINDOW_MS 30000UL
static unsigned long apRetryStartedMs = 0;
static bool apRetryActive = false;

static void apModeRetryLoop()
{
    static unsigned long lastCycleMs = 0;
    if (!globalVariables.apMode || strlen(globalVariables.SSID) == 0 || strlen(globalVariables.APPW) == 0)
        return;

    unsigned long now = millis();
    if (!apRetryActive)
    {
        if (now - lastCycleMs < AP_RETRY_INTERVAL_MS)
            return;
        lastCycleMs = now;
        apRetryActive = true;
        apRetryStartedMs = now;
        LogSerial.println(F("[WiFiManager] AP mode: retrying the station connection in the background"));
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(globalVariables.SSID, globalVariables.APPW);
        return;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        LogSerial.println(F("[WiFiManager] Station connected - restarting into normal mode"));
        restartRequested = true;
        restartRequestMs = now;
        apRetryActive = false;
        return;
    }
    if (now - apRetryStartedMs > AP_RETRY_WINDOW_MS)
    {
        // give the AP its channel back until the next cycle
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);
        apRetryActive = false;
    }
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------
void loop()
{
    serialLoop();

    // Captive portal DNS must answer in AP mode -- upstream had this behind
    // `globalVariables.started`, which is never true in AP mode (REVIEW #17).
    if (WiFi.getMode() & WIFI_AP)
        dnsServer.processNextRequest();

    wifiLoop(); // non-blocking reconnect + async scan polling (REVIEW #6)
    apModeRetryLoop(); // fell back to the setup AP with valid credentials? keep trying STA

    if (globalVariables.started)
        discoveryLoop(); // non-blocking SSDP state machine (REVIEW #3)

    websocketLoop(); // 1 Hz status push + coalesced change pushes (section 7.5)

    // ---- deferred work requested by the other tasks ------------------------
    if (configDirty)
    {
        configDirty = false;
        STATE_LOCK();
        validateConfig();
        STATE_UNLOCK();
        saveConfig();
        ledDirty = true;
    }

    if (printerConfig.rescanWiFiNetwork)
    {
        printerConfig.rescanWiFiNetwork = false;
        LogSerial.println(F("[WiFi] Rescan requested (pin the strongest AP)"));
        wifiRescanStrongest();
    }

    if (printerStateDirty)
    {
        printerStateDirty = false;
        ledDirty = true;
        websocketNotifyChange(); // coalesced WebSocket push (api workstream)
    }

    // POST /api/config/reset -- the LittleFS write belongs to this task.
    if (factoryResetRequested)
    {
        factoryResetRequested = false;
        configDirty = false;
        deleteConfig();
        restartRequested = true;
        restartRequestMs = millis();
    }

    // ---- LEDs: evaluate on change / at >=10 Hz, then one PWM tick ----------
    ledLoop();

    if (restartRequested && (millis() - restartRequestMs) > 1500)
    {
        LogSerial.println(F("[System] Restarting now..."));
        Serial.flush();
        ESP.restart();
    }

    delay(1); // yield to the WiFi/async tasks; never a real wait
}
