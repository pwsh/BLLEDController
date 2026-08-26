#ifndef _BLLED_API
#define _BLLED_API

// ---------------------------------------------------------------------------
// api.h -- the /api/* REST surface and the JSON model shared by REST, the
// WebSocket and the external MQTT broker (ARCHITECTURE.md section 7).
//
// Public model builders (re-entrant: locals only, no static JsonDocument, the
// lock is never held while serialising):
//   buildStatusJson(JsonDocument &)                 -- 7.1
//   buildLedJson(JsonDocument &)                    -- the "led" sub-object
//   buildConfigJson(JsonDocument &, bool secrets)   -- 7.2
//   applyConfigJson(JsonVariantConst, String &errors, bool &restartRequired)
//   registerApiRoutes(AsyncWebServer &)
//
// Threading (ARCHITECTURE.md section 2): every handler runs on the AsyncTCP
// task and may only read/write the shared structs under STATE_LOCK(), raise
// configDirty / ledDirty / restartRequested / factoryResetRequested, or call
// mqttEnqueue().  Handlers never touch the LED hardware and never talk to
// PubSubClient.  The one exception to "no LittleFS from the async task" is the
// config-restore upload, which must stream the body to a temp file as it
// arrives; every other filesystem write happens in the main loop.
//
// buildStatusJson() is also called from the main loop (WebSocket push) and from
// the MQTT task (external broker publish), so it must stay re-entrant.
//
// Deviation from ARCHITECTURE.md section 7.4: GET /api/stages is not
// implemented (dropped from the contract by the orchestrator -- no consumer;
// stage names already arrive as status.printer.stageName).
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Update.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>

#include "types.h"
#include "stages.h"
#include "logSerial.h"
#include "filesystem.h"
#include "leds.h"
#include "mqttparsingutility.h"
#include "wifi-manager.h"
#include "bblPrinterDiscovery.h"

#ifndef STRCODENAME
#define STRCODENAME "Balder"
#endif

// Defined in mqttpublish.h (same translation unit, included after this file).
bool extMqttConnected();
int extMqttState();

// Defined in web-server.h (same translation unit).
void websocketNotifyChange();

// Raised by POST /api/config/reset, handled by the main loop (LittleFS writes
// belong to the main loop -- ARCHITECTURE.md section 2).
volatile bool factoryResetRequested = false;

// Body limit for the JSON endpoints (a full config PUT is ~1.6 kB).
#define API_JSON_BODY_MAX 8192

// ---------------------------------------------------------------------------
// Auth (section 7: every route; AP mode open; REVIEW #27/#28/#29)
// ---------------------------------------------------------------------------

static bool isApMode()
{
    return globalVariables.apMode || (WiFi.getMode() & WIFI_AP);
}

bool isAuthorized(AsyncWebServerRequest *request)
{
    if (isApMode())
        return true; // the captive portal must stay reachable
    if (strlen(securityVariables.HTTPUser) == 0 || strlen(securityVariables.HTTPPass) == 0)
        return true;
    return request->authenticate(securityVariables.HTTPUser, securityVariables.HTTPPass);
}

#define AUTH_OR_RETURN(req)                  \
    if (!isAuthorized(req))                  \
    {                                        \
        return req->requestAuthentication(); \
    }

// ---------------------------------------------------------------------------
// Small response helpers
// ---------------------------------------------------------------------------

static void apiError(AsyncWebServerRequest *request, int code, const char *message)
{
    JsonDocument doc;
    doc["error"] = message;
    String out;
    serializeJson(doc, out);
    request->send(code, "application/json", out);
}

