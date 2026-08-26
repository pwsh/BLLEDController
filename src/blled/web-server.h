#ifndef _BLLEDWEB_SERVER
#define _BLLEDWEB_SERVER

// ---------------------------------------------------------------------------
// web-server.h -- HTTP server wiring: static assets, legacy aliases, the
// captive portal, the WebSocket and the /api/* routes from api.h
// (ARCHITECTURE.md section 7).
//
// Security (docs/REVIEW.md #27-#31):
//   * every route is behind isAuthorized() (HTTP Basic when webUser+webPass are
//     set and the device is not in AP mode) -- static assets, OTA, restore and
//     the WebSocket handshake included
//   * /submitConfig, /submitWiFi, GET /factoryreset and /config.json are GONE
//     (the last two were CSRF / plaintext-secret holes)
//   * X-Content-Type-Options: nosniff on every response
//
// Threading: every handler runs on the AsyncTCP task; see api.h.  The
// WebSocket push runs from the main loop via websocketLoop().
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>

#include "types.h"
#include "stages.h"
#include "logSerial.h"
#include "filesystem.h"
#include "leds.h"
#include "wifi-manager.h"
#include "bblPrinterDiscovery.h"
#include "api.h"
#include "mqttpublish.h"

AsyncWebServer webServer(80);
AsyncWebSocket ws("/ws");

#include "../www/www.h"

// ---- WebSocket push cadence (section 7.5) ---------------------------------
#define WS_PUSH_INTERVAL_MS 1000UL
#define WS_COALESCE_MS 200UL
#define WS_CLEANUP_INTERVAL_MS 5000UL
#define WS_MAX_CLIENTS 4

static unsigned long wsLastPushMs = 0;
static unsigned long wsLastCleanupMs = 0;
static volatile bool wsChangePending = false;
static uint8_t wsLastOutput[5] = {0, 0, 0, 0, 0};

// ---------------------------------------------------------------------------
// Static assets
// ---------------------------------------------------------------------------

// Long-lived assets are versioned by the firmware they are baked into, so a
// short max-age plus revalidation on reload is the right trade for a device
// that gets OTA updates.
static void sendGz(AsyncWebServerRequest *request, const uint8_t *data, size_t len, const char *mime,
                   const char *cacheControl = "public, max-age=3600")
{
    AsyncWebServerResponse *response = request->beginResponse(200, mime, data, len);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", cacheControl);
    request->send(response);
}

