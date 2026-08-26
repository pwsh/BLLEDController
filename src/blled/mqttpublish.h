#ifndef _BLLED_MQTTPUBLISH
#define _BLLED_MQTTPUBLISH

// ---------------------------------------------------------------------------
// mqttpublish.h -- optional second MQTT client for an external broker, with
// Home Assistant discovery (ARCHITECTURE.md section 8, docs/HA-DISCOVERY.md).
//
// Threading (ARCHITECTURE.md section 2): the PubSubClient below is touched
// ONLY from mqttTask.  setupMqttPublish() (main loop, during setup) merely
// initialises plain fields; mqttPublishStateChanged() (main loop / mqttTask)
// only sets a volatile flag.  Everything else -- connect, subscribe, publish,
// callback -- runs inside mqttPublishLoop(), which mqttmanager.h calls from the
// task loop.
//
// Buffering: the status payload is 1.5-2 kB, far over a sane PubSubClient
// buffer.  It is therefore streamed with beginPublish()/write()/endPublish()
// through a 256-byte chunk writer, so the client buffer only has to hold the
// discovery configs (< 512 B each).
//
// Topics (base = mqttExtBaseTopic, default "blled/<host>"):
//   <base>/availability  retained "online", LWT "offline"
//   <base>/status        retained, the /api/status object
//   <base>/led           retained, the "led" sub-object
//   <base>/light         retained, HA JSON-light state
//   <base>/light/set     subscribed, HA JSON-light command
//   <base>/set           subscribed, BLLED JSON command
//   <base>/cmd           subscribed, plain-text command
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include "types.h"
#include "stages.h"
#include "logSerial.h"
#include "filesystem.h"
#include "mqttparsingutility.h"
#include "api.h"

// ---- tuning ----------------------------------------------------------------
#define EXT_BACKOFF_MIN_MS 5000UL
#define EXT_BACKOFF_MAX_MS 60000UL
#define EXT_STATUS_MIN_GAP_MS 1000UL // "within 1 s of a change"
#define EXT_LED_MIN_GAP_MS 500UL
#define EXT_BUFFER_SIZE 512          // discovery configs only; status is streamed
#define EXT_TOPIC_MAX 96
#define EXT_DISCOVERY_TOPIC_MAX 128

static WiFiClient extNetClient;
static PubSubClient extClient(extNetClient);

// ---- reserved topic strings (built once per configuration change) ----------
static char extBase[EXT_TOPIC_MAX] = "";
static char extTopicAvail[EXT_TOPIC_MAX] = "";
static char extTopicStatus[EXT_TOPIC_MAX] = "";
static char extTopicLed[EXT_TOPIC_MAX] = "";
static char extTopicLight[EXT_TOPIC_MAX] = "";
static char extTopicLightSet[EXT_TOPIC_MAX] = "";
static char extTopicSet[EXT_TOPIC_MAX] = "";
static char extTopicCmd[EXT_TOPIC_MAX] = "";
static char extClientId[24] = "";
static char extMac6[8] = "";
static char extDeviceId[20] = ""; // blled_<mac6>

// ---- runtime ---------------------------------------------------------------
static volatile bool extSetupDone = false;
static bool extWasEnabled = false;
static uint32_t extConfigHash = 0;      // host/port/user/pass/base -> reconnect
static uint32_t extDiscoveryHash = 0;   // host/prefix/base/enabled -> republish
static unsigned long extNextAttemptMs = 0;
static unsigned long extBackoffMs = EXT_BACKOFF_MIN_MS;
static unsigned long extLastStatusMs = 0;
static unsigned long extLastLedMs = 0;
static volatile bool extStateChanged = true;
static int extLastState = -1;
static int extDiscoveryCursor = -1; // >= 0 while publishing discovery configs
static bool extDiscoveryClearing = false;  // this run publishes empty payloads
static bool extDiscoveryCleared = false;   // the "remove entities" run already ran
static unsigned long extLastHashCheckMs = 0;

// Cheap change detector for the "led"/"light" payloads.
struct ExtLedFingerprint
{
    uint8_t output[5];
    uint8_t effect;
    uint8_t brightness;
    bool overrideActive;
    uint8_t reasonHash;
};
static ExtLedFingerprint extLastLed = {{0, 0, 0, 0, 0}, 255, 255, false, 0};

// ---------------------------------------------------------------------------
// Status accessors used by /api/status (declared in api.h)
// ---------------------------------------------------------------------------
bool extMqttConnected()
{
    return printerConfig.mqttExtEnabled && extClient.connected();
}