static void apiSendDoc(AsyncWebServerRequest *request, JsonDocument &doc)
{
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

static void apiOk(AsyncWebServerRequest *request)
{
    request->send(200, "application/json", "{\"ok\":true}");
}

// "#rrggbb", exactly.
static bool apiHexIsValid(const char *s)
{
    if (s == NULL || s[0] != '#' || strlen(s) != 7)
        return false;
    for (int i = 1; i < 7; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}

static bool apiModeIsKnown(const char *s)
{
    if (s == NULL)
        return false;
    return !strcasecmp(s, "auto") || !strcasecmp(s, "maintenance") || !strcasecmp(s, "test") ||
           !strcasecmp(s, "rainbow") || !strcasecmp(s, "wifi") || !strcasecmp(s, "off");
}

static bool apiEffectIsKnown(const char *s)
{
    if (s == NULL)
        return false;
    return !strcasecmp(s, "solid") || !strcasecmp(s, "breathe") || !strcasecmp(s, "blink") ||
           !strcasecmp(s, "fastblink") || !strcasecmp(s, "rainbow");
}

// ---------------------------------------------------------------------------
// Snapshot of the config fields the status model needs.  Copied under the lock
// so that the (slow) JSON building below runs unlocked.
// ---------------------------------------------------------------------------
struct ApiCfgSnapshot
{
    char host[33];
    char printerIP[16];
    char serialNumber[16];
    int brightness;
    LedMode ledMode;
    bool inactivityEnabled;
    uint16_t inactivityMins;
    uint16_t finishTimerMins;
    FinishExitMode finishExitMode;
    bool mqttExtEnabled;
};

static void apiSnapshot(PrinterState &st, LedRuntime &lr, ApiCfgSnapshot &cfg)
{
    STATE_LOCK();
    memcpy(&st, &printerState, sizeof(PrinterState));
    memcpy(&lr, &ledRuntime, sizeof(LedRuntime));
    strlcpy(cfg.host, globalVariables.Host, sizeof(cfg.host));
    strlcpy(cfg.printerIP, printerConfig.printerIP, sizeof(cfg.printerIP));
    strlcpy(cfg.serialNumber, printerConfig.serialNumber, sizeof(cfg.serialNumber));
    cfg.brightness = printerConfig.brightness;
    cfg.ledMode = printerConfig.ledMode;
    cfg.inactivityEnabled = printerConfig.inactivityEnabled;
    cfg.inactivityMins = printerConfig.inactivityMins;
    cfg.finishTimerMins = printerConfig.finishTimerMins;
    cfg.finishExitMode = printerConfig.finishExitMode;
    cfg.mqttExtEnabled = printerConfig.mqttExtEnabled;
    STATE_UNLOCK();
}

// NAN -> JSON null.  HA templates guard these with | default(0),
// see docs/HA-DISCOVERY.md section 3d.
static void apiSetTemp(JsonObject obj, const char *key, float v)
{
    if (isnan(v))
        obj[key] = nullptr;
    else
        obj[key] = roundf(v * 10.0f) / 10.0f;
}

// ---------------------------------------------------------------------------
// The "led" sub-object (7.1) -- also published on <base>/led.
// ---------------------------------------------------------------------------
static void apiFillLed(JsonObject led, const LedRuntime &lr, const ApiCfgSnapshot &cfg, unsigned long now)
{
    led["mode"] = ledModeToString(cfg.ledMode);
    led["r"] = lr.output[0];
    led["g"] = lr.output[1];
    led["b"] = lr.output[2];
    led["ww"] = lr.output[3];
    led["cw"] = lr.output[4];
    led["brightness"] = cfg.brightness;
    led["effect"] = ledEffectToString(lr.effect);
    // lr is a stack snapshot: ArduinoJson keeps const char* by pointer, so copy.
    led["reason"] = String(lr.reason);
    led["override"] = lr.overrideActive;
    uint32_t remain = 0;
    if (lr.overrideActive && lr.overrideUntilMs != 0 && (long)(lr.overrideUntilMs - now) > 0)
        remain = (uint32_t)((lr.overrideUntilMs - now) / 1000UL);
    led["overrideRemainingSec"] = remain;
    led["identify"] = lr.identifyRequested;
}

void buildLedJson(JsonDocument &doc)
{
    PrinterState st;
    LedRuntime lr;
    ApiCfgSnapshot cfg;
    apiSnapshot(st, lr, cfg);
    apiFillLed(doc.to<JsonObject>(), lr, cfg, millis());
}

// ---------------------------------------------------------------------------
// GET /api/status (7.1)
// ---------------------------------------------------------------------------
void buildStatusJson(JsonDocument &doc)
{
    PrinterState st;
    LedRuntime lr;
    ApiCfgSnapshot cfg;
    apiSnapshot(st, lr, cfg);

    const unsigned long now = millis();
    const bool ap = isApMode();

    // ---- device ------------------------------------------------------------
    JsonObject dev = doc["device"].to<JsonObject>();
    dev["fw"] = globalVariables.FWVersion;
    dev["host"] = cfg.host;
    dev["ip"] = (ap ? WiFi.softAPIP() : WiFi.localIP()).toString();
    dev["mac"] = WiFi.macAddress();
    dev["rssi"] = ap ? 0 : (int)WiFi.RSSI();
    dev["uptimeSec"] = (uint32_t)(now / 1000UL);
    dev["heapFree"] = (uint32_t)ESP.getFreeHeap();
    dev["heapMin"] = (uint32_t)ESP.getMinFreeHeap();
    dev["apMode"] = ap;
    char mdns[40];
    snprintf(mdns, sizeof(mdns), "%s.local", cfg.host);
    dev["mdns"] = mdns;
    dev["chip"] = ESP.getChipModel();
    dev["sdk"] = ESP.getSdkVersion();

    // ---- printer -----------------------------------------------------------
    JsonObject pr = doc["printer"].to<JsonObject>();
    pr["connected"] = st.online;
    pr["ip"] = cfg.printerIP;
    pr["serial"] = cfg.serialNumber;
    pr["model"] = st.model;
    pr["fw"] = st.printerFw;
    pr["lastReportSec"] = (uint32_t)((st.lastReportMs == 0) ? 0 : ((now - st.lastReportMs) / 1000UL));
    pr["gcodeState"] = st.gcodeState;
    pr["stage"] = st.stage;
    pr["stageName"] = stageName(st.stage);
    pr["overrideStage"] = st.overrideStage;
    pr["progress"] = st.progress;
    pr["remainingMin"] = st.remainingMin;
    pr["layer"] = st.layer;
    pr["totalLayers"] = st.totalLayers;
    apiSetTemp(pr, "nozzleTemp", st.nozzleTemp);
    apiSetTemp(pr, "nozzleTarget", st.nozzleTarget);
    apiSetTemp(pr, "bedTemp", st.bedTemp);
    apiSetTemp(pr, "bedTarget", st.bedTarget);
    apiSetTemp(pr, "chamberTemp", st.chamberTemp);
    pr["fanPart"] = st.fanPart;
    pr["fanAux"] = st.fanAux;
    pr["fanChamber"] = st.fanChamber;
    pr["fanHeatbreak"] = st.fanHeatbreak;
    pr["chamberLight"] = st.chamberLight;
    pr["workLight"] = st.workLight;
    pr["doorOpen"] = st.doorOpen;
    pr["doorKnown"] = (st.doorEdgeCount > 0); // some X1C firmware never reports the door (bit 23 stuck)
    pr["sdcard"] = st.sdcard;
    pr["speedLevel"] = st.speedLevel;
    pr["jobName"] = st.jobName;
    pr["printType"] = st.printType;
    pr["printError"] = st.printError;
    pr["wifiSignal"] = st.wifiSignal;

    JsonObject ams = pr["ams"].to<JsonObject>();
    ams["present"] = st.amsPresent;
    ams["trayNow"] = st.amsTrayNow;
    ams["trayColor"] = st.amsTrayColor;
    ams["humidity"] = st.amsHumidity;

    JsonArray hmsArr = pr["hms"].to<JsonArray>();
    for (uint8_t i = 0; i < st.hmsCount && i < HMS_MAX; i++)
    {
        char code[26];
        hmsFormatCode(st.hms[i].code, code, sizeof(code));
        JsonObject e = hmsArr.add<JsonObject>();
        e["code"] = code;
        e["severity"] = hmsSeverityName(st.hms[i].severity);
        e["module"] = hmsModuleName(st.hms[i].module);
        e["ignored"] = st.hms[i].ignored;
    }
    pr["hmsHighest"] = hmsSeverityName(st.hmsHighestSeverity);

    // ---- led ---------------------------------------------------------------
    apiFillLed(doc["led"].to<JsonObject>(), lr, cfg, now);

    // ---- timers ------------------------------------------------------------
    JsonObject tm = doc["timers"].to<JsonObject>();
    tm["finishActive"] = lr.finishActive;
    uint32_t finishRemain = 0;
    if (lr.finishActive && cfg.finishExitMode == FinishExitMode::Timer)
    {
        unsigned long total = (unsigned long)cfg.finishTimerMins * 60000UL;
        unsigned long elapsed = now - lr.finishStartMs;
        finishRemain = (elapsed >= total) ? 0 : (uint32_t)((total - elapsed) / 1000UL);
    }
    tm["finishRemainingSec"] = finishRemain;
    uint32_t inactRemain = 0;
    if (cfg.inactivityEnabled && !lr.idleOffActive && cfg.inactivityMins > 0)
    {
        unsigned long total = (unsigned long)cfg.inactivityMins * 60000UL;
        unsigned long elapsed = now - lr.inactivityStartMs;
        inactRemain = (elapsed >= total) ? 0 : (uint32_t)((total - elapsed) / 1000UL);
    }
    tm["inactivityRemainingSec"] = inactRemain;
    tm["idleOff"] = lr.idleOffActive;
    tm["doorToggleOff"] = lr.doorToggleOff;

    // ---- mqtt --------------------------------------------------------------
    JsonObject mq = doc["mqtt"].to<JsonObject>();
    JsonObject mqp = mq["printer"].to<JsonObject>();
    mqp["connected"] = st.online;
    mqp["state"] = st.mqttState;
    mqp["stateText"] = mqttStateText(st.mqttState);
    mqp["reconnects"] = st.reconnects;
    JsonObject mqe = mq["external"].to<JsonObject>();
    int extState = extMqttState();
    mqe["enabled"] = cfg.mqttExtEnabled;
    mqe["connected"] = extMqttConnected();
    mqe["state"] = extState;
    mqe["stateText"] = mqttStateText(extState);
}

// ---------------------------------------------------------------------------
// Config (7.2)
// ---------------------------------------------------------------------------

#define API_SECRET_COUNT 3
#define API_SECRET_MASK "********"
static const char *const API_SECRET_KEYS[API_SECRET_COUNT] = {"wifiPass", "webPass", "mqttExtPass"};

// Keys that only take effect after a restart (7.2).
#define API_NETWORK_COUNT 9
static const char *const API_NETWORK_KEYS[API_NETWORK_COUNT] = {
    "wifiSSID", "wifiPass", "BSSID", "host", "printerIP", "serialNumber", "accessCode", "webUser", "webPass"};

// Longest of those values is wifiPass (65 bytes incl. NUL).
#define API_NET_VALUE_MAX 66

static char *apiStringFieldPtr(const char *key)
{
    char part = 0;
    const ConfigField *f = configFindField(key, &part);
    return (f != NULL && f->kind == K_STR) ? (char *)f->ptr : NULL;
}

void buildConfigJson(JsonDocument &doc, bool includeSecrets)
{
    STATE_LOCK();
    configToJson(doc);
    STATE_UNLOCK();

    if (includeSecrets)
        return; // a backup deliberately contains everything

    for (int i = 0; i < API_SECRET_COUNT; i++)
    {
        const char *v = doc[API_SECRET_KEYS[i]].as<const char *>();
        doc[API_SECRET_KEYS[i]] = (v != NULL && v[0] != '\0') ? API_SECRET_MASK : "";
    }
}

// Partial merge.  Secrets: absent or "********" = unchanged, "" = clear.
// `restartRequired` is set when one of the network keys actually changed value.
bool applyConfigJson(JsonVariantConst v, String &errors, bool &restartRequired)
{
    restartRequired = false;

    JsonObjectConst obj = v.as<JsonObjectConst>();
    if (obj.isNull())
    {
        errors = F("expected a JSON object");
        return false;
    }

    char before[API_NETWORK_COUNT][API_NET_VALUE_MAX];
    char keep[API_SECRET_COUNT][API_NET_VALUE_MAX];
    bool masked[API_SECRET_COUNT];

    STATE_LOCK();

    // Remember the network values so we only ask for a restart on a real change.
    for (int i = 0; i < API_NETWORK_COUNT; i++)
    {
        const char *p = apiStringFieldPtr(API_NETWORK_KEYS[i]);
        strlcpy(before[i], p ? p : "", API_NET_VALUE_MAX);
    }

    // A masked secret means "keep it": configFromJson() would happily store the
    // literal mask, so stash and restore those fields around the merge.
    for (int i = 0; i < API_SECRET_COUNT; i++)
    {
        const char *supplied = obj[API_SECRET_KEYS[i]].as<const char *>();
        masked[i] = (supplied != NULL && strcmp(supplied, API_SECRET_MASK) == 0);
        const char *p = apiStringFieldPtr(API_SECRET_KEYS[i]);
        strlcpy(keep[i], p ? p : "", API_NET_VALUE_MAX);
    }

    bool ok = configFromJson(v, true, errors);

    for (int i = 0; i < API_SECRET_COUNT; i++)
    {
        if (!masked[i])
            continue;
        char part = 0;
        const ConfigField *f = configFindField(API_SECRET_KEYS[i], &part);
        if (f != NULL && f->kind == K_STR)
            strlcpy((char *)f->ptr, keep[i], f->size);
    }

    for (int i = 0; i < API_NETWORK_COUNT; i++)
    {
        const char *p = apiStringFieldPtr(API_NETWORK_KEYS[i]);
        if (strcmp(before[i], p ? p : "") != 0)
            restartRequired = true;
    }

    // Transient request flag, never persisted (see CONFIG_CONSUMED_KEYS).
    if (obj["rescanWiFiNetwork"].as<bool>())
        printerConfig.rescanWiFiNetwork = true;

    STATE_UNLOCK();

    configDirty = true;
    ledDirty = true;
    return ok;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void handleApiStatus(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    JsonDocument doc;
    buildStatusJson(doc);
    apiSendDoc(request, doc);
}

static void handleApiConfigGet(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    JsonDocument doc;
    buildConfigJson(doc, false);
    apiSendDoc(request, doc);
}

static void handleApiConfigPut(AsyncWebServerRequest *request, JsonVariant &json)
{
    AUTH_OR_RETURN(request);

    String errors;
    bool restartRequired = false;
    if (!applyConfigJson(json.as<JsonVariantConst>(), errors, restartRequired))
    {
        String msg = F("unknown or invalid keys: ");
        msg += errors;
        return apiError(request, 400, msg.c_str());
    }

    JsonDocument doc;
    buildConfigJson(doc, false);
    if (restartRequired)
        doc["restartRequired"] = true;
    apiSendDoc(request, doc);
    websocketNotifyChange();
}

static void handleApiConfigBackup(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);

    JsonDocument doc;
    buildConfigJson(doc, true);

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->addHeader("Content-Disposition", "attachment; filename=\"blledconfig.json\"");
    response->addHeader("Cache-Control", "no-store");
    serializeJsonPretty(doc, *response);
    request->send(response);
}

static void handleApiConfigReset(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    LogSerial.println(F("[API] Factory reset requested"));
    configDirty = false; // do not let the main loop re-save the config we are deleting
    factoryResetRequested = true;
    apiOk(request);
}

// ---- config restore (multipart) -------------------------------------------
static bool apiRestoreAuthOk = false;
static File apiRestoreFile;

static void handleApiRestoreUpload(AsyncWebServerRequest *request, const String &filename, size_t index,
                                   uint8_t *data, size_t len, bool final)
{
    if (index == 0)
    {
        // REVIEW #28: authenticate BEFORE the first byte reaches the filesystem.
        apiRestoreAuthOk = isAuthorized(request);
        if (!apiRestoreAuthOk)
        {
            request->requestAuthentication();
            return;
        }
        LogSerial.printf("[API] Config restore: %s\n", filename.c_str());
        LittleFS.remove(configTmpPath);
        apiRestoreFile = LittleFS.open(configTmpPath, "w");
    }
    if (!apiRestoreAuthOk || !apiRestoreFile)
        return;

    if (apiRestoreFile.position() + len > 32768)
    {
        apiRestoreFile.close();
        LittleFS.remove(configTmpPath);
        return;
    }
    apiRestoreFile.write(data, len);
    if (final)
        apiRestoreFile.close();
}

static void handleApiRestoreDone(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);

    bool ok = false;
    File f = LittleFS.open(configTmpPath, "r");
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
        return apiError(request, 400, "not a valid BLLED configuration");
    }

    configDirty = false; // the restored file wins; never overwrite it from RAM
    LittleFS.remove(configPath);
    LittleFS.rename(configTmpPath, configPath);
    request->send(200, "application/json", "{\"ok\":true,\"restartRequired\":true}");
    restartRequested = true;
    restartRequestMs = millis();
}

