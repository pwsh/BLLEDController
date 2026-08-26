#ifndef _BLLEDFILESYSTEM
#define _BLLEDFILESYSTEM

// ---------------------------------------------------------------------------
// filesystem.h -- configuration persistence (ARCHITECTURE.md §5)
//
// Responsibilities
//   * mount LittleFS, load/save /blledconfig.json (flat keys = PrinterConfig
//     field names), migrate the upstream v2 key names/units, validate + clamp.
//   * a single table (CONFIG_FIELDS) drives load, save, JSON export and JSON
//     import, so a missing/extra/garbage key can never crash the device.
//   * safe colour parsing (hex2rgb) and the enum <-> string helpers declared in
//     types.h.
//   * helpers reused by the API workstream:
//        void configToJson(JsonDocument &doc)                     full config (secrets included)
//        bool configFromJson(JsonVariantConst v, bool partial, String &errors)
//        bool validateConfigJson(const char *buf, size_t len)     restore upload check
//
// Threading
//   loadConfig()/saveConfig()/deleteConfig() touch LittleFS and must only be
//   called from the main loop (setup() included).  The async web task sets
//   configDirty = true instead; main.cpp saves and applies.
//   configToJson()/configFromJson() only touch the config structs -- the caller
//   holds STATE_LOCK() when called from another task.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "logSerial.h"
#include "types.h"
#include "stages.h"

static const char *configPath = "/blledconfig.json";
static const char *configTmpPath = "/blledconfig.tmp";

// ---------------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------------

// Single hex digit -> value, or -1.
static inline int hexDigitValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Safe for ANY input (NULL, "", "#", "#ff", "#3F3CFBAA", "#ffffff\n", 200 chars).
// Never loops on the input length, never writes past RGBhex[8].
// Short input is right-padded with zeros (upstream behaviour: "#ff" -> ff0000).
COLOR hex2rgb(const char *hex, short ww, short cw)
{
    COLOR color;
    uint32_t value = 0;
    int digits = 0;

    if (hex != NULL)
    {
        const char *p = hex;
        if (*p == '#')
            p++;
        while (*p != '\0' && digits < 6)
        {
            int d = hexDigitValue(*p);
            if (d < 0)
                break; // stop at the first non-hex character
            value = (value << 4) | (uint32_t)d;
            digits++;
            p++;
        }
    }
    if (digits < 6)
        value <<= (4 * (6 - digits));

    color.r = (short)((value >> 16) & 0xFF);
    color.g = (short)((value >> 8) & 0xFF);
    color.b = (short)(value & 0xFF);
    color.ww = constrain(ww, (short)0, (short)255);
    color.cw = constrain(cw, (short)0, (short)255);
    snprintf(color.RGBhex, sizeof(color.RGBhex), "#%02x%02x%02x", color.r, color.g, color.b);
    return color;
}

// Refresh the canonical "#rrggbb" cache after r/g/b were changed directly.
static inline void colorSyncHex(COLOR &c)
{
    c.r = constrain(c.r, (short)0, (short)255);
    c.g = constrain(c.g, (short)0, (short)255);
    c.b = constrain(c.b, (short)0, (short)255);
    c.ww = constrain(c.ww, (short)0, (short)255);
    c.cw = constrain(c.cw, (short)0, (short)255);
    snprintf(c.RGBhex, sizeof(c.RGBhex), "#%02x%02x%02x", c.r, c.g, c.b);
}

static inline bool colorIsBlack(const COLOR &c)
{
    return (c.r + c.g + c.b + c.ww + c.cw) == 0;
}

// ---------------------------------------------------------------------------
// Enum <-> string (declared in types.h)
// ---------------------------------------------------------------------------

const char *ledModeToString(LedMode m)
{
    switch (m)
    {
    case LedMode::Maintenance: return "maintenance";
    case LedMode::Test: return "test";
    case LedMode::Rainbow: return "rainbow";
    case LedMode::WifiStrength: return "wifi";
    case LedMode::Off: return "off";
    case LedMode::Auto:
    default: return "auto";
    }
}

LedMode ledModeFromString(const char *s, LedMode fallback)
{
    if (!s) return fallback;
    if (!strcasecmp(s, "auto")) return LedMode::Auto;
    if (!strcasecmp(s, "maintenance")) return LedMode::Maintenance;
    if (!strcasecmp(s, "test")) return LedMode::Test;
    if (!strcasecmp(s, "rainbow")) return LedMode::Rainbow;
    if (!strcasecmp(s, "wifi")) return LedMode::WifiStrength;
    if (!strcasecmp(s, "off")) return LedMode::Off;
    return fallback;
}