int extMqttState()
{
    return printerConfig.mqttExtEnabled ? extLastState : -1;
}

// ---------------------------------------------------------------------------
// Chunked publish: serialise straight into the socket without a big buffer.
// ---------------------------------------------------------------------------
class ExtChunkWriter : public Print
{
public:
    size_t write(uint8_t b) override
    {
        _buf[_n++] = b;
        if (_n == sizeof(_buf))
            flushChunk();
        return 1;
    }

    size_t write(const uint8_t *data, size_t len) override
    {
        size_t left = len;
        while (left > 0)
        {
            size_t room = sizeof(_buf) - _n;
            size_t take = (left < room) ? left : room;
            memcpy(_buf + _n, data, take);
            _n += take;
            data += take;
            left -= take;
            if (_n == sizeof(_buf))
                flushChunk();
        }
        return len;
    }

    void flushChunk()
    {
        if (_n == 0)
            return;
        extClient.write(_buf, _n);
        _n = 0;
    }

private:
    uint8_t _buf[256];
    size_t _n = 0;
};

// Discovery configs are up to ~500 B; streaming them keeps the PubSubClient
// buffer at 512 B (it then only has to hold the fixed header + topic).
static bool extPublishText(const char *topic, const char *payload, bool retained)
{
    if (!extClient.connected())
        return false;
    size_t len = strlen(payload);
    if (len == 0)
        return extClient.publish(topic, "", retained);
    if (!extClient.beginPublish(topic, len, retained))
        return false;
    extClient.write((const uint8_t *)payload, len);
    return extClient.endPublish() != 0;
}

static bool extPublishDoc(const char *topic, JsonDocument &doc, bool retained)
{
    if (!extClient.connected())
        return false;
    size_t len = measureJson(doc);
    if (!extClient.beginPublish(topic, len, retained))
        return false;
    ExtChunkWriter writer;
    serializeJson(doc, writer);
    writer.flushChunk();
    return extClient.endPublish() != 0;
}

// ---------------------------------------------------------------------------
// Topic / identity setup
// ---------------------------------------------------------------------------
static uint32_t extHashString(uint32_t h, const char *s)
{
    while (*s)
        h = h * 31u + (uint8_t)*s++;
    return h;
}

static void extBuildTopics()
{
    char base[EXT_TOPIC_MAX];
    STATE_LOCK();
    if (printerConfig.mqttExtBaseTopic[0] != '\0')
        strlcpy(base, printerConfig.mqttExtBaseTopic, sizeof(base));
    else
        snprintf(base, sizeof(base), "blled/%s", globalVariables.Host);
    STATE_UNLOCK();

    // strip a trailing slash so "<base>/x" never becomes "<base>//x"
    size_t l = strlen(base);
    while (l > 0 && base[l - 1] == '/')
        base[--l] = '\0';
    if (base[0] == '\0')
        strlcpy(base, "blled", sizeof(base));

    strlcpy(extBase, base, sizeof(extBase));
    snprintf(extTopicAvail, sizeof(extTopicAvail), "%s/availability", base);
    snprintf(extTopicStatus, sizeof(extTopicStatus), "%s/status", base);
    snprintf(extTopicLed, sizeof(extTopicLed), "%s/led", base);
    snprintf(extTopicLight, sizeof(extTopicLight), "%s/light", base);
    snprintf(extTopicLightSet, sizeof(extTopicLightSet), "%s/light/set", base);
    snprintf(extTopicSet, sizeof(extTopicSet), "%s/set", base);
    snprintf(extTopicCmd, sizeof(extTopicCmd), "%s/cmd", base);
}

static uint32_t extComputeConfigHash()
{
    uint32_t h = 2166136261u;
    STATE_LOCK();
    h = extHashString(h, printerConfig.mqttExtHost);
    h = h * 31u + printerConfig.mqttExtPort;
    h = extHashString(h, printerConfig.mqttExtUser);
    h = extHashString(h, printerConfig.mqttExtPass);
    h = extHashString(h, printerConfig.mqttExtBaseTopic);
    h = extHashString(h, globalVariables.Host);
    STATE_UNLOCK();
    return h;
}

