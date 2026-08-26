#ifndef _TYPES
#define _TYPES

// ---------------------------------------------------------------------------
// BLLED v3 shared types. This file is the contract between the core firmware,
// the API/MQTT layer and the web UI. See docs/ARCHITECTURE.md.
//
// Threading: see ARCHITECTURE.md §2. Everything in PrinterState, LedRuntime and
// PrinterConfig is shared between the main loop, the MQTT task and the AsyncTCP
// task and must be accessed under STATE_LOCK()/STATE_UNLOCK(). Keep critical
// sections short (assignments/copies only). All strings are fixed char arrays so
// that copying a struct is safe (no String across tasks).
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ---- Colour ---------------------------------------------------------------
typedef struct COLORStruct
{
    short r = 0;
    short g = 0;
    short b = 0;
    short ww = 0;
    short cw = 0;
    char RGBhex[8] = "#000000"; // canonical lower-case "#rrggbb"
} COLOR;

// ---- Enums (string forms are used verbatim in JSON/config) ----------------
enum class LedMode : uint8_t
{
    Auto = 0,     // "auto"        follow the printer
    Maintenance,  // "maintenance" always on, maintenanceColor
    Test,         // "test"        always on, testColor
    Rainbow,      // "rainbow"     colour cycle
    WifiStrength, // "wifi"        RSSI colour
    Off           // "off"         LEDs off
};

enum class LedEffect : uint8_t
{
    Solid = 0, // "solid"
    Breathe,   // "breathe"
    Blink,     // "blink"
    FastBlink, // "fastblink"
    Rainbow    // "rainbow" (engine-internal for LedMode::Rainbow; also allowed as override effect)
};

enum class PrintingVisual : uint8_t
{
    Solid = 0,     // "solid"
    ProgressBlend, // "progress"  runningColor -> finishColor by mc_percent
    Breathe        // "breathe"
};

enum class PreheatVisual : uint8_t
{
    Solid = 0, // "solid"
    TempGlow   // "tempglow"  brightness follows temperature ratio
};

enum class FinishExitMode : uint8_t
{
    Door = 0, // "door"
    Timer     // "timer"
};

// String <-> enum helpers (defined in filesystem.h, usable everywhere after include).
const char *ledModeToString(LedMode m);
LedMode ledModeFromString(const char *s, LedMode fallback = LedMode::Auto);
const char *ledEffectToString(LedEffect e);
LedEffect ledEffectFromString(const char *s, LedEffect fallback = LedEffect::Solid);
const char *printingVisualToString(PrintingVisual v);
PrintingVisual printingVisualFromString(const char *s, PrintingVisual fallback = PrintingVisual::Solid);
const char *preheatVisualToString(PreheatVisual v);
PreheatVisual preheatVisualFromString(const char *s, PreheatVisual fallback = PreheatVisual::Solid);
const char *finishExitModeToString(FinishExitMode m);
FinishExitMode finishExitModeFromString(const char *s, FinishExitMode fallback = FinishExitMode::Door);

// ---- Live printer state (written by mqttTask, read by everyone) -----------
struct HmsEntry
{
    uint64_t code = 0;    // (attr << 32) | code
    uint8_t severity = 0; // 1 Fatal, 2 Serious, 3 Common, 4 Info
    uint8_t module = 0;   // attr >> 24
    bool ignored = false; // matched hmsIgnoreList
};

#define HMS_MAX 8

struct PrinterState
{
    // connection
    bool online = false;             // printer MQTT connected
    unsigned long lastReportMs = 0;  // millis() of last parsed report
    unsigned long disconnectMs = 0;  // millis() when connection dropped (0 = connected)
    int mqttState = -1;              // PubSubClient::state()
    uint16_t reconnects = 0;
    char printerFw[24] = "";         // info.get_version -> module "ota" sw_ver
    char model[12] = "";             // printerModelFromSerial()

    // print job (print.*)
    char gcodeState[12] = "";        // gcode_state; "" = unknown
    int16_t stage = -1;              // stg_cur (X1 idle -1, P1 idle 255)
    int16_t overrideStage = 999;     // derived from HMS codes (6,10,17,20,21); 999 = none
    uint8_t progress = 0;            // mc_percent
    uint32_t remainingMin = 0;       // mc_remaining_time
    uint16_t layer = 0;              // layer_num
    uint16_t totalLayers = 0;        // total_layer_num
    char jobName[64] = "";           // subtask_name (fallback: gcode_file basename)
    char printType[8] = "";          // print_type ("local", "cloud", "")
    int32_t printError = 0;          // print_error
    uint8_t speedLevel = 0;          // spd_lvl 1..4
    bool sdcard = false;             // sdcard

