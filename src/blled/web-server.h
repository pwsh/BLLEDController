#ifndef _BLLEDWEB_SERVER
#define _BLLEDWEB_SERVER

// ---------------------------------------------------------------------------
// web-server.h -- TRANSITIONAL. Minimal port of the upstream routes onto the v3
// state/config so the project builds and stays usable.  The API workstream
// replaces this file with api.h + a rewritten web-server.h (ARCHITECTURE.md §7);
// do not add features here.
//
// Already fixed while porting (cheap, in-file):
//   * OTA upload requires authentication (REVIEW #27)
//   * /configrestore checks auth in the upload handler at index == 0 and
//     validates the JSON before replacing the config (REVIEW #28)
//   * /config.json no longer echoes any password (REVIEW #29)
//   * the WiFi scan endpoint uses the asynchronous scan (REVIEW #5)
//   * mDNS failure no longer hangs the boot in `while(1) delay(500)` (REVIEW #16)
//
// Threading: all handlers run on the AsyncTCP task.  They only read/write the
// config structs and raise configDirty/ledDirty/restartRequested; LittleFS
// writes (except the restore upload, which streams to a temp file) and LED work
// happen in the main loop.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ESPAsyncWebServer.h>

#include "types.h"
#include "stages.h"
#include "filesystem.h"
#include "leds.h"
#include "wifi-manager.h"
#include "bblPrinterDiscovery.h"

AsyncWebServer webServer(80);
AsyncWebSocket ws("/ws");

#include "../www/www.h"

static unsigned long lastWsPush = 0;
static const unsigned long wsPushInterval = 1000;

static bool isApMode()
{
    return globalVariables.apMode || (WiFi.getMode() & WIFI_AP);
}

bool isAuthorized(AsyncWebServerRequest *request)
{
    if (isApMode())
        return true; // captive portal must stay reachable
    if (strlen(securityVariables.HTTPUser) == 0 || strlen(securityVariables.HTTPPass) == 0)
        return true;
    return request->authenticate(securityVariables.HTTPUser, securityVariables.HTTPPass);
}