const char *ledEffectToString(LedEffect e)
{
    switch (e)
    {
    case LedEffect::Breathe: return "breathe";
    case LedEffect::Blink: return "blink";
    case LedEffect::FastBlink: return "fastblink";
    case LedEffect::Rainbow: return "rainbow";
    case LedEffect::Solid:
    default: return "solid";
    }
}

LedEffect ledEffectFromString(const char *s, LedEffect fallback)
{
    if (!s) return fallback;
    if (!strcasecmp(s, "solid")) return LedEffect::Solid;
    if (!strcasecmp(s, "breathe")) return LedEffect::Breathe;
    if (!strcasecmp(s, "blink")) return LedEffect::Blink;
    if (!strcasecmp(s, "fastblink")) return LedEffect::FastBlink;
    if (!strcasecmp(s, "rainbow")) return LedEffect::Rainbow;
    return fallback;
}

const char *printingVisualToString(PrintingVisual v)
{
    switch (v)
    {
    case PrintingVisual::ProgressBlend: return "progress";
    case PrintingVisual::Breathe: return "breathe";
    case PrintingVisual::Solid:
    default: return "solid";
    }
}

PrintingVisual printingVisualFromString(const char *s, PrintingVisual fallback)
{
    if (!s) return fallback;
    if (!strcasecmp(s, "solid")) return PrintingVisual::Solid;
    if (!strcasecmp(s, "progress")) return PrintingVisual::ProgressBlend;
    if (!strcasecmp(s, "breathe")) return PrintingVisual::Breathe;
    return fallback;
}

const char *preheatVisualToString(PreheatVisual v)
{
    return (v == PreheatVisual::TempGlow) ? "tempglow" : "solid";
}

PreheatVisual preheatVisualFromString(const char *s, PreheatVisual fallback)
{
    if (!s) return fallback;
    if (!strcasecmp(s, "solid")) return PreheatVisual::Solid;
    if (!strcasecmp(s, "tempglow")) return PreheatVisual::TempGlow;
    return fallback;
}

const char *finishExitModeToString(FinishExitMode m)
{
    return (m == FinishExitMode::Timer) ? "timer" : "door";
}

FinishExitMode finishExitModeFromString(const char *s, FinishExitMode fallback)
{
    if (!s) return fallback;
    if (!strcasecmp(s, "door")) return FinishExitMode::Door;
    if (!strcasecmp(s, "timer")) return FinishExitMode::Timer;
    return fallback;
}

// ---------------------------------------------------------------------------
// Configuration table -- drives load, save, JSON import/export and validation.
// Every key has a default (set by applyConfigDefaults()), so a missing key can
// never leave a field undefined and can never crash the loader.
// ---------------------------------------------------------------------------

enum CfgKind : uint8_t
{
    K_BOOL,
    K_INT,     // int
    K_U16,     // uint16_t
    K_U8,      // uint8_t
    K_STR,     // char[] of `size`
    K_COLOR,   // three JSON keys: <key>RGB / <key>WW / <key>CW
    K_LEDMODE,
    K_EFFECT,
    K_PVIS,
    K_PHVIS,
    K_FEXIT
};

struct ConfigField
{
    const char *key;
    uint8_t kind;
    void *ptr;
    uint16_t size; // string buffer size
    int32_t lo;    // numeric clamp (inclusive)
    int32_t hi;
};