// ---- LED (7.3) -------------------------------------------------------------
static void handleApiLedPost(AsyncWebServerRequest *request, JsonVariant &json)
{
    AUTH_OR_RETURN(request);
    JsonObjectConst o = json.as<JsonObjectConst>();
    if (o.isNull())
        return apiError(request, 400, "expected a JSON object");

    COLOR c;
    if (!o["hex"].isNull())
    {
        const char *hex = o["hex"].as<const char *>();
        if (!apiHexIsValid(hex))
            return apiError(request, 400, "hex must be #rrggbb");
        c = hex2rgb(hex, 0, 0);
    }
    else
    {
        c.r = (short)constrain((int)(o["r"] | 0), 0, 255);
        c.g = (short)constrain((int)(o["g"] | 0), 0, 255);
        c.b = (short)constrain((int)(o["b"] | 0), 0, 255);
        snprintf(c.RGBhex, sizeof(c.RGBhex), "#%02x%02x%02x", c.r, c.g, c.b);
    }
    c.ww = (short)constrain((int)(o["ww"] | 0), 0, 255);
    c.cw = (short)constrain((int)(o["cw"] | 0), 0, 255);

    LedEffect effect = LedEffect::Solid;
    if (!o["effect"].isNull())
    {
        const char *e = o["effect"].as<const char *>();
        if (!apiEffectIsKnown(e))
            return apiError(request, 400, "effect must be solid|breathe|blink|fastblink|rainbow");
        effect = ledEffectFromString(e, LedEffect::Solid);
    }

    int32_t durationSec = o["durationSec"] | 0;
    if (durationSec < 0 || durationSec > 86400)
        return apiError(request, 400, "durationSec must be 0..86400");

    int8_t brightness = -1;
    if (!o["brightness"].isNull())
    {
        int b = o["brightness"].as<int>();
        if (b < 0 || b > 100)
            return apiError(request, 400, "brightness must be 0..100");
        brightness = (int8_t)b;
    }

    ledRequestOverride(c, effect, (uint32_t)durationSec * 1000UL, brightness);

    JsonDocument doc;
    buildLedJson(doc);
    apiSendDoc(request, doc);
    websocketNotifyChange();
}