static uint32_t extComputeDiscoveryHash()
{
    uint32_t h = 2166136261u;
    STATE_LOCK();
    h = extHashString(h, globalVariables.Host);
    h = extHashString(h, printerConfig.haPrefix);
    h = h * 31u + (printerConfig.haDiscovery ? 1u : 0u);
    STATE_UNLOCK();
    return extHashString(h, extBase);
}

// ---------------------------------------------------------------------------
// Payload builders
// ---------------------------------------------------------------------------

// HA JSON-light state shape (docs/HA-DISCOVERY.md 3a).  ON = override active;
// colour = override colour, or the current engine output when OFF.
static void extBuildLightJson(JsonDocument &doc)
{
    LedRuntime lr;
    int brightness;
    STATE_LOCK();
    memcpy(&lr, &ledRuntime, sizeof(LedRuntime));
    brightness = printerConfig.brightness;
    STATE_UNLOCK();

    bool on = lr.overrideActive;
    if (on && lr.overrideBrightness >= 0)
        brightness = lr.overrideBrightness;

    doc["state"] = on ? "ON" : "OFF";
    doc["brightness"] = (uint8_t)constrain((brightness * 255) / 100, 0, 255);
    doc["color_mode"] = "rgb";
    JsonObject col = doc["color"].to<JsonObject>();
    if (on)
    {
        col["r"] = lr.overrideColor.r;
        col["g"] = lr.overrideColor.g;
        col["b"] = lr.overrideColor.b;
        doc["effect"] = ledEffectToString(lr.overrideEffect);
    }
    else
    {
        col["r"] = lr.output[0];
        col["g"] = lr.output[1];
        col["b"] = lr.output[2];
        doc["effect"] = ledEffectToString(lr.effect);
    }
}

static ExtLedFingerprint extCurrentLedFingerprint()
{
    ExtLedFingerprint fp;
    STATE_LOCK();
    memcpy(fp.output, ledRuntime.output, 5);
    fp.effect = (uint8_t)ledRuntime.effect;
    fp.overrideActive = ledRuntime.overrideActive;
    fp.brightness = (uint8_t)constrain(printerConfig.brightness, 0, 100);
    uint8_t h = 0;
    for (const char *p = ledRuntime.reason; *p; p++)
        h = (uint8_t)(h * 31u + (uint8_t)*p);
    fp.reasonHash = h;
    STATE_UNLOCK();
    return fp;
}

// ---------------------------------------------------------------------------
// Home Assistant discovery (classic per-entity, retained, "~" shorthand)
// ---------------------------------------------------------------------------
enum ExtEntityKind : uint8_t
{
    E_LIGHT = 0,
    E_SELECT,
    E_NUMBER,
    E_SENSOR,
    E_BINARY,
    E_BUTTON
};

struct ExtEntity
{
    uint8_t kind;
    const char *key;       // object-id suffix / unique-id suffix
    const char *name;      // NULL = main entity ("name": null)
    const char *valueTpl;  // sensor/binary_sensor value_template ("" = none)
    const char *deviceCla; // "" = none
    const char *unit;      // "" = none
    const char *extra;     // verbatim extra JSON keys, "" = none
};