#define STATIC_ROUTE(uri, sym, cache)                                              \
    webServer.on(AsyncURIMatcher::exact(uri), HTTP_GET,                            \
                 [](AsyncWebServerRequest *request)                                \
                 {                                                                 \
                     AUTH_OR_RETURN(request);                                      \
                     sendGz(request, sym##_gz, sym##_gz_len, sym##_gz_mime, cache); \
                 })

static void handleIndex(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    sendGz(request, index_html_gz, index_html_gz_len, index_html_gz_mime, "no-cache");
}

static void handleWiFiSetupPage(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    sendGz(request, wifiSetup_html_gz, wifiSetup_html_gz_len, wifiSetup_html_gz_mime, "no-cache");
}

// ---------------------------------------------------------------------------
// Captive portal (AP mode)
//
// Phones and laptops decide whether to pop the "sign in to network" prompt by
// fetching a well-known probe URL right after joining a WiFi network and
// checking for an exact expected answer. Anything else - including a redirect -
// means "captive", so answering every probe with a 302 to the setup page both
// triggers the prompt and lands the user on /wifi. The DNS server started in
// startAPMode() resolves every name to the AP address, so the probe requests
// reach us in the first place. HTTPS probes cannot be intercepted (by design).
// ---------------------------------------------------------------------------
static const char *const CAPTIVE_PROBES[] = {
    "/generate_204", "/gen_204",                          // Android, Chrome OS
    "/hotspot-detect.html", "/library/test/success.html", // iOS / macOS
    "/connecttest.txt", "/ncsi.txt", "/redirect",         // Windows
    "/canonical.html", "/success.txt",                    // Firefox
    "/check_network_status.txt", "/nm-check.txt",         // NetworkManager / KDE
};

// Absolute URL: captive-portal mini browsers do not always resolve names, and
// a relative Location is resolved against whatever host the probe used.
static void captiveRedirect(AsyncWebServerRequest *request)
{
    String url = String("http://") + WiFi.softAPIP().toString() + "/wifi";
    AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
    response->addHeader("Location", url);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

// ---------------------------------------------------------------------------
// Legacy aliases (section 7).  Kept so old bookmarks/scripts keep working.
// ---------------------------------------------------------------------------
static void handleLegacyGetConfig(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    JsonDocument doc;
    buildConfigJson(doc, false);
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

// ---------------------------------------------------------------------------
// WebSocket (section 7.5)
// ---------------------------------------------------------------------------

// Any task may call this; it only sets a flag that websocketLoop() consumes.
void websocketNotifyChange()
{
    wsChangePending = true;
}

static void wsPushStatus()
{
    JsonDocument doc;
    buildStatusJson(doc);
    String out;
    out.reserve(measureJson(doc) + 1);
    serializeJson(doc, out);
    ws.textAll(out);
}

// Called from the main loop.  Pushes at 1 Hz while clients are connected, plus
// immediately (coalesced to >= 200 ms) when the printer state or the LED output
// changed.
void websocketLoop()
{
    const unsigned long now = millis();

    if (now - wsLastCleanupMs >= WS_CLEANUP_INTERVAL_MS)
    {
        wsLastCleanupMs = now;
        ws.cleanupClients(WS_MAX_CLIENTS);
    }

    // The LED engine does not know about the WebSocket; watch its output here.
    if (memcmp(wsLastOutput, ledRuntime.output, sizeof(wsLastOutput)) != 0)
    {
        memcpy(wsLastOutput, ledRuntime.output, sizeof(wsLastOutput));
        wsChangePending = true;
    }

    if (ws.count() == 0)
    {
        wsChangePending = false;
        return;
    }

    if (wsChangePending && (now - wsLastPushMs) >= WS_COALESCE_MS)
    {
        wsChangePending = false;
        wsLastPushMs = now;
        wsPushStatus();
        return;
    }

    if ((now - wsLastPushMs) >= WS_PUSH_INTERVAL_MS)
    {
        wsLastPushMs = now;
        wsPushStatus();
    }
}

// Client -> server commands (section 7.5).
static void wsHandleCommand(const char *payload, size_t len)
{
    JsonDocument doc;
    if (deserializeJson(doc, payload, len))
        return;
    const char *cmd = doc["cmd"].as<const char *>();
    if (cmd == NULL)
        return;

    if (!strcmp(cmd, "clearLed"))
    {
        ledClearOverride();
        wsChangePending = true;
        return;
    }
    if (strcmp(cmd, "led") != 0)
        return;

    COLOR c;
    const char *hex = doc["hex"].as<const char *>();
    if (hex != NULL && hex[0] == '#' && strlen(hex) == 7)
        c = hex2rgb(hex, 0, 0);
    else
    {
        c.r = (short)constrain((int)(doc["r"] | 0), 0, 255);
        c.g = (short)constrain((int)(doc["g"] | 0), 0, 255);
        c.b = (short)constrain((int)(doc["b"] | 0), 0, 255);
        snprintf(c.RGBhex, sizeof(c.RGBhex), "#%02x%02x%02x", c.r, c.g, c.b);
    }
    c.ww = (short)constrain((int)(doc["ww"] | 0), 0, 255);
    c.cw = (short)constrain((int)(doc["cw"] | 0), 0, 255);

    LedEffect effect = ledEffectFromString(doc["effect"].as<const char *>(), LedEffect::Solid);
    uint32_t durationMs = (uint32_t)constrain((int32_t)(doc["durationSec"] | 0), (int32_t)0, (int32_t)86400) * 1000UL;
    int8_t brightness = -1;
    if (doc["brightness"].is<int>())
        brightness = (int8_t)constrain(doc["brightness"].as<int>(), 0, 100);

    ledRequestOverride(c, effect, durationMs, brightness);
    wsChangePending = true;
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                      void *arg, uint8_t *data, size_t len)
{
    (void)server;
    switch (type)
    {
    case WS_EVT_CONNECT:
        if (printerConfig.debugChanges)
            LogSerial.printf("[WS] Client %u connected\n", client->id());
        wsChangePending = true;
        break;

    case WS_EVT_DISCONNECT:
    case WS_EVT_ERROR:
        break;

    case WS_EVT_DATA:
    {
        AwsFrameInfo *info = (AwsFrameInfo *)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT && len < 512)
            wsHandleCommand((const char *)data, len);
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Server setup
// ---------------------------------------------------------------------------
void setupWebserver()
{
    // REVIEW #16: never hang the boot when mDNS cannot start.
    if (!MDNS.begin(globalVariables.Host))
        LogSerial.println(F("[MDNS] Responder failed to start (continuing)"));
    else
        MDNS.addService("http", "tcp", 80);

    LogSerial.println(F("[Web] Setting up the web server"));

    DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");

    // ---- /api/* ------------------------------------------------------------
    registerApiRoutes(webServer);

    // ---- static assets -----------------------------------------------------
    webServer.on(AsyncURIMatcher::exact("/"), HTTP_GET, [](AsyncWebServerRequest *request)
                 {
        if (isApMode())
            return captiveRedirect(request);
        handleIndex(request); });
    webServer.on(AsyncURIMatcher::exact("/index.html"), HTTP_GET, handleIndex);
    webServer.on(AsyncURIMatcher::exact("/wifi"), HTTP_GET, handleWiFiSetupPage);
    webServer.on(AsyncURIMatcher::exact("/wifiSetup.html"), HTTP_GET, handleWiFiSetupPage);

    STATIC_ROUTE("/app.js", app_js, "public, max-age=3600");
    STATIC_ROUTE("/style.css", style_css, "public, max-age=3600");
    STATIC_ROUTE("/blled.svg", blled_svg, "public, max-age=86400");
    STATIC_ROUTE("/favicon.png", favicon_png, "public, max-age=86400");
    STATIC_ROUTE("/favicon.ico", favicon_png, "public, max-age=86400");

    // The WebSerial page must be registered before LogSerial.begin(), which
    // installs the library's own (plain, unauthenticated) page on /webserial.
    STATIC_ROUTE("/webserial", webSerialPage_html, "no-cache");

    // ---- legacy aliases (section 7) ---------------------------------------
    webServer.on(AsyncURIMatcher::exact("/getConfig"), HTTP_GET, handleLegacyGetConfig);
    webServer.on(AsyncURIMatcher::exact("/configfile.json"), HTTP_GET, handleApiConfigBackup);
    webServer.on(AsyncURIMatcher::exact("/printerList"), HTTP_GET, handleApiPrinters);
    webServer.on(AsyncURIMatcher::exact("/update"), HTTP_POST, handleApiUpdateDone, handleApiUpdateUpload);
    webServer.on(AsyncURIMatcher::exact("/configrestore"), HTTP_POST, handleApiRestoreDone, handleApiRestoreUpload);

    // ---- captive-portal probes (only meaningful in AP mode) ----------------
    for (size_t i = 0; i < sizeof(CAPTIVE_PROBES) / sizeof(CAPTIVE_PROBES[0]); i++)
    {
        webServer.on(AsyncURIMatcher::exact(CAPTIVE_PROBES[i]), HTTP_ANY, [](AsyncWebServerRequest *request)
                     {
            if (isApMode())
                return captiveRedirect(request);
            request->send(404, "text/plain", "not found"); });
    }

    // ---- WebSocket ---------------------------------------------------------
    // REVIEW #29: the handshake goes through the same auth as every other route.
    ws.handleHandshake([](AsyncWebServerRequest *request) -> bool
                       { return isAuthorized(request); });
    ws.onEvent(onWsEvent);
    webServer.addHandler(&ws);

    // ---- 404 / captive portal ---------------------------------------------
    webServer.onNotFound([](AsyncWebServerRequest *request)
                         {
        if (request->method() == HTTP_OPTIONS)
            return request->send(204);
        if (request->url().startsWith("/api/"))
            return apiError(request, 404, "no such endpoint");
        if (isApMode())
            return captiveRedirect(request); // captive-portal catch-all
        request->send(404, "application/json", "{\"error\":\"not found\"}"); });

    // The WebSerial log socket (/webserialws) is owned by the library; give it
    // the same Basic credentials as every other route (open in AP mode).
    if (!isApMode() && strlen(securityVariables.HTTPUser) > 0 && strlen(securityVariables.HTTPPass) > 0)
        LogSerial.setAuthentication(securityVariables.HTTPUser, securityVariables.HTTPPass);
    LogSerial.begin(&webServer);

    webServer.begin();
    LogSerial.println(F("[Web] Web server started"));
}

#endif