    // temperatures (NAN = unknown)
    float nozzleTemp = NAN, nozzleTarget = NAN, bedTemp = NAN, bedTarget = NAN, chamberTemp = NAN;

    // fans, percent 0..100 (report gives "0".."15" strings)
    uint8_t fanPart = 0, fanAux = 0, fanChamber = 0, fanHeatbreak = 0;

    // lights / door / flags
    bool chamberLight = true;        // lights_report[node=chamber_light].mode == on  (also system.ledctrl)
    bool workLight = false;          // lights_report[node=work_light] on|flashing
    bool doorOpen = false;           // home_flag bit 23
    uint32_t homeFlag = 0;
    uint32_t doorEdgeCount = 0;      // increments on every door open/close edge
    unsigned long lastDoorOpenMs = 0, lastDoorCloseMs = 0;
    int8_t wifiSignal = 0;           // printer's own RSSI dBm (wifi_signal "-30dBm")

    // AMS summary
    bool amsPresent = false;
    int8_t amsTrayNow = -1;          // ams.tray_now (255 = none) -> -1
    char amsTrayColor[10] = "";      // "#rrggbb" of the active tray (tray_color is RRGGBBAA)
    uint8_t amsHumidity = 0;         // ams.ams[0].humidity (1..5 index as reported)

    // HMS
    HmsEntry hms[HMS_MAX];
    uint8_t hmsCount = 0;
    uint8_t hmsHighestSeverity = 0;  // most severe non-ignored (1 = worst); 0 = none
    uint64_t hmsHighestCode = 0;
};

// ---- LED runtime (owned by main loop; override/identify fields may be set from other tasks) ----
struct LedRuntime
{
    // engine output (last written, 0..255 after brightness)
    uint8_t output[5] = {0, 0, 0, 0, 0};   // r,g,b,ww,cw
    COLOR targetColor;                      // decision colour before brightness/effect
    LedEffect effect = LedEffect::Solid;
    char reason[48] = "Boot";
    float progress = 0;                     // 0..1 for ProgressBlend

    // override (API/MQTT)
    bool overrideActive = false;
    COLOR overrideColor;
    LedEffect overrideEffect = LedEffect::Solid;
    unsigned long overrideUntilMs = 0;      // 0 = until cleared
    int8_t overrideBrightness = -1;         // -1 = use config
    bool identifyRequested = false;

    // bookkeeping
    bool finishActive = false;
    unsigned long finishStartMs = 0;
    unsigned long inactivityStartMs = 0;
    bool idleOffActive = false;
    bool doorToggleOff = false;
    bool chamberLightLocked = false;
    uint32_t seenDoorEdges = 0;
};

// ---- Persisted configuration (flat JSON keys = field names unless noted) ---
struct PrinterConfig
{
    // printer connection
    char printerIP[16] = "";
    char accessCode[9] = "";
    char serialNumber[16] = "";
    bool printerAutoIp = true;         // update printerIP when discovery sees the serial at a new IP
    bool isP1Printer = false;          // P1 (no lidar/door switch) defaults

    // wifi
    char BSSID[18] = "";
    bool rescanWiFiNetwork = false;    // transient request (not saved)

    // led general
    int brightness = 20;               // 0..100
    LedMode ledMode = LedMode::Auto;   // json "ledMode": auto|maintenance|test|rainbow|wifi|off
    uint16_t fadeMs = 500;             // 0..5000
    uint8_t effectSpeed = 5;           // 1..10
    bool followChamberLight = true;    // LEDs off when the printer's chamber light is off
    COLOR runningColor;                // default WW+CW 255
    COLOR maintenanceColor;            // default WW+CW 255
    COLOR testColor;                   // default #3F3CFB
    COLOR wifiColor;                   // boot/scan colour, default #FFA500
    PrintingVisual printingVisual = PrintingVisual::Solid; // json "printingVisual": solid|progress|breathe
    PreheatVisual preheatVisual = PreheatVisual::Solid;    // json "preheatVisual": solid|tempglow

    // print events
    bool finishIndication = true;
    COLOR finishColor;                 // default #00FF00
    LedEffect finishEffect = LedEffect::Solid;
    FinishExitMode finishExitMode = FinishExitMode::Door; // json "finishExitMode": door|timer
    uint16_t finishTimerMins = 10;
    bool inactivityEnabled = true;
    uint16_t inactivityMins = 60;
    bool controlChamberLight = false;  // drive the printer's chamber light (on at print start/door, off at idle)
    bool doorToggleEnabled = true;     // door closed twice within 2 s toggles the LEDs
    uint16_t offlineTimeoutSec = 30;   // LEDs off this long after the printer MQTT drops
    bool lidarStagesEnabled = true;    // apply the lidar stage colours (upstream "doorSwitch")
    COLOR stage14Color, stage1Color, stage8Color, stage9Color, stage10Color; // lidar stages (default off)