static const ConfigField CONFIG_FIELDS[] = {
    // --- network / identity -------------------------------------------------
    {"wifiSSID", K_STR, globalVariables.SSID, sizeof(globalVariables.SSID), 0, 0},
    {"wifiPass", K_STR, globalVariables.APPW, sizeof(globalVariables.APPW), 0, 0},
    {"host", K_STR, globalVariables.Host, sizeof(globalVariables.Host), 0, 0},
    {"webUser", K_STR, securityVariables.HTTPUser, sizeof(securityVariables.HTTPUser), 0, 0},
    {"webPass", K_STR, securityVariables.HTTPPass, sizeof(securityVariables.HTTPPass), 0, 0},
    {"BSSID", K_STR, printerConfig.BSSID, sizeof(printerConfig.BSSID), 0, 0},
    // --- printer ------------------------------------------------------------
    {"printerIP", K_STR, printerConfig.printerIP, sizeof(printerConfig.printerIP), 0, 0},
    {"accessCode", K_STR, printerConfig.accessCode, sizeof(printerConfig.accessCode), 0, 0},
    {"serialNumber", K_STR, printerConfig.serialNumber, sizeof(printerConfig.serialNumber), 0, 0},
    {"printerAutoIp", K_BOOL, &printerConfig.printerAutoIp, 0, 0, 0},
    {"isP1Printer", K_BOOL, &printerConfig.isP1Printer, 0, 0, 0},
    // --- led general --------------------------------------------------------
    {"brightness", K_INT, &printerConfig.brightness, 0, 0, 100},
    {"ledMode", K_LEDMODE, &printerConfig.ledMode, 0, 0, 0},
    {"fadeMs", K_U16, &printerConfig.fadeMs, 0, 0, 5000},
    {"effectSpeed", K_U8, &printerConfig.effectSpeed, 0, 1, 10},
    {"followChamberLight", K_BOOL, &printerConfig.followChamberLight, 0, 0, 0},
    {"printingVisual", K_PVIS, &printerConfig.printingVisual, 0, 0, 0},
    {"preheatVisual", K_PHVIS, &printerConfig.preheatVisual, 0, 0, 0},
    {"running", K_COLOR, &printerConfig.runningColor, 0, 0, 0},
    {"maintenance", K_COLOR, &printerConfig.maintenanceColor, 0, 0, 0},
    {"test", K_COLOR, &printerConfig.testColor, 0, 0, 0},
    {"wifi", K_COLOR, &printerConfig.wifiColor, 0, 0, 0},
    {"preheat", K_COLOR, &printerConfig.preheatColor, 0, 0, 0},
    // --- print events -------------------------------------------------------
    {"finishIndication", K_BOOL, &printerConfig.finishIndication, 0, 0, 0},
    {"finish", K_COLOR, &printerConfig.finishColor, 0, 0, 0},
    {"finishEffect", K_EFFECT, &printerConfig.finishEffect, 0, 0, 0},
    {"finishExitMode", K_FEXIT, &printerConfig.finishExitMode, 0, 0, 0},
    {"finishTimerMins", K_U16, &printerConfig.finishTimerMins, 0, 0, 999},
    {"inactivityEnabled", K_BOOL, &printerConfig.inactivityEnabled, 0, 0, 0},
    {"inactivityMins", K_U16, &printerConfig.inactivityMins, 0, 0, 999},
    {"controlChamberLight", K_BOOL, &printerConfig.controlChamberLight, 0, 0, 0},
    {"doorToggleEnabled", K_BOOL, &printerConfig.doorToggleEnabled, 0, 0, 0},
    {"offlineTimeoutSec", K_U16, &printerConfig.offlineTimeoutSec, 0, 0, 3600},
    {"lidarStagesEnabled", K_BOOL, &printerConfig.lidarStagesEnabled, 0, 0, 0},
    {"stage14", K_COLOR, &printerConfig.stage14Color, 0, 0, 0},
    {"stage1", K_COLOR, &printerConfig.stage1Color, 0, 0, 0},
    {"stage8", K_COLOR, &printerConfig.stage8Color, 0, 0, 0},
    {"stage9", K_COLOR, &printerConfig.stage9Color, 0, 0, 0},
    {"stage10", K_COLOR, &printerConfig.stage10Color, 0, 0, 0},
    // --- errors & alerts ----------------------------------------------------
    {"errorDetection", K_BOOL, &printerConfig.errorDetection, 0, 0, 0},
    {"errorEffect", K_EFFECT, &printerConfig.errorEffect, 0, 0, 0},
    {"pauseEffect", K_EFFECT, &printerConfig.pauseEffect, 0, 0, 0},
    {"pause", K_COLOR, &printerConfig.pauseColor, 0, 0, 0},
    {"firstLayer", K_COLOR, &printerConfig.firstLayerColor, 0, 0, 0},
    {"nozzleClog", K_COLOR, &printerConfig.nozzleClogColor, 0, 0, 0},
    {"hmsSerious", K_COLOR, &printerConfig.hmsSeriousColor, 0, 0, 0},
    {"hmsFatal", K_COLOR, &printerConfig.hmsFatalColor, 0, 0, 0},
    {"hmsCommonEnabled", K_BOOL, &printerConfig.hmsCommonEnabled, 0, 0, 0},
    {"hmsCommon", K_COLOR, &printerConfig.hmsCommonColor, 0, 0, 0},
    {"filamentRunout", K_COLOR, &printerConfig.filamentRunoutColor, 0, 0, 0},
    {"frontCover", K_COLOR, &printerConfig.frontCoverColor, 0, 0, 0},
    {"nozzleTemp", K_COLOR, &printerConfig.nozzleTempColor, 0, 0, 0},
    {"bedTemp", K_COLOR, &printerConfig.bedTempColor, 0, 0, 0},
    {"hmsIgnoreList", K_STR, printerConfig.hmsIgnoreList, sizeof(printerConfig.hmsIgnoreList), 0, 0},
    // --- external mqtt / home assistant -------------------------------------
    {"mqttExtEnabled", K_BOOL, &printerConfig.mqttExtEnabled, 0, 0, 0},
    {"mqttExtHost", K_STR, printerConfig.mqttExtHost, sizeof(printerConfig.mqttExtHost), 0, 0},
    {"mqttExtPort", K_U16, &printerConfig.mqttExtPort, 0, 1, 65535},
    {"mqttExtUser", K_STR, printerConfig.mqttExtUser, sizeof(printerConfig.mqttExtUser), 0, 0},
    {"mqttExtPass", K_STR, printerConfig.mqttExtPass, sizeof(printerConfig.mqttExtPass), 0, 0},
    {"mqttExtBaseTopic", K_STR, printerConfig.mqttExtBaseTopic, sizeof(printerConfig.mqttExtBaseTopic), 0, 0},
    {"mqttExtIntervalSec", K_U16, &printerConfig.mqttExtIntervalSec, 0, 1, 3600},
    {"haDiscovery", K_BOOL, &printerConfig.haDiscovery, 0, 0, 0},
    {"haPrefix", K_STR, printerConfig.haPrefix, sizeof(printerConfig.haPrefix), 0, 0},
    // --- debug --------------------------------------------------------------
    {"debugVerbose", K_BOOL, &printerConfig.debugVerbose, 0, 0, 0},
    {"debugChanges", K_BOOL, &printerConfig.debugChanges, 0, 0, 0},
    {"debugMqtt", K_BOOL, &printerConfig.debugMqtt, 0, 0, 0},
};