static void handleApiLedDelete(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    ledClearOverride();
    JsonDocument doc;
    buildLedJson(doc);
    apiSendDoc(request, doc);
    websocketNotifyChange();
}

static void handleApiLedMode(AsyncWebServerRequest *request, JsonVariant &json)
{
    AUTH_OR_RETURN(request);
    const char *mode = json["mode"].as<const char *>();
    if (!apiModeIsKnown(mode))
        return apiError(request, 400, "mode must be auto|maintenance|test|rainbow|wifi|off");

    STATE_LOCK();
    printerConfig.ledMode = ledModeFromString(mode, printerConfig.ledMode);
    STATE_UNLOCK();
    configDirty = true;
    ledDirty = true;

    JsonDocument doc;
    buildLedJson(doc);
    apiSendDoc(request, doc);
    websocketNotifyChange();
}

static void handleApiLedBrightness(AsyncWebServerRequest *request, JsonVariant &json)
{
    AUTH_OR_RETURN(request);
    if (!json["brightness"].is<int>())
        return apiError(request, 400, "brightness must be a number 0..100");
    int b = json["brightness"].as<int>();
    if (b < 0 || b > 100)
        return apiError(request, 400, "brightness must be 0..100");

    STATE_LOCK();
    printerConfig.brightness = b;
    STATE_UNLOCK();
    configDirty = true;
    ledDirty = true;

    JsonDocument doc;
    buildLedJson(doc);
    apiSendDoc(request, doc);
    websocketNotifyChange();
}