// state_topic is <base>/status for everything except the light (HA's JSON light
// schema cannot template a nested payload -- docs/HA-DISCOVERY.md 3a).
static const ExtEntity EXT_ENTITIES[] = {
    {E_LIGHT, "light", NULL, "", "", "", ""},
    {E_SELECT, "mode", "LED mode", "{{ value_json.led.mode }}", "", "",
     "\"options\":[\"auto\",\"maintenance\",\"test\",\"rainbow\",\"wifi\",\"off\"],\"cmd_tpl\":\"{\\\"mode\\\":\\\"{{ value }}\\\"}\",\"ic\":\"mdi:led-strip-variant\""},
    {E_NUMBER, "brightness", "Brightness", "{{ value_json.led.brightness }}", "", "%",
     "\"min\":0,\"max\":100,\"step\":1,\"cmd_tpl\":\"{\\\"brightness\\\":{{ value }}}\",\"ic\":\"mdi:brightness-6\""},

    {E_SENSOR, "stage", "Stage", "{{ value_json.printer.stageName | default('Unknown') }}", "", "", "\"ic\":\"mdi:printer-3d\""},
    {E_SENSOR, "gcodestate", "G-code state", "{{ value_json.printer.gcodeState | default('') }}", "", "", "\"ic\":\"mdi:state-machine\""},
    {E_SENSOR, "progress", "Progress", "{{ value_json.printer.progress | default(0) }}", "", "%", "\"stat_cla\":\"measurement\",\"ic\":\"mdi:progress-clock\""},
    {E_SENSOR, "remaining", "Remaining", "{{ value_json.printer.remainingMin | default(0) }}", "duration", "min", "\"stat_cla\":\"measurement\""},
    {E_SENSOR, "layer", "Layer", "{{ value_json.printer.layer | default(0) }}", "", "", "\"stat_cla\":\"measurement\",\"ic\":\"mdi:layers\""},
    {E_SENSOR, "totallayers", "Total layers", "{{ value_json.printer.totalLayers | default(0) }}", "", "", "\"ic\":\"mdi:layers-triple\""},
    {E_SENSOR, "nozzletemp", "Nozzle temperature", "{{ value_json.printer.nozzleTemp | default(0) }}", "temperature", "\\u00b0C", "\"stat_cla\":\"measurement\",\"sug_dsp_prc\":1"},
    {E_SENSOR, "bedtemp", "Bed temperature", "{{ value_json.printer.bedTemp | default(0) }}", "temperature", "\\u00b0C", "\"stat_cla\":\"measurement\",\"sug_dsp_prc\":1"},
    {E_SENSOR, "chambertemp", "Chamber temperature", "{{ value_json.printer.chamberTemp | default(0) }}", "temperature", "\\u00b0C", "\"stat_cla\":\"measurement\",\"sug_dsp_prc\":1"},
    {E_SENSOR, "ledreason", "LED reason", "{{ value_json.led.reason | default('') }}", "", "", "\"ic\":\"mdi:lightbulb-question\""},
    {E_SENSOR, "hmshighest", "Printer alert level", "{{ value_json.printer.hmsHighest | default('None') }}", "", "", "\"ic\":\"mdi:alert-circle\""},
    {E_SENSOR, "rssi", "WiFi signal", "{{ value_json.device.rssi | default(0) }}", "signal_strength", "dBm", "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\""},

    {E_BINARY, "connected", "Printer connected", "{{ 'ON' if value_json.printer.connected else 'OFF' }}", "connectivity", "", "\"ent_cat\":\"diagnostic\""},
    {E_BINARY, "door", "Door / lid", "{{ 'ON' if value_json.printer.doorOpen else 'OFF' }}", "door", "", ""},
    {E_BINARY, "chamberlight", "Chamber light", "{{ 'ON' if value_json.printer.chamberLight else 'OFF' }}", "light", "", ""},
    {E_BINARY, "finishactive", "Finish indication", "{{ 'ON' if value_json.timers.finishActive else 'OFF' }}", "", "", "\"ic\":\"mdi:party-popper\""},

    {E_BUTTON, "identify", "Identify", "", "", "", "\"pl_prs\":\"IDENTIFY\",\"ic\":\"mdi:led-on\""},
    {E_BUTTON, "pushall", "Refresh printer state", "", "", "", "\"pl_prs\":\"PUSHALL\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:refresh\""},
    {E_BUTTON, "restart", "Restart controller", "", "", "", "\"pl_prs\":\"RESTART\",\"ent_cat\":\"config\",\"dev_cla\":\"restart\""},
};
#define EXT_ENTITY_COUNT (sizeof(EXT_ENTITIES) / sizeof(EXT_ENTITIES[0]))

static const char *extComponentName(uint8_t kind)
{
    switch (kind)
    {
    case E_LIGHT: return "light";
    case E_SELECT: return "select";
    case E_NUMBER: return "number";
    case E_BINARY: return "binary_sensor";
    case E_BUTTON: return "button";
    case E_SENSOR:
    default: return "sensor";
    }
}

static void extDiscoveryTopic(const ExtEntity &e, char *out, size_t len)
{
    STATE_LOCK();
    snprintf(out, len, "%s/%s/%s_%s/config", printerConfig.haPrefix, extComponentName(e.kind),
             extDeviceId, e.key);
    STATE_UNLOCK();
}