static const size_t CONFIG_FIELD_COUNT = sizeof(CONFIG_FIELDS) / sizeof(CONFIG_FIELDS[0]);

// Upstream v2 key -> v3 key.  Applied on load AND on restore of a v2 backup, so
// old configuration files keep working (ARCHITECTURE.md §5).
struct KeyAlias
{
    const char *from;
    const char *to;
};

static const KeyAlias CONFIG_ALIASES[] = {
    {"ssid", "wifiSSID"},
    {"appw", "wifiPass"},
    {"pass", "wifiPass"},
    {"HTTPUser", "webUser"},
    {"HTTPPass", "webPass"},
    {"bssi", "BSSID"},
    {"printerIp", "printerIP"},
    {"printerSerial", "serialNumber"},
    {"replicatestate", "followChamberLight"},
    {"finishindication", "finishIndication"},
    {"finishColor", "finishRGB"},
    {"errordetection", "errorDetection"},
    {"doorSwitch", "lidarStagesEnabled"},
    {"p1Printer", "isP1Printer"},
    {"debuging", "debugVerbose"},
    {"debugingchange", "debugChanges"},
    {"mqttdebug", "debugMqtt"},
    {"firstlayerRGB", "firstLayerRGB"},
    {"firstlayerWW", "firstLayerWW"},
    {"firstlayerCW", "firstLayerCW"},
    {"nozzleclogRGB", "nozzleClogRGB"},
    {"nozzleclogWW", "nozzleClogWW"},
    {"nozzleclogCW", "nozzleClogCW"},
};

// Legacy keys consumed by migrateLegacyConfig() (value semantics changed) or
// deliberately dropped.  Listed so they are not reported as "unknown key".
static const char *const CONFIG_CONSUMED_KEYS[] = {
    "maintMode", "discoMode", "showtestcolor", "debugwifi",
    "finishExit", "inactivityTimeOut", "finish_check", "webpagePassword",
    "rescanWiFiNetwork",
};

// Resolve an alias; returns the canonical key (or the input when not aliased).
static const char *configResolveAlias(const char *key)
{
    for (size_t i = 0; i < sizeof(CONFIG_ALIASES) / sizeof(CONFIG_ALIASES[0]); i++)
        if (strcmp(CONFIG_ALIASES[i].from, key) == 0)
            return CONFIG_ALIASES[i].to;
    return key;
}

// Find the field for a JSON key.  For colours the key is "<base>RGB|WW|CW";
// `colorPart` then receives 'R', 'W' or 'C'.
static const ConfigField *configFindField(const char *key, char *colorPart)
{
    *colorPart = 0;
    for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++)
    {
        const ConfigField &f = CONFIG_FIELDS[i];
        if (f.kind == K_COLOR)
        {
            size_t klen = strlen(f.key);
            if (strncmp(key, f.key, klen) != 0)
                continue;
            const char *suffix = key + klen;
            if (strcmp(suffix, "RGB") == 0) { *colorPart = 'R'; return &f; }
            if (strcmp(suffix, "WW") == 0) { *colorPart = 'W'; return &f; }
            if (strcmp(suffix, "CW") == 0) { *colorPart = 'C'; return &f; }
            continue;
        }
        if (strcmp(f.key, key) == 0)
            return &f;
    }
    return NULL;
}