static void handleApiLedIdentify(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    ledRequestIdentify();
    apiOk(request);
    websocketNotifyChange();
}

// ---- actions & discovery (7.4) --------------------------------------------
static void handleApiAction(AsyncWebServerRequest *request, JsonVariant &json)
{
    AUTH_OR_RETURN(request);
    const char *action = json["action"].as<const char *>();
    if (action == NULL)
        return apiError(request, 400, "missing \"action\"");

    if (!strcmp(action, "restart"))
    {
        LogSerial.println(F("[API] Restart requested"));
        restartRequested = true;
        restartRequestMs = millis();
    }
    else if (!strcmp(action, "chamberLight") || !strcmp(action, "workLight"))
    {
        if (json["on"].isNull())
            return apiError(request, 400, "chamberLight/workLight require \"on\"");
        bool on = json["on"].as<bool>();
        mqttEnqueue(!strcmp(action, "chamberLight") ? MqttCmd::ChamberLight : MqttCmd::WorkLight, on ? 1 : 0);
    }
    else if (!strcmp(action, "pushall"))
    {
        mqttEnqueue(MqttCmd::PushAll, (json["force"] | false) ? 1 : 0);
    }
    else if (!strcmp(action, "rescanWifi"))
    {
        printerConfig.rescanWiFiNetwork = true; // picked up by the main loop
    }
    else if (!strcmp(action, "discover"))
    {
        discoveryRequest();
    }
    else if (!strcmp(action, "reconnectMqtt"))
    {
        mqttEnqueue(MqttCmd::Reconnect, 0);
    }
    else
    {
        String msg = F("unknown action: ");
        msg += action;
        return apiError(request, 400, msg.c_str());
    }
    apiOk(request);
}