static void sendGz(AsyncWebServerRequest *request, const uint8_t *data, size_t len, const char *mime)
{
    AsyncWebServerResponse *response = request->beginResponse(200, mime, data, len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

#define AUTH_OR_RETURN(req)                 \
    if (!isAuthorized(req))                 \
    {                                       \
        return req->requestAuthentication(); \
    }

// ---------------------------------------------------------------------------
// Static pages (asset names come from pre_build.py / src/www)
// ---------------------------------------------------------------------------
static void handleIndex(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    sendGz(request, index_html_gz, index_html_gz_len, index_html_gz_mime);
}

static void handleAppJs(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    sendGz(request, app_js_gz, app_js_gz_len, app_js_gz_mime);
}

static void handleGetIcon(AsyncWebServerRequest *request)
{
    sendGz(request, blled_svg_gz, blled_svg_gz_len, blled_svg_gz_mime);
}

static void handleGetFavicon(AsyncWebServerRequest *request)
{
    sendGz(request, favicon_png_gz, favicon_png_gz_len, favicon_png_gz_mime);
}

static void handleStyleCss(AsyncWebServerRequest *request)
{
    sendGz(request, style_css_gz, style_css_gz_len, style_css_gz_mime);
}

static void handleWiFiSetupPage(AsyncWebServerRequest *request)
{
    sendGz(request, wifiSetup_html_gz, wifiSetup_html_gz_len, wifiSetup_html_gz_mime);
}

static void handleWebSerialPage(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    sendGz(request, webSerialPage_html_gz, webSerialPage_html_gz_len, webSerialPage_html_gz_mime);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static void handleGetConfig(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);

    JsonDocument doc;
    STATE_LOCK();
    configToJson(doc);
    STATE_UNLOCK();

    // secrets are never echoed (REVIEW #29)
    doc["wifiPass"] = strlen(globalVariables.APPW) ? "********" : "";
    doc["webPass"] = strlen(securityVariables.HTTPPass) ? "********" : "";
    doc["mqttExtPass"] = strlen(printerConfig.mqttExtPass) ? "********" : "";

    // legacy aliases still used by the current setup page
    doc["firmwareversion"] = globalVariables.FWVersion;
    doc["wifiStrength"] = WiFi.RSSI();
    doc["ip"] = printerConfig.printerIP;
    doc["code"] = printerConfig.accessCode;
    doc["id"] = printerConfig.serialNumber;
    doc["apMAC"] = printerConfig.BSSID;

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// Old form post -> JSON -> configFromJson(). Booleans follow HTML checkbox
// semantics (absent == off), everything else is only applied when present.
static void handleSubmitConfig(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);

    JsonDocument doc;
    for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++)
    {
        const ConfigField &f = CONFIG_FIELDS[i];
        if (f.kind == K_COLOR)
        {
            char key[40];
            const char *suffix[3] = {"RGB", "WW", "CW"};
            for (int s = 0; s < 3; s++)
            {
                snprintf(key, sizeof(key), "%s%s", f.key, suffix[s]);
                if (request->hasParam(key, true))
                    doc[key] = request->getParam(key, true)->value();
            }
            continue;
        }
        if (f.kind == K_BOOL)
        {
            doc[f.key] = request->hasParam(f.key, true);
            continue;
        }
        if (request->hasParam(f.key, true))
            doc[f.key] = request->getParam(f.key, true)->value();
    }
    // upstream form field names
    if (request->hasParam("deviceName", true))
        doc["host"] = request->getParam("deviceName", true)->value();
    if (request->hasParam("brightnessslider", true))
        doc["brightness"] = request->getParam("brightnessslider", true)->value();
    if (request->hasParam("inactivityMins", true))
        doc["inactivityMins"] = request->getParam("inactivityMins", true)->value();

    String errors;
    STATE_LOCK();
    configFromJson(doc.as<JsonVariantConst>(), true, errors);
    printerConfig.rescanWiFiNetwork = request->hasParam("rescanWiFiNetwork", true);
    STATE_UNLOCK();

    configDirty = true;
    ledDirty = true;
    request->send(200, "text/plain", "OK");
}

static void handlePrinterConfigJson(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    JsonDocument doc;
    doc["ssid"] = globalVariables.SSID;
    doc["pass"] = strlen(globalVariables.APPW) ? "********" : "";
    doc["host"] = globalVariables.Host;
    doc["printerIP"] = printerConfig.printerIP;
    doc["printerSerial"] = printerConfig.serialNumber;
    doc["accessCode"] = printerConfig.accessCode;
    doc["webUser"] = securityVariables.HTTPUser;
    doc["webPass"] = strlen(securityVariables.HTTPPass) ? "********" : "";
    doc["isAPMode"] = isApMode();

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handleSubmitWiFi(AsyncWebServerRequest *request)
{
    auto param = [request](const char *name) -> String
    {
        return request->hasParam(name, true) ? request->getParam(name, true)->value() : String("");
    };

    String ssid = param("ssid");
    String pass = param("pass");
    ssid.trim();
    pass.trim();
    if (ssid.length() > 0)
        strlcpy(globalVariables.SSID, ssid.c_str(), sizeof(globalVariables.SSID));
    if (pass.length() > 0 && pass != "********")
        strlcpy(globalVariables.APPW, pass.c_str(), sizeof(globalVariables.APPW));

    String bssidStr = param("bssid");
    if (bssidStr.length() > 0)
        strlcpy(printerConfig.BSSID, bssidStr.c_str(), sizeof(printerConfig.BSSID));

    String host = param("host");
    host.trim();
    if (host.length() > 0)
        strlcpy(globalVariables.Host, host.c_str(), sizeof(globalVariables.Host));

    String printerIP = param("printerIP");
    if (printerIP.length() > 0)
        strlcpy(printerConfig.printerIP, printerIP.c_str(), sizeof(printerConfig.printerIP));
    String printerSerial = param("printerSerial");
    if (printerSerial.length() > 0)
        strlcpy(printerConfig.serialNumber, printerSerial.c_str(), sizeof(printerConfig.serialNumber));
    String accessCode = param("accessCode");
    if (accessCode.length() > 0)
        strlcpy(printerConfig.accessCode, accessCode.c_str(), sizeof(printerConfig.accessCode));

    String webUser = param("webUser");
    String webPass = param("webPass");
    if (request->hasParam("webUser", true))
        strlcpy(securityVariables.HTTPUser, webUser.c_str(), sizeof(securityVariables.HTTPUser));
    if (request->hasParam("webPass", true) && webPass != "********")
        strlcpy(securityVariables.HTTPPass, webPass.c_str(), sizeof(securityVariables.HTTPPass));

    STATE_LOCK();
    validateConfig();
    STATE_UNLOCK();
    configDirty = true;

    request->send(200, "text/plain", "Settings saved, restarting...");
    restartRequested = true;
    restartRequestMs = millis();
}

static void handleDownloadConfigFile(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);

    JsonDocument doc;
    STATE_LOCK();
    configToJson(doc); // a backup deliberately contains the secrets
    STATE_UNLOCK();

    String jsonString;
    serializeJsonPretty(doc, jsonString);
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", jsonString);
    response->addHeader("Content-Disposition", "attachment; filename=\"blledconfig.json\"");
    request->send(response);
}

static void handleFactoryReset(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    LogSerial.println(F("[FactoryReset] Deleting configuration"));
    deleteConfig();
    request->send(200, "text/plain", "Factory reset complete. Restarting...");
    restartRequested = true;
    restartRequestMs = millis();
}

// ---------------------------------------------------------------------------
// WiFi scan / printer list
// ---------------------------------------------------------------------------
static void handleWiFiScan(AsyncWebServerRequest *request)
{
    JsonDocument doc;
    wifiScanResultsJson(doc); // asynchronous: {"scanning":true} until ready
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

static void handlePrinterList(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    discoveryRequest();
    JsonDocument doc;
    discoveryListJson(doc);
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// WebSocket (transitional payload; §7.1 lands with api.h)
// ---------------------------------------------------------------------------
void sendJsonToAll(JsonDocument &doc)
{
    String jsonString;
    serializeJson(doc, jsonString);
    ws.textAll(jsonString);
}

void websocketLoop()
{
    if (ws.count() == 0)
        return;
    if (millis() - lastWsPush <= wsPushInterval)
        return;
    lastWsPush = millis();

    PrinterState snapshot;
    STATE_LOCK();
    memcpy(&snapshot, &printerState, sizeof(PrinterState));
    STATE_UNLOCK();

    JsonDocument doc;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime"] = millis() / 1000;
    doc["doorOpen"] = snapshot.doorOpen;
    doc["printerConnection"] = snapshot.online;
    doc["clients"] = ws.count();
    doc["stg_cur"] = snapshot.stage;
    doc["stageName"] = stageName(snapshot.stage);
    doc["gcodeState"] = snapshot.gcodeState;
    doc["progress"] = snapshot.progress;
    doc["ledReason"] = ledRuntime.reason;
    sendJsonToAll(doc);
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                      void *arg, uint8_t *data, size_t len)
{
    (void)server;
    (void)arg;
    (void)data;
    (void)len;
    switch (type)
    {
    case WS_EVT_CONNECT:
        LogSerial.printf("[WS] Client connected: %u\n", client->id());
        break;
    case WS_EVT_DISCONNECT:
    case WS_EVT_ERROR:
        ws.cleanupClients();
        break;
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

    LogSerial.println(F("Setting up webserver"));

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
        if (isApMode())
            request->redirect("/wifi");
        else
            handleIndex(request); });
    webServer.on("/index.html", HTTP_GET, handleIndex);
    webServer.on("/app.js", HTTP_GET, handleAppJs);
    webServer.on("/getConfig", HTTP_GET, handleGetConfig);
    webServer.on("/submitConfig", HTTP_POST, handleSubmitConfig);
    webServer.on("/blled.svg", HTTP_GET, handleGetIcon);
    webServer.on("/favicon.ico", HTTP_GET, handleGetFavicon);
    webServer.on("/config.json", HTTP_GET, handlePrinterConfigJson);
    webServer.on("/wifi", HTTP_GET, handleWiFiSetupPage);
    webServer.on("/wifiScan", HTTP_GET, handleWiFiScan);
    webServer.on("/submitWiFi", HTTP_POST, handleSubmitWiFi);
    webServer.on("/style.css", HTTP_GET, handleStyleCss);
    webServer.on("/configfile.json", HTTP_GET, handleDownloadConfigFile);
    webServer.on("/webserial", HTTP_GET, handleWebSerialPage);
    webServer.on("/printerList", HTTP_GET, handlePrinterList);
    webServer.on("/factoryreset", HTTP_GET, handleFactoryReset);
    webServer.on("/factoryreset", HTTP_POST, handleFactoryReset);

    // ---- config restore: stream to a temp file, validate, then swap --------
    webServer.on(
        "/configrestore", HTTP_POST,
        [](AsyncWebServerRequest *request)
        {
            AUTH_OR_RETURN(request);
            File f = LittleFS.open(configTmpPath, "r");
            bool ok = false;
            if (f)
            {
                size_t size = f.size();
                if (size > 0 && size < 32768)
                {
                    std::unique_ptr<char[]> buf(new (std::nothrow) char[size + 1]);
                    if (buf)
                    {
                        size_t read = f.readBytes(buf.get(), size);
                        buf[read] = '\0';
                        ok = validateConfigJson(buf.get(), read);
                    }
                }
                f.close();
            }
            if (!ok)
            {
                LittleFS.remove(configTmpPath);
                request->send(400, "application/json", "{\"error\":\"not a valid BLLED configuration\"}");
                return;
            }
            LittleFS.remove(configPath);
            LittleFS.rename(configTmpPath, configPath);
            request->send(200, "application/json", "{\"ok\":true,\"restartRequired\":true}");
            restartRequested = true;
            restartRequestMs = millis();
        },
        [](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
        {
            static File uploadFile;
            if (index == 0)
            {
                // REVIEW #28: authenticate BEFORE the first byte is written.
                if (!isAuthorized(request))
                {
                    request->requestAuthentication();
                    return;
                }
                LogSerial.printf("[ConfigUpload] Start: %s\n", filename.c_str());
                uploadFile = LittleFS.open(configTmpPath, "w");
            }
            if (uploadFile)
                uploadFile.write(data, len);
            if (final && uploadFile)
                uploadFile.close();
        });

    // ---- OTA ---------------------------------------------------------------
    webServer.on(
        "/update", HTTP_POST,
        [](AsyncWebServerRequest *request)
        {
            AUTH_OR_RETURN(request);
            bool ok = !Update.hasError();
            request->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"update failed\"}");
            if (ok)
            {
                restartRequested = true;
                restartRequestMs = millis();
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
        {
            if (index == 0)
            {
                // REVIEW #27: OTA was completely unauthenticated upstream.
                if (!isAuthorized(request))
                {
                    request->requestAuthentication();
                    return;
                }
                LogSerial.printf("[OTA] Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN))
                    Update.printError(LogSerial);
            }
            if (Update.isRunning() && Update.write(data, len) != len)
                Update.printError(LogSerial);
            if (final && Update.isRunning())
            {
                if (Update.end(true))
                    LogSerial.printf("[OTA] Success (%u bytes)\n", (unsigned)(index + len));
                else
                    Update.printError(LogSerial);
            }
        });

    LogSerial.begin(&webServer);

    ws.onEvent(onWsEvent);
    webServer.addHandler(&ws);
    webServer.begin();

    LogSerial.println(F("Webserver started"));
}

#endif