static bool configKeyIsConsumed(const char *key)
{
    for (size_t i = 0; i < sizeof(CONFIG_CONSUMED_KEYS) / sizeof(CONFIG_CONSUMED_KEYS[0]); i++)
        if (strcmp(CONFIG_CONSUMED_KEYS[i], key) == 0)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Defaults (upstream colours -- see docs/CHANGELOG.md if you change one)
// ---------------------------------------------------------------------------
void applyConfigDefaults()
{
    printerConfig = PrinterConfig(); // struct initialisers hold the scalar defaults

    printerConfig.runningColor = hex2rgb("#000000", 255, 255);     // warm+cold white
    printerConfig.maintenanceColor = hex2rgb("#000000", 255, 255); // warm+cold white
    printerConfig.testColor = hex2rgb("#3F3CFB", 0, 0);            // violet
    printerConfig.wifiColor = hex2rgb("#FFA500", 0, 0);            // orange
    printerConfig.preheatColor = hex2rgb("#FF6A00", 0, 0);         // heat-up blend start (cold)
    printerConfig.finishColor = hex2rgb("#00FF00", 0, 0);          // green

    printerConfig.stage14Color = hex2rgb("#000000", 0, 0); // cleaning nozzle   - off
    printerConfig.stage1Color = hex2rgb("#000055", 0, 0);  // bed levelling     - dim blue
    printerConfig.stage8Color = hex2rgb("#000000", 0, 0);  // calibr. extrusion - off
    printerConfig.stage9Color = hex2rgb("#000000", 0, 0);  // scanning bed      - off
    printerConfig.stage10Color = hex2rgb("#000000", 0, 0); // first layer insp. - off

    printerConfig.pauseColor = hex2rgb("#0000FF", 0, 0);
    printerConfig.firstLayerColor = hex2rgb("#0000FF", 0, 0);
    printerConfig.nozzleClogColor = hex2rgb("#0000FF", 0, 0);

    printerConfig.hmsSeriousColor = hex2rgb("#FF0000", 0, 0);
    printerConfig.hmsFatalColor = hex2rgb("#FF0000", 0, 0);
    printerConfig.hmsCommonColor = hex2rgb("#FFA500", 0, 0);
    printerConfig.filamentRunoutColor = hex2rgb("#FF0000", 0, 0);
    printerConfig.frontCoverColor = hex2rgb("#FF0000", 0, 0);
    printerConfig.nozzleTempColor = hex2rgb("#FF0000", 0, 0);
    printerConfig.bedTempColor = hex2rgb("#FF0000", 0, 0);
}

// ---------------------------------------------------------------------------
// Validation / normalisation
// ---------------------------------------------------------------------------

// Normalise the HMS ignore list ONCE (upstream did this per entry per message):
// upper case, '-' -> '_', whitespace removed, newlines -> ',', no empty items.
static void normaliseHmsIgnoreList()
{
    char out[sizeof(printerConfig.hmsIgnoreList)];
    size_t o = 0;
    bool lastWasComma = true; // suppress a leading comma
    for (size_t i = 0; printerConfig.hmsIgnoreList[i] != '\0' && o + 1 < sizeof(out); i++)
    {
        char c = printerConfig.hmsIgnoreList[i];
        if (c == ' ' || c == '\t' || c == '\r')
            continue;
        if (c == '\n' || c == ';' || c == ',')
        {
            if (lastWasComma)
                continue;
            out[o++] = ',';
            lastWasComma = true;
            continue;
        }
        if (c == '-')
            c = '_';
        out[o++] = (char)toupper((unsigned char)c);
        lastWasComma = false;
    }
    while (o > 0 && out[o - 1] == ',')
        o--; // no trailing comma
    out[o] = '\0';
    memcpy(printerConfig.hmsIgnoreList, out, o + 1);
}

bool hmsCodeIsIgnored(const char *code, const char *list = printerConfig.hmsIgnoreList);

// Is `code` ("HMS_XXXX_XXXX_XXXX_XXXX") on the (already normalised) ignore list?
// `list` defaults to the live config; the MQTT task passes a local copy so it
// never walks a string the API task might be rewriting.
bool hmsCodeIsIgnored(const char *code, const char *list)
{
    if (list == NULL || list[0] == '\0' || code == NULL)
        return false;
    size_t clen = strlen(code);
    const char *p = list;
    while (p != NULL && *p != '\0')
    {
        const char *end = strchr(p, ',');
        size_t len = (end != NULL) ? (size_t)(end - p) : strlen(p);
        if (len == clen && strncasecmp(p, code, clen) == 0)
            return true;
        p = (end != NULL) ? end + 1 : NULL;
    }
    return false;
}

// Clamp every numeric field, keep strings NUL-terminated, normalise derived data.
void validateConfig()
{
    for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++)
    {
        const ConfigField &f = CONFIG_FIELDS[i];
        switch (f.kind)
        {
        case K_INT:
        {
            int *p = (int *)f.ptr;
            *p = (int)constrain((int32_t)*p, f.lo, f.hi);
            break;
        }
        case K_U16:
        {
            uint16_t *p = (uint16_t *)f.ptr;
            *p = (uint16_t)constrain((int32_t)*p, f.lo, f.hi);
            break;
        }
        case K_U8:
        {
            uint8_t *p = (uint8_t *)f.ptr;
            *p = (uint8_t)constrain((int32_t)*p, f.lo, f.hi);
            break;
        }
        case K_STR:
            ((char *)f.ptr)[f.size - 1] = '\0';
            break;
        case K_COLOR:
            colorSyncHex(*(COLOR *)f.ptr);
            break;
        default:
            break;
        }
    }

    // never allow an empty mDNS host name (upstream hung in MDNS.begin(""))
    if (globalVariables.Host[0] == '\0')
        strlcpy(globalVariables.Host, "BLLED", sizeof(globalVariables.Host));
    if (printerConfig.haPrefix[0] == '\0')
        strlcpy(printerConfig.haPrefix, "homeassistant", sizeof(printerConfig.haPrefix));

    normaliseHmsIgnoreList();

    // keep the derived printer model in sync with the serial number
    strlcpy(printerState.model, printerModelFromSerial(printerConfig.serialNumber), sizeof(printerState.model));
}