// Hand-built so the payload stays byte-tight and the abbreviations from
// docs/HA-DISCOVERY.md section 2 are used verbatim.  Only the light carries the
// full `dev` + `o` blocks; HA merges device info across entities sharing `ids`.
static bool extPublishDiscoveryFor(const ExtEntity &e, bool clear)
{
    char topic[EXT_DISCOVERY_TOPIC_MAX];
    extDiscoveryTopic(e, topic, sizeof(topic));

    if (clear)
        return extClient.publish(topic, "", true); // empty retained = remove entity

    char host[33];
    char ip[16];
    STATE_LOCK();
    strlcpy(host, globalVariables.Host, sizeof(host));
    STATE_UNLOCK();
    strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));

    String p;
    p.reserve(520);
    p += "{\"~\":\"";
    p += extBase;
    p += "\",\"uniq_id\":\"";
    p += extDeviceId;
    p += "_";
    p += e.key;
    p += "\",";

    if (e.name == NULL)
        p += "\"name\":null,";
    else
    {
        p += "\"name\":\"";
        p += e.name;
        p += "\",";
    }

    switch (e.kind)
    {
    case E_LIGHT:
        p += "\"schema\":\"json\",\"stat_t\":\"~/light\",\"cmd_t\":\"~/light/set\","
             "\"brightness\":true,\"sup_clrm\":[\"rgb\"],\"effect\":true,"
             "\"fx_list\":[\"solid\",\"breathe\",\"blink\",\"fastblink\",\"rainbow\"],";
        break;
    case E_SELECT:
    case E_NUMBER:
        p += "\"stat_t\":\"~/status\",\"cmd_t\":\"~/set\",\"val_tpl\":\"";
        p += e.valueTpl;
        p += "\",";
        break;
    case E_BUTTON:
        p += "\"cmd_t\":\"~/cmd\",";
        break;
    default: // sensor / binary_sensor
        p += "\"stat_t\":\"~/status\",\"val_tpl\":\"";
        p += e.valueTpl;
        p += "\",";
        break;
    }

    if (e.deviceCla[0] != '\0')
    {
        p += "\"dev_cla\":\"";
        p += e.deviceCla;
        p += "\",";
    }
    if (e.unit[0] != '\0')
    {
        p += "\"unit_of_meas\":\"";
        p += e.unit;
        p += "\",";
    }
    if (e.extra[0] != '\0')
    {
        p += e.extra;
        p += ",";
    }

    p += "\"avty_t\":\"~/availability\",\"dev\":{\"ids\":[\"";
    p += extDeviceId;
    p += "\"]";
    if (e.kind == E_LIGHT)
    {
        p += ",\"name\":\"";
        p += host;
        p += "\",\"mf\":\"DutchDeveloper\",\"mdl\":\"BLLED\",\"sw\":\"";
        p += globalVariables.FWVersion;
        p += "\",\"cu\":\"http://";
        p += ip;
        p += "/\"},\"o\":{\"name\":\"BLLED\",\"sw\":\"";
        p += globalVariables.FWVersion;
        p += "\",\"url\":\"https://github.com/DutchDevelop/BLLEDController\"}}";
    }
    else
    {
        p += "}}";
    }

    return extPublishText(topic, p.c_str(), true);
}

// ---------------------------------------------------------------------------
// Inbound commands (runs inside extClient.loop(), i.e. on mqttTask)
// ---------------------------------------------------------------------------

static void extApplyOverrideFromJson(JsonObjectConst o)
{
    COLOR c;
    bool haveColor = false;

    const char *hex = o["hex"].as<const char *>();
    if (hex != NULL && hex[0] == '#' && strlen(hex) == 7)
    {
        c = hex2rgb(hex, 0, 0);
        haveColor = true;
    }
    if (!o["r"].isNull() || !o["g"].isNull() || !o["b"].isNull())
    {
        c.r = (short)constrain((int)(o["r"] | 0), 0, 255);
        c.g = (short)constrain((int)(o["g"] | 0), 0, 255);
        c.b = (short)constrain((int)(o["b"] | 0), 0, 255);
        haveColor = true;
    }
    c.ww = (short)constrain((int)(o["ww"] | 0), 0, 255);
    c.cw = (short)constrain((int)(o["cw"] | 0), 0, 255);
    if (!o["ww"].isNull() || !o["cw"].isNull())
        haveColor = true;
    if (!haveColor)
        return;
    snprintf(c.RGBhex, sizeof(c.RGBhex), "#%02x%02x%02x", c.r, c.g, c.b);

    LedEffect effect = ledEffectFromString(o["effect"].as<const char *>(), LedEffect::Solid);
    uint32_t durationMs = (uint32_t)constrain((int32_t)(o["durationSec"] | 0), (int32_t)0, (int32_t)86400) * 1000UL;
    int8_t brightness = -1;
    if (!o["brightness"].isNull())
        brightness = (int8_t)constrain((int)o["brightness"].as<int>(), 0, 100);

    ledRequestOverride(c, effect, durationMs, brightness);
}