static void handleApiPrinters(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    JsonDocument doc;
    discoveryListJson(doc);
    apiSendDoc(request, doc);
}

static void handleApiWifiScan(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);
    JsonDocument doc;
    wifiScanResultsJson(doc); // {"scanning":true} until the async scan finishes
    apiSendDoc(request, doc);
}

static void handleApiInfo(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);

    JsonDocument doc;
    doc["fw"] = globalVariables.FWVersion;
    doc["build"] = __DATE__ " " __TIME__;
    doc["codename"] = STRCODENAME;
    doc["chip"] = ESP.getChipModel();
    doc["chipRev"] = ESP.getChipRevision();
    doc["cores"] = ESP.getChipCores();
    doc["flashSize"] = ESP.getFlashChipSize();
    doc["sketchSize"] = ESP.getSketchSize();
    doc["sketchFree"] = ESP.getFreeSketchSpace();
    doc["sdk"] = ESP.getSdkVersion();

    JsonObject pins = doc["pins"].to<JsonObject>();
    pins["r"] = redPin;
    pins["g"] = greenPin;
    pins["b"] = bluePin;
    pins["ww"] = warmPin;
    pins["cw"] = coldPin;

    JsonObject libs = doc["libs"].to<JsonObject>();
    libs["ArduinoJson"] = ARDUINOJSON_VERSION;