// ---------------------------------------------------------------------------
// JSON <-> config
// ---------------------------------------------------------------------------

void configToJson(JsonDocument &doc)
{
    for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++)
    {
        const ConfigField &f = CONFIG_FIELDS[i];
        switch (f.kind)
        {
        case K_BOOL: doc[f.key] = *(bool *)f.ptr; break;
        case K_INT: doc[f.key] = *(int *)f.ptr; break;
        case K_U16: doc[f.key] = *(uint16_t *)f.ptr; break;
        case K_U8: doc[f.key] = *(uint8_t *)f.ptr; break;
        case K_STR: doc[f.key] = (const char *)f.ptr; break;
        case K_LEDMODE: doc[f.key] = ledModeToString(*(LedMode *)f.ptr); break;
        case K_EFFECT: doc[f.key] = ledEffectToString(*(LedEffect *)f.ptr); break;
        case K_PVIS: doc[f.key] = printingVisualToString(*(PrintingVisual *)f.ptr); break;
        case K_PHVIS: doc[f.key] = preheatVisualToString(*(PreheatVisual *)f.ptr); break;
        case K_FEXIT: doc[f.key] = finishExitModeToString(*(FinishExitMode *)f.ptr); break;
        case K_COLOR:
        {
            const COLOR *c = (const COLOR *)f.ptr;
            char key[40];
            snprintf(key, sizeof(key), "%sRGB", f.key);
            doc[key] = c->RGBhex;
            snprintf(key, sizeof(key), "%sWW", f.key);
            doc[key] = c->ww;
            snprintf(key, sizeof(key), "%sCW", f.key);
            doc[key] = c->cw;
            break;
        }
        }
    }
}

// Loose bool conversion so form posts ("on"/"1"/"true") work too.
static bool jsonToBool(JsonVariantConst v)
{
    if (v.is<bool>())
        return v.as<bool>();
    if (v.is<int>())
        return v.as<int>() != 0;
    const char *s = v.as<const char *>();
    if (s == NULL)
        return false;
    return (!strcasecmp(s, "true") || !strcasecmp(s, "on") || !strcasecmp(s, "1") || !strcasecmp(s, "yes"));
}

static void jsonToStr(JsonVariantConst v, char *dst, size_t size)
{
    const char *s = v.as<const char *>();
    if (s != NULL)
    {
        strlcpy(dst, s, size);
        return;
    }
    if (!v.isNull())
    {
        String tmp = v.as<String>();
        strlcpy(dst, tmp.c_str(), size);
    }
}