// <base>/set -- the BLLED command shape (section 8).
static void extHandleSetTopic(JsonObjectConst o)
{
    if (o["clear"].as<bool>())
        ledClearOverride();

    const char *mode = o["mode"].as<const char *>();
    if (mode != NULL)
    {
        STATE_LOCK();
        printerConfig.ledMode = ledModeFromString(mode, printerConfig.ledMode);
        STATE_UNLOCK();
        configDirty = true;
        ledDirty = true;
    }

    if (o["brightness"].is<int>() && o["mode"].isNull() && o["hex"].isNull() &&
        o["r"].isNull() && o["g"].isNull() && o["b"].isNull())
    {
        // a bare {"brightness":n} persists; with a colour it is a temporary
        // override brightness, handled by extApplyOverrideFromJson()
        STATE_LOCK();
        printerConfig.brightness = constrain(o["brightness"].as<int>(), 0, 100);
        STATE_UNLOCK();
        configDirty = true;
        ledDirty = true;
    }

    if (!o["chamberLight"].isNull())
        mqttEnqueue(MqttCmd::ChamberLight, o["chamberLight"].as<bool>() ? 1 : 0);

    if (o["identify"].as<bool>())
        ledRequestIdentify();

    extApplyOverrideFromJson(o);
}

// <base>/light/set -- HA's fixed JSON-light command shape (untemplatable).
static void extHandleLightSet(JsonObjectConst o)
{
    const char *state = o["state"].as<const char *>();
    if (state != NULL && !strcasecmp(state, "OFF"))
    {
        ledClearOverride();
        return;
    }

    COLOR c;
    LedRuntime lr;
    STATE_LOCK();
    memcpy(&lr, &ledRuntime, sizeof(LedRuntime));
    STATE_UNLOCK();

    JsonObjectConst col = o["color"].as<JsonObjectConst>();
    if (!col.isNull())
    {
        c.r = (short)constrain((int)(col["r"] | 0), 0, 255);
        c.g = (short)constrain((int)(col["g"] | 0), 0, 255);
        c.b = (short)constrain((int)(col["b"] | 0), 0, 255);
    }
    else if (lr.overrideActive)
    {
        c = lr.overrideColor;
    }
    else
    {
        c.r = c.g = c.b = 255; // plain "ON" with nothing else: white
    }
    snprintf(c.RGBhex, sizeof(c.RGBhex), "#%02x%02x%02x", c.r, c.g, c.b);

    LedEffect effect = lr.overrideActive ? lr.overrideEffect : LedEffect::Solid;
    const char *fx = o["effect"].as<const char *>();
    if (fx != NULL)
        effect = ledEffectFromString(fx, effect);

    int8_t brightness = lr.overrideBrightness;
    if (o["brightness"].is<int>())
    {
        int b255 = constrain(o["brightness"].as<int>(), 0, 255);
        brightness = (int8_t)((b255 * 100 + 127) / 255);
    }

    ledRequestOverride(c, effect, 0, brightness);
}

// <base>/cmd -- plain payloads for simple automations and the HA buttons.
static void extHandleCmdTopic(const char *payload)
{
    if (!strcasecmp(payload, "ON"))
    {
        COLOR c;
        c.r = c.g = c.b = 255;
        strlcpy(c.RGBhex, "#ffffff", sizeof(c.RGBhex));
        ledRequestOverride(c, LedEffect::Solid, 0, -1);
    }
    else if (!strcasecmp(payload, "OFF"))
        ledClearOverride();
    else if (!strcasecmp(payload, "IDENTIFY"))
        ledRequestIdentify();
    else if (!strcasecmp(payload, "PUSHALL"))
        mqttEnqueue(MqttCmd::PushAll, 1);
    else if (!strcasecmp(payload, "RESTART"))
    {
        restartRequested = true;
        restartRequestMs = millis();
    }
    else
        LogSerial.printf("[ExtMQTT] Unknown /cmd payload: %s\n", payload);
}