#ifdef ASYNCWEBSERVER_VERSION
    libs["ESPAsyncWebServer"] = ASYNCWEBSERVER_VERSION;
#endif
#ifdef ASYNCTCP_VERSION
    libs["AsyncTCP"] = ASYNCTCP_VERSION;
#endif
#ifdef WSL_VERSION
    libs["MycilaWebSerial"] = WSL_VERSION;
#endif
    apiSendDoc(request, doc);
}

// ---- OTA (7.4) -------------------------------------------------------------
static bool apiOtaAuthOk = false;
static bool apiOtaStarted = false;
static const char *apiOtaError = NULL;

static void handleApiUpdateUpload(AsyncWebServerRequest *request, const String &filename, size_t index,
                                  uint8_t *data, size_t len, bool final)
{
    if (index == 0)
    {
        // REVIEW #27: OTA was completely unauthenticated upstream.
        apiOtaAuthOk = isAuthorized(request);
        apiOtaStarted = false;
        apiOtaError = NULL;
        if (!apiOtaAuthOk)
        {
            request->requestAuthentication();
            return;
        }
        LogSerial.printf("[OTA] Start: %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            apiOtaError = Update.errorString();
            Update.printError(LogSerial);
            return;
        }
        apiOtaStarted = true;
    }
    if (!apiOtaAuthOk || !apiOtaStarted || apiOtaError != NULL)
        return;

    if (Update.write(data, len) != len)
    {
        apiOtaError = Update.errorString();
        Update.printError(LogSerial);
        Update.abort();
        apiOtaStarted = false;
        return;
    }
    if (final)
    {
        if (Update.end(true))
            LogSerial.printf("[OTA] Success (%u bytes)\n", (unsigned)(index + len));
        else
        {
            apiOtaError = Update.errorString();
            Update.printError(LogSerial);
        }
        apiOtaStarted = false;
    }
}