    // errors & alerts
    bool errorDetection = true;
    LedEffect errorEffect = LedEffect::Solid;
    LedEffect pauseEffect = LedEffect::Solid;
    COLOR pauseColor, firstLayerColor, nozzleClogColor;                       // default blue
    COLOR hmsSeriousColor, hmsFatalColor, filamentRunoutColor, frontCoverColor, nozzleTempColor, bedTempColor; // default red
    bool hmsCommonEnabled = false;
    COLOR hmsCommonColor;              // default #FFA500
    char hmsIgnoreList[256] = "";      // normalised: upper-case, '_' separators, comma-separated

    // external mqtt / home assistant
    bool mqttExtEnabled = false;
    char mqttExtHost[64] = "";
    uint16_t mqttExtPort = 1883;
    char mqttExtUser[32] = "";
    char mqttExtPass[64] = "";
    char mqttExtBaseTopic[48] = "";    // "" -> "blled/<host>"
    uint16_t mqttExtIntervalSec = 10;
    bool haDiscovery = true;
    char haPrefix[24] = "homeassistant";

    // debug
    bool debugVerbose = false;         // everything
    bool debugChanges = true;          // on-change events
    bool debugMqtt = false;            // filtered payload per message (Serial only)
};

// JSON colour keys: "<name>RGB", "<name>WW", "<name>CW" where <name> is the field name without
// "Color": running, maintenance, test, wifi, finish, stage14, stage1, stage8, stage9, stage10,
// pause, firstLayer, nozzleClog, hmsSerious, hmsFatal, hmsCommon, filamentRunout, frontCover,
// nozzleTemp, bedTemp.  (Upstream used "firstlayer"/"nozzleclog"; accept both on load.)

struct SecurityVariables
{
    char HTTPUser[40] = "";  // json "webUser"
    char HTTPPass[40] = "";  // json "webPass"
};

struct GlobalVariables
{
    char SSID[33] = "";      // json "wifiSSID"  (legacy "ssid")
    char APPW[65] = "";      // json "wifiPass"  (legacy "appw")
    char Host[33] = "BLLED"; // json "host"
    const char *FWVersion = STRVERSION;
    bool started = false;    // full STA start-up completed (false in AP mode)
    bool apMode = false;
};

// ---- Globals ---------------------------------------------------------------
PrinterState printerState;
LedRuntime ledRuntime;
PrinterConfig printerConfig;
SecurityVariables securityVariables;
GlobalVariables globalVariables;

SemaphoreHandle_t stateMutex = NULL; // created in setup() before any task starts
#define STATE_LOCK() xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY)
#define STATE_UNLOCK() xSemaphoreGiveRecursive(stateMutex)

volatile bool printerStateDirty = false;    // mqttTask -> main loop: re-evaluate LEDs, push WS/MQTT status
volatile bool ledDirty = false;             // any task -> main loop: re-evaluate LEDs now
volatile bool configDirty = false;          // any task -> main loop: save config + apply
volatile bool restartRequested = false;     // any task -> main loop: restart after ~1.5 s
unsigned long restartRequestMs = 0;

// ---- Cross-module declarations (definitions live in the named header) ------
// leds.h
void ledRequestOverride(const COLOR &c, LedEffect e, uint32_t durationMs, int8_t brightness = -1);
void ledClearOverride();
void ledRequestIdentify();

// mqttmanager.h
enum class MqttCmd : uint8_t { None = 0, ChamberLight, WorkLight, PushAll, GetVersion, Reconnect };
bool mqttEnqueue(MqttCmd cmd, int32_t arg = 0);
void controlChamberLight(bool on); // wrapper: mqttEnqueue(MqttCmd::ChamberLight, on)

// mqttpublish.h (api workstream). Weak default so the core compiles without it.
void setupMqttPublish() __attribute__((weak));
void mqttPublishLoop() __attribute__((weak));   // called from mqttTask after mqttClient.loop()
void mqttPublishStateChanged() __attribute__((weak)); // hint: publish status soon

// filesystem.h
COLOR hex2rgb(const char *hex, short ww = 0, short cw = 0); // safe: any input, never loops
void loadConfig();
void saveConfig();
void applyConfigDefaults();

// bblPrinterDiscovery.h
void discoveryRequest(); // start a discovery round (non-blocking)

// wifi-manager.h
void wifiScanRequest();  // start async scan

#endif