// Apply every *present* key of `v`.  Absent keys always keep their current
// value, so this serves both the full load and a partial PUT /api/config.
// Unknown keys are collected in `errors` (comma separated) and make it return
// false; everything that WAS understood has still been applied.
bool configFromJson(JsonVariantConst v, bool partial, String &errors)
{
    (void)partial; // absent keys are always "unchanged"; kept for API clarity
    JsonObjectConst obj = v.as<JsonObjectConst>();
    if (obj.isNull())
    {
        errors = F("expected a JSON object");
        return false;
    }

    bool ok = true;
    for (JsonPairConst kv : obj)
    {
        const char *rawKey = kv.key().c_str();
        if (configKeyIsConsumed(rawKey))
            continue; // handled by migrateLegacyConfig() / intentionally dropped
        const char *key = configResolveAlias(rawKey);
        char part = 0;
        const ConfigField *f = configFindField(key, &part);
        if (f == NULL)
        {
            if (errors.length() > 0)
                errors += ", ";
            errors += rawKey;
            ok = false;
            continue;
        }
        JsonVariantConst val = kv.value();
        switch (f->kind)
        {
        case K_BOOL: *(bool *)f->ptr = jsonToBool(val); break;
        case K_INT: *(int *)f->ptr = (int)constrain((int32_t)val.as<int32_t>(), f->lo, f->hi); break;
        case K_U16: *(uint16_t *)f->ptr = (uint16_t)constrain((int32_t)val.as<int32_t>(), f->lo, f->hi); break;
        case K_U8: *(uint8_t *)f->ptr = (uint8_t)constrain((int32_t)val.as<int32_t>(), f->lo, f->hi); break;
        case K_STR: jsonToStr(val, (char *)f->ptr, f->size); break;
        case K_LEDMODE: *(LedMode *)f->ptr = ledModeFromString(val.as<const char *>(), *(LedMode *)f->ptr); break;
        case K_EFFECT: *(LedEffect *)f->ptr = ledEffectFromString(val.as<const char *>(), *(LedEffect *)f->ptr); break;
        case K_PVIS: *(PrintingVisual *)f->ptr = printingVisualFromString(val.as<const char *>(), *(PrintingVisual *)f->ptr); break;
        case K_PHVIS: *(PreheatVisual *)f->ptr = preheatVisualFromString(val.as<const char *>(), *(PreheatVisual *)f->ptr); break;
        case K_FEXIT: *(FinishExitMode *)f->ptr = finishExitModeFromString(val.as<const char *>(), *(FinishExitMode *)f->ptr); break;
        case K_COLOR:
        {
            COLOR *c = (COLOR *)f->ptr;
            if (part == 'R')
            {
                COLOR parsed = hex2rgb(val.as<const char *>(), c->ww, c->cw);
                *c = parsed;
            }
            else if (part == 'W')
                c->ww = (short)constrain(val.as<int>(), 0, 255);
            else if (part == 'C')
                c->cw = (short)constrain(val.as<int>(), 0, 255);
            break;
        }
        }
    }

    validateConfig();
    return ok;
}

// Upstream v2 -> v3 value semantics (the *name* changes live in CONFIG_ALIASES).
static void migrateLegacyConfig(JsonDocument &json)
{
    bool migrated = false;

    // The five mutually exclusive "modes" became one ledMode enum.
    if (!json["ledMode"].is<const char *>())
    {
        if (jsonToBool(json["maintMode"]))
        {
            printerConfig.ledMode = LedMode::Maintenance;
            migrated = true;
        }
        else if (jsonToBool(json["discoMode"]))
        {
            printerConfig.ledMode = LedMode::Rainbow;
            migrated = true;
        }
        else if (jsonToBool(json["showtestcolor"]))
        {
            printerConfig.ledMode = LedMode::Test;
            migrated = true;
        }
        else if (jsonToBool(json["debugwifi"]))
        {
            printerConfig.ledMode = LedMode::WifiStrength;
            migrated = true;
        }
    }

    // finishExit(bool, true = wait for a door interaction) -> finishExitMode
    if (!json["finishExitMode"].is<const char *>() && !json["finishExit"].isNull())
    {
        printerConfig.finishExitMode = jsonToBool(json["finishExit"]) ? FinishExitMode::Door : FinishExitMode::Timer;
        migrated = true;
    }

    // upstream stored the finish timer in MILLIseconds under "finishTimerMins"
    if (!json["finishTimerMins"].isNull())
    {
        uint32_t v = json["finishTimerMins"].as<uint32_t>();
        if (v > 1000)
        {
            printerConfig.finishTimerMins = (uint16_t)constrain((int32_t)(v / 60000UL), 0, 999);
            migrated = true;
        }
    }

    // inactivityTimeOut (ms) -> inactivityMins
    if (json["inactivityMins"].isNull() && !json["inactivityTimeOut"].isNull())
    {
        uint32_t v = json["inactivityTimeOut"].as<uint32_t>();
        printerConfig.inactivityMins = (uint16_t)constrain((int32_t)(v / 60000UL), 0, 999);
        migrated = true;
    }

    if (migrated)
    {
        LogSerial.println(F("[Filesystem] Migrated legacy (v2) configuration keys"));
        validateConfig();
        configDirty = true; // re-save in the new format from the main loop
    }
}