static void handleApiUpdateDone(AsyncWebServerRequest *request)
{
    AUTH_OR_RETURN(request);

    if (apiOtaError != NULL)
    {
        String msg = F("firmware update failed: ");
        msg += apiOtaError;
        apiOtaError = NULL;
        return apiError(request, 400, msg.c_str());
    }
    if (Update.hasError())
        return apiError(request, 400, "firmware update failed");
    if (!Update.isFinished())
        return apiError(request, 400, "no firmware image received");

    request->send(200, "application/json", "{\"ok\":true}");
    restartRequested = true;
    restartRequestMs = millis();
}

// ---------------------------------------------------------------------------
// Route registration
//
// Every route uses an EXACT matcher: the library default is "backward
// compatible" (exact OR prefix + "/"), under which /api/config would swallow
// /api/config/backup and /api/led would swallow /api/led/mode.
// ---------------------------------------------------------------------------

static void apiJsonRoute(AsyncWebServer &server, const char *uri, WebRequestMethodComposite method,
                         ArJsonRequestHandlerFunction fn)
{
    AsyncCallbackJsonWebHandler *h = new AsyncCallbackJsonWebHandler(AsyncURIMatcher::exact(uri), fn);
    h->setMethod(method);
    h->setMaxContentLength(API_JSON_BODY_MAX);
    server.addHandler(h);
}

void registerApiRoutes(AsyncWebServer &server)
{
    server.on(AsyncURIMatcher::exact("/api/status"), HTTP_GET, handleApiStatus);

    server.on(AsyncURIMatcher::exact("/api/config"), HTTP_GET, handleApiConfigGet);
    apiJsonRoute(server, "/api/config", HTTP_PUT, handleApiConfigPut);
    server.on(AsyncURIMatcher::exact("/api/config/backup"), HTTP_GET, handleApiConfigBackup);
    server.on(AsyncURIMatcher::exact("/api/config/restore"), HTTP_POST, handleApiRestoreDone, handleApiRestoreUpload);
    server.on(AsyncURIMatcher::exact("/api/config/reset"), HTTP_POST, handleApiConfigReset);

    apiJsonRoute(server, "/api/led", HTTP_POST, handleApiLedPost);
    server.on(AsyncURIMatcher::exact("/api/led"), HTTP_DELETE, handleApiLedDelete);
    apiJsonRoute(server, "/api/led/mode", HTTP_POST, handleApiLedMode);
    apiJsonRoute(server, "/api/led/brightness", HTTP_POST, handleApiLedBrightness);
    server.on(AsyncURIMatcher::exact("/api/led/identify"), HTTP_POST, handleApiLedIdentify);

    apiJsonRoute(server, "/api/action", HTTP_POST, handleApiAction);

    server.on(AsyncURIMatcher::exact("/api/printers"), HTTP_GET, handleApiPrinters);
    server.on(AsyncURIMatcher::exact("/api/wifi/scan"), HTTP_GET, handleApiWifiScan);
    server.on(AsyncURIMatcher::exact("/api/info"), HTTP_GET, handleApiInfo);

    server.on(AsyncURIMatcher::exact("/api/update"), HTTP_POST, handleApiUpdateDone, handleApiUpdateUpload);
}

#endif