static void extCallback(char *topic, byte *payload, unsigned int length)
{
    if (length == 0 || length > 1024)
        return;

    char buf[257];
    unsigned int n = (length < sizeof(buf) - 1) ? length : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    if (!strcmp(topic, extTopicCmd))
    {
        while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
            buf[--n] = '\0';
        extHandleCmdTopic(buf);
        extStateChanged = true;
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, buf, n))
    {
        LogSerial.printf("[ExtMQTT] Bad JSON on %s\n", topic);
        return;
    }
    JsonObjectConst o = doc.as<JsonObjectConst>();
    if (o.isNull())
        return;

    if (!strcmp(topic, extTopicLightSet))
        extHandleLightSet(o);
    else if (!strcmp(topic, extTopicSet))
        extHandleSetTopic(o);

    extStateChanged = true;
}

// ---------------------------------------------------------------------------
// Connection handling (mqttTask only)
// ---------------------------------------------------------------------------

static void extDisconnect(bool announceOffline)
{
    if (extClient.connected())
    {
        if (announceOffline)
            extClient.publish(extTopicAvail, "offline", true);
        extClient.disconnect();
    }
    extLastState = -1;
}

static void extTryConnect()
{
    char host[64], user[32], pass[64];
    uint16_t port;
    STATE_LOCK();
    strlcpy(host, printerConfig.mqttExtHost, sizeof(host));
    strlcpy(user, printerConfig.mqttExtUser, sizeof(user));
    strlcpy(pass, printerConfig.mqttExtPass, sizeof(pass));
    port = printerConfig.mqttExtPort;
    STATE_UNLOCK();

    if (host[0] == '\0')
        return;

    extClient.setServer(host, port);
    bool ok = extClient.connect(extClientId,
                                user[0] ? user : NULL,
                                user[0] ? pass : NULL,
                                extTopicAvail, 1 /* willQos */, true /* willRetain */, "offline");
    extLastState = extClient.state();

    if (!ok)
    {
        LogSerial.printf("[ExtMQTT] Connect to %s:%u failed: %s (%d)\n", host, (unsigned)port,
                         mqttStateText(extLastState), extLastState);
        extBackoffMs = (extBackoffMs * 2 > EXT_BACKOFF_MAX_MS) ? EXT_BACKOFF_MAX_MS : extBackoffMs * 2;
        extNextAttemptMs = millis() + extBackoffMs;
        return;
    }

    LogSerial.printf("[ExtMQTT] Connected to %s:%u as %s (base %s)\n", host, (unsigned)port, extClientId, extBase);
    extBackoffMs = EXT_BACKOFF_MIN_MS;

    extClient.publish(extTopicAvail, "online", true);
    extClient.subscribe(extTopicSet);
    extClient.subscribe(extTopicCmd);
    extClient.subscribe(extTopicLightSet);

    // (re)publish the retained discovery configs, one per loop pass.  With
    // discovery off, publish the empty payloads that remove them -- once.
    if (printerConfig.haDiscovery)
    {
        extDiscoveryCursor = 0;
        extDiscoveryClearing = false;
        extDiscoveryCleared = false;
    }
    else if (!extDiscoveryCleared)
    {
        extDiscoveryCursor = 0;
        extDiscoveryClearing = true;
    }
    else
    {
        extDiscoveryCursor = -1;
    }
    extStateChanged = true;
    extLastStatusMs = 0;
    extLastLedMs = 0;
}

// ---------------------------------------------------------------------------
// Hooks called by the core (weak symbols declared in types.h)
// ---------------------------------------------------------------------------