// ---------------------------------------------------------------------------
// File handling (main loop only)
// ---------------------------------------------------------------------------

void setupFileSystem()
{
    LogSerial.println(F("[Filesystem] Mounting LittleFS"));
    if (!LittleFS.begin())
    {
        LogSerial.println(F("[Filesystem] Mount failed - formatting"));
        LittleFS.format();
        if (!LittleFS.begin())
        {
            LogSerial.println(F("[Filesystem] Format failed - continuing without persistence"));
            return;
        }
    }
    LogSerial.println(F("[Filesystem] Mounted LittleFS"));
}

bool hasConfigFile()
{
    return LittleFS.exists(configPath);
}

void saveConfig()
{
    // Serialise under the lock, do the (slow) file I/O outside it.
    JsonDocument json;
    STATE_LOCK();
    configToJson(json);
    STATE_UNLOCK();

    File configFile = LittleFS.open(configTmpPath, "w");
    if (!configFile)
    {
        LogSerial.println(F("[Filesystem] Failed to open config for writing"));
        return;
    }
    size_t written = serializeJson(json, configFile);
    configFile.close();

    if (written == 0)
    {
        LogSerial.println(F("[Filesystem] Failed to serialise config"));
        LittleFS.remove(configTmpPath);
        return;
    }
    LittleFS.remove(configPath);
    if (!LittleFS.rename(configTmpPath, configPath))
    {
        LogSerial.println(F("[Filesystem] Failed to rename temporary config"));
        return;
    }
    LogSerial.printf("[Filesystem] Config saved (%u bytes)\n", (unsigned)written);
}

void loadConfig()
{
    applyConfigDefaults();

    File configFile = LittleFS.open(configPath, "r");
    if (!configFile)
    {
        LogSerial.println(F("[Filesystem] No config file - writing defaults"));
        validateConfig();
        saveConfig();
        return;
    }

    size_t size = configFile.size();
    if (size == 0 || size > 32768)
    {
        LogSerial.println(F("[Filesystem] Config file empty or oversized - using defaults"));
        configFile.close();
        validateConfig();
        return;
    }

    std::unique_ptr<char[]> buf(new (std::nothrow) char[size + 1]);
    if (!buf)
    {
        LogSerial.println(F("[Filesystem] Out of memory reading config"));
        configFile.close();
        validateConfig();
        return;
    }
    size_t read = configFile.readBytes(buf.get(), size);
    configFile.close();
    buf[read] = '\0';

    JsonDocument json;
    DeserializationError err = deserializeJson(json, buf.get(), read);
    if (err)
    {
        LogSerial.print(F("[Filesystem] Config parse error: "));
        LogSerial.println(err.c_str());
        LogSerial.println(F("[Filesystem] Keeping defaults (file left untouched)"));
        validateConfig();
        return;
    }

    String unknown;
    configFromJson(json.as<JsonVariantConst>(), false, unknown);
    migrateLegacyConfig(json);
    validateConfig();

    if (unknown.length() > 0 && printerConfig.debugVerbose)
    {
        LogSerial.print(F("[Filesystem] Ignored unknown config keys: "));
        LogSerial.println(unknown);
    }
    LogSerial.println(F("[Filesystem] Loaded config"));
}

void deleteConfig()
{
    LogSerial.println(F("[Filesystem] Deleting config file"));
    LittleFS.remove(configPath);
    LittleFS.remove(configTmpPath);
}

// Restore-upload guard: the payload must parse and contain at least one key we
// understand, otherwise we would happily brick the device with a random file.
bool validateConfigJson(const char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return false;

    JsonDocument json;
    if (deserializeJson(json, buf, len))
        return false;

    JsonObjectConst obj = json.as<JsonObjectConst>();
    if (obj.isNull())
        return false;

    for (JsonPairConst kv : obj)
    {
        const char *rawKey = kv.key().c_str();
        if (configKeyIsConsumed(rawKey))
            return true;
        char part = 0;
        if (configFindField(configResolveAlias(rawKey), &part) != NULL)
            return true;
    }
    return false;
}

#endif