void setupMqttPublish()
{
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(extMac6, sizeof(extMac6), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(extClientId, sizeof(extClientId), "BLLED-%s", extMac6);
    snprintf(extDeviceId, sizeof(extDeviceId), "blled_%s", extMac6);

    extBuildTopics();
    extConfigHash = extComputeConfigHash();
    extDiscoveryHash = extComputeDiscoveryHash();

    extClient.setBufferSize(EXT_BUFFER_SIZE); // status/led/light are streamed
    extClient.setSocketTimeout(5);
    extClient.setKeepAlive(30);
    extClient.setCallback(extCallback);

    extSetupDone = true;
    extWasEnabled = printerConfig.mqttExtEnabled;
    LogSerial.printf("[ExtMQTT] %s (client %s)\n",
                     printerConfig.mqttExtEnabled ? "enabled" : "disabled", extClientId);
}

// "publish the status soon" hint; safe from any task (a single volatile store).
void mqttPublishStateChanged()
{
    extStateChanged = true;
}

// Called from mqttTask after mqttClient.loop().  Never blocks for long.
void mqttPublishLoop()
{
    if (!extSetupDone)
        return;

    const bool enabled = printerConfig.mqttExtEnabled;

    // ---- runtime enable/disable -------------------------------------------
    if (!enabled)
    {
        if (extWasEnabled)
        {
            LogSerial.println(F("[ExtMQTT] Disabled - disconnecting"));
            extDisconnect(true);
            extWasEnabled = false;
            extDiscoveryCursor = -1;
        }
        return;
    }
    if (!extWasEnabled)
    {
        LogSerial.println(F("[ExtMQTT] Enabled"));
        extWasEnabled = true;
        extNextAttemptMs = 0;
        extBackoffMs = EXT_BACKOFF_MIN_MS;
        extDiscoveryCleared = false;
    }

    if (WiFi.status() != WL_CONNECTED)
        return;

    // ---- broker/topic settings changed at runtime -------------------------
    // Checked once a second: each hash takes the state lock, and this loop runs
    // every 10 ms.
    bool hashCheckDue = (millis() - extLastHashCheckMs) >= 1000UL;
    if (hashCheckDue)
    {
        extLastHashCheckMs = millis();
        uint32_t cfgHash = extComputeConfigHash();
        if (cfgHash != extConfigHash)
        {
            LogSerial.println(F("[ExtMQTT] Broker settings changed - reconnecting"));
            extDisconnect(true);
            extBuildTopics();
            extConfigHash = cfgHash;
            extNextAttemptMs = 0;
            extBackoffMs = EXT_BACKOFF_MIN_MS;
            extDiscoveryCleared = false;
        }
    }

    // ---- connect ----------------------------------------------------------
    if (!extClient.connected())
    {
        if (millis() < extNextAttemptMs)
            return;
        extNextAttemptMs = millis() + extBackoffMs;
        extTryConnect();
        return;
    }

    extClient.loop();
    extLastState = extClient.state();

    // ---- discovery: republish when host/prefix/base/enable changed --------
    if (hashCheckDue)
    {
        uint32_t discHash = extComputeDiscoveryHash();
        if (discHash != extDiscoveryHash)
        {
            extDiscoveryHash = discHash;
            extDiscoveryCursor = 0;
            extDiscoveryClearing = !printerConfig.haDiscovery;
            if (printerConfig.haDiscovery)
                extDiscoveryCleared = false;
        }
    }

    // One entity per pass so the task never stalls on ~22 publishes.
    if (extDiscoveryCursor >= 0)
    {
        extPublishDiscoveryFor(EXT_ENTITIES[extDiscoveryCursor], extDiscoveryClearing);
        if ((size_t)(++extDiscoveryCursor) >= EXT_ENTITY_COUNT)
        {
            extDiscoveryCursor = -1;
            if (extDiscoveryClearing)
                extDiscoveryCleared = true;
            LogSerial.printf("[ExtMQTT] Home Assistant discovery %s (%u entities)\n",
                             extDiscoveryClearing ? "cleared" : "published", (unsigned)EXT_ENTITY_COUNT);
            extDiscoveryClearing = false;
        }
        return; // one publish per pass
    }

    const unsigned long now = millis();

    // ---- <base>/led and <base>/light on change ----------------------------
    if ((now - extLastLedMs) >= EXT_LED_MIN_GAP_MS)
    {
        ExtLedFingerprint fp = extCurrentLedFingerprint();
        if (memcmp(&fp, &extLastLed, sizeof(fp)) != 0)
        {
            extLastLed = fp;
            extLastLedMs = now;

            JsonDocument ledDoc;
            buildLedJson(ledDoc);
            extPublishDoc(extTopicLed, ledDoc, true);

            JsonDocument lightDoc;
            extBuildLightJson(lightDoc);
            extPublishDoc(extTopicLight, lightDoc, true);

            extStateChanged = true;
            return;
        }
    }

    // ---- <base>/status: every interval, and within 1 s of a change --------
    unsigned long intervalMs = (unsigned long)printerConfig.mqttExtIntervalSec * 1000UL;
    if (intervalMs < 1000UL)
        intervalMs = 1000UL;
    bool due = (now - extLastStatusMs) >= intervalMs;
    bool soon = extStateChanged && (now - extLastStatusMs) >= EXT_STATUS_MIN_GAP_MS;
    if (due || soon)
    {
        extStateChanged = false;
        extLastStatusMs = now;
        JsonDocument doc;
        buildStatusJson(doc);
        if (!extPublishDoc(extTopicStatus, doc, true))
            LogSerial.println(F("[ExtMQTT] Status publish failed"));
    }
}

#endif
