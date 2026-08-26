#ifndef _MQTTMANAGER
#define _MQTTMANAGER

// ---------------------------------------------------------------------------
// mqttmanager.h -- printer MQTT client + report parsing (ARCHITECTURE.md §6)
//
// Responsibilities
//   * one FreeRTOS task (core 1, prio 1, 20 kB) owning the TLS PubSubClient to
//     the printer on 8883.  Nobody else may touch mqttClient (REVIEW #2).
//   * on connect: subscribe device/<serial>/report and request `pushall` +
//     `get_version` (P1/A1 only push deltas -- REVIEW #32).
//   * parse each report through an ArduinoJson filter into `printerState` under
//     STATE_LOCK(), then raise printerStateDirty.
//   * an 8-slot lock-free command ring drained by the task, so the main loop and
//     the async web task can ask for a publish without touching PubSubClient.
//
// Threading
//   Everything below runs on mqttTask except mqttEnqueue()/controlChamberLight()
//   (any task) and setupMqtt() (main loop, once).  Per-message debug output goes
//   to Serial directly -- LogSerial (WebSerial) is only safe for the rare
//   connect/disconnect lines (REVIEW #7).
// ---------------------------------------------------------------------------

#define TASK_RAM 20480 // 20 kB MQTT task stack

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "types.h"
#include "stages.h"
#include "logSerial.h"
#include "filesystem.h"
#include "mqttparsingutility.h"
#include "AutoGrowBufferStream.h"

// ---- tunables --------------------------------------------------------------
#define MQTT_PORT 8883
#define MQTT_RETRY_MS 3000UL        // between connection attempts
#define MQTT_PUSHALL_MIN_MS 300000UL // 5 min minimum between pushall requests
#define MQTT_STATE_UNKNOWN_MS 10000UL // ask for a pushall if gcode_state is still unknown
#define MQTT_FASTPATH_HOLD_MS 3000UL // ignore slower push_status light/state after a system command

WiFiClientSecure wifiSecureClient;
PubSubClient mqttClient(wifiSecureClient);
AutoGrowBufferStream stream;

static char mqttClientId[24] = "BLLED-";
static char mqttReportTopic[40] = "";
static char mqttRequestTopic[40] = "";

static TaskHandle_t mqttTaskHandle = NULL;
static unsigned long mqttAttemptMs = 0;
static unsigned long mqttPushAllMs = 0;
static unsigned long mqttConnectedMs = 0;
static unsigned long mqttFastPathMs = 0;

// ---------------------------------------------------------------------------
// Command queue (any task -> mqttTask)
// ---------------------------------------------------------------------------
#define MQTT_CMD_SLOTS 8

struct MqttCmdItem
{
    MqttCmd cmd;
    int32_t arg;
};

static portMUX_TYPE mqttCmdMux = portMUX_INITIALIZER_UNLOCKED;
static MqttCmdItem mqttCmdRing[MQTT_CMD_SLOTS];
static uint8_t mqttCmdHead = 0;
static uint8_t mqttCmdTail = 0;

bool mqttEnqueue(MqttCmd cmd, int32_t arg)
{
    bool ok = false;
    portENTER_CRITICAL(&mqttCmdMux);
    uint8_t next = (uint8_t)((mqttCmdHead + 1) % MQTT_CMD_SLOTS);
    if (next != mqttCmdTail)
    {
        mqttCmdRing[mqttCmdHead].cmd = cmd;
        mqttCmdRing[mqttCmdHead].arg = arg;
        mqttCmdHead = next;
        ok = true;
    }
    portEXIT_CRITICAL(&mqttCmdMux);
    return ok;
}

static bool mqttDequeue(MqttCmdItem &out)
{
    bool ok = false;
    portENTER_CRITICAL(&mqttCmdMux);
    if (mqttCmdTail != mqttCmdHead)
    {
        out = mqttCmdRing[mqttCmdTail];
        mqttCmdTail = (uint8_t)((mqttCmdTail + 1) % MQTT_CMD_SLOTS);
        ok = true;
    }
    portEXIT_CRITICAL(&mqttCmdMux);
    return ok;
}

// Thin wrapper kept for compatibility with the upstream call sites.
void controlChamberLight(bool on)
{
    mqttEnqueue(MqttCmd::ChamberLight, on ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Publishing (mqttTask only)
// ---------------------------------------------------------------------------

static void mqttPublishRequest(JsonDocument &doc, const char *what)
{
    if (!mqttClient.connected() || mqttRequestTopic[0] == '\0')
        return;
    char payload[320];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0 || n >= sizeof(payload))
    {
        Serial.println(F("[MQTT] request payload too large - dropped"));
        return;
    }
    if (!mqttClient.publish(mqttRequestTopic, payload))
        Serial.printf("[MQTT] publish of %s failed\n", what);
    else if (printerConfig.debugChanges || printerConfig.debugVerbose)
        Serial.printf("[MQTT] sent %s\n", what);
}

static void mqttSendLightControl(const char *node, bool on)
{
    JsonDocument doc;
    JsonObject system = doc["system"].to<JsonObject>();
    system["command"] = "ledctrl";
    system["sequence_id"] = "blled";
    system["led_node"] = node;
    system["led_mode"] = on ? "on" : "off";
    system["led_on_time"] = 500;
    system["led_off_time"] = 500;
    system["loop_times"] = 0;
    system["interval_time"] = 1000;
    mqttPublishRequest(doc, node);
    mqttFastPathMs = millis();
}

static void mqttSendPushAll(bool force)
{
    unsigned long now = millis();
    if (!force && mqttPushAllMs != 0 && (now - mqttPushAllMs) < MQTT_PUSHALL_MIN_MS)
        return;
    mqttPushAllMs = now;

    JsonDocument doc;
    JsonObject pushing = doc["pushing"].to<JsonObject>();
    pushing["sequence_id"] = "0";
    pushing["command"] = "pushall";
    pushing["version"] = 1;
    pushing["push_target"] = 1;
    mqttPublishRequest(doc, "pushall");
}

static void mqttSendGetVersion()
{
    JsonDocument doc;
    JsonObject info = doc["info"].to<JsonObject>();
    info["sequence_id"] = "0";
    info["command"] = "get_version";
    mqttPublishRequest(doc, "get_version");
}

// ---------------------------------------------------------------------------
// Report parsing
// ---------------------------------------------------------------------------

// "0".."15" -> 0..100 %
static uint8_t fanToPercent(JsonVariantConst v)
{
    if (v.isNull())
        return 0;
    int raw = v.as<int>(); // works for both "12" and 12
    raw = constrain(raw, 0, 15);
    return (uint8_t)lround(raw * 100.0 / 15.0);
}

// HMS codes that upstream mapped onto a printer stage (front cover, runout, ...).
static int16_t hmsOverrideStageFor(uint64_t code)
{
    switch (code)
    {
    case 0x0C0003000003000BULL: return 10; // first layer inspection
    case 0x0300120000020001ULL: return 17; // front cover falling
    case 0x0700200000030001ULL: return 6;  // filament runout
    case 0x0300020000010001ULL: return 20; // nozzle temperature
    case 0x0300010000010007ULL: return 21; // bed temperature
    default: return 999;
    }
}

// Build the parse filter once; it is reused for every message.
static const JsonDocument &mqttParseFilter()
{
    static JsonDocument filter;
    static bool built = false;
    if (built)
        return filter;
    built = true;

    JsonObject print = filter["print"].to<JsonObject>();
    print["command"] = true;
    print["gcode_state"] = true;
    print["stg_cur"] = true;
    print["hms"] = true;
    print["home_flag"] = true;
    print["lights_report"] = true;
    print["mc_percent"] = true;
    print["mc_remaining_time"] = true;
    print["layer_num"] = true;
    print["total_layer_num"] = true;
    print["subtask_name"] = true;
    print["gcode_file"] = true;
    print["print_type"] = true;
    print["print_error"] = true;
    print["spd_lvl"] = true;
    print["sdcard"] = true;
    print["nozzle_temper"] = true;
    print["nozzle_target_temper"] = true;
    print["bed_temper"] = true;
    print["bed_target_temper"] = true;
    print["chamber_temper"] = true;
    print["cooling_fan_speed"] = true;
    print["big_fan1_speed"] = true;
    print["big_fan2_speed"] = true;
    print["heatbreak_fan_speed"] = true;
    print["wifi_signal"] = true;

    JsonObject ams = print["ams"].to<JsonObject>();
    ams["tray_now"] = true;
    JsonObject amsUnit = ams["ams"].to<JsonArray>().add<JsonObject>();
    amsUnit["humidity"] = true;
    JsonObject tray = amsUnit["tray"].to<JsonArray>().add<JsonObject>();
    tray["id"] = true;
    tray["tray_color"] = true;

    JsonObject system = filter["system"].to<JsonObject>();
    system["command"] = true;
    system["led_mode"] = true;

    JsonObject info = filter["info"].to<JsonObject>();
    info["command"] = true;
    JsonObject module = info["module"].to<JsonArray>().add<JsonObject>();
    module["name"] = true;
    module["sw_ver"] = true;

    return filter;
}

// Commands that carry nothing we care about but arrive in bursts.
static bool mqttCommandIsNoise(const char *cmd)
{
    if (cmd == NULL)
        return false;
    return (!strcmp(cmd, "gcode_line") || !strcmp(cmd, "project_prepare") ||
            !strcmp(cmd, "project_file") || !strcmp(cmd, "clean_print_error") ||
            !strcmp(cmd, "resume") || !strcmp(cmd, "get_accessories") ||
            !strcmp(cmd, "prepare") || !strcmp(cmd, "extrusion_cali_get"));
}

static void ParseCallback(char *topic, byte *payload, unsigned int length)
{
    (void)topic;

    JsonDocument messageobject;
    DeserializationError err = deserializeJson(messageobject, payload, length,
                                               DeserializationOption::Filter(mqttParseFilter()));
    if (err)
    {
        Serial.print(F("[MQTT] deserialize error: "));
        Serial.println(err.c_str());
        return;
    }
    if (messageobject.size() == 0)
        return; // nothing we watch survived the filter

    JsonObjectConst print = messageobject["print"];
    if (!print.isNull() && mqttCommandIsNoise(print["command"]))
        return;

    if (printerConfig.debugMqtt)
    {
        // Per-message output: Serial only, never WebSerial (REVIEW #7).
        Serial.printf("[MQTT] (filtered, %u raw bytes) ", (unsigned)length);
        serializeJson(messageobject, Serial);
        Serial.println();
    }

    // Work on a private copy: only this task writes printerState, so a
    // read-modify-write is safe and keeps the critical sections to two memcpys.
    PrinterState ns, orig;
    STATE_LOCK();
    memcpy(&ns, &printerState, sizeof(PrinterState));
    STATE_UNLOCK();
    memcpy(&orig, &ns, sizeof(PrinterState));

    bool activity = false; // "user visible" change -> wakes the LEDs / idle timer
    unsigned long now = millis();

    // ---- info.get_version reply -------------------------------------------
    JsonObjectConst info = messageobject["info"];
    if (!info.isNull() && info["command"] == "get_version")
    {
        for (JsonObjectConst module : info["module"].as<JsonArrayConst>())
        {
            const char *name = module["name"];
            if (name != NULL && strcmp(name, "ota") == 0)
            {
                strlcpy(ns.printerFw, module["sw_ver"] | "", sizeof(ns.printerFw));
                break;
            }
        }
    }

    // ---- system.ledctrl fast path (arrives before the slower push_status) ---
    JsonObjectConst system = messageobject["system"];
    if (!system.isNull() && system["command"] == "ledctrl")
    {
        bool on = (system["led_mode"] == "on");
        if (ns.chamberLight != on)
        {
            ns.chamberLight = on;
            activity = true;
        }
        mqttFastPathMs = now;
    }

    if (!print.isNull())
    {
        // ---- door / home flag ---------------------------------------------
        if (!print["home_flag"].isNull())
        {
            uint32_t hf = (uint32_t)print["home_flag"].as<int32_t>(); // signed on the wire
            ns.homeFlag = hf;
            bool doorState = (hf & HOME_FLAG_DOOR_OPEN) != 0;
            if (doorState != ns.doorOpen)
            {
                ns.doorOpen = doorState;
                ns.doorEdgeCount++;
                if (doorState)
                    ns.lastDoorOpenMs = now;
                else
                    ns.lastDoorCloseMs = now;
                activity = true;
                if (printerConfig.debugChanges)
                    Serial.printf("[MQTT] Door %s\n", doorState ? "opened" : "closed");
            }
        }

        // ---- stage ---------------------------------------------------------
        if (!print["stg_cur"].isNull())
        {
            int16_t stage = (int16_t)print["stg_cur"].as<int>();
            if (stage != ns.stage)
            {
                ns.stage = stage;
                activity = true;
                if (printerConfig.debugChanges)
                    Serial.printf("[MQTT] stg_cur %d (%s)\n", stage, stageName(stage));
            }
        }

        // ---- gcode_state ---------------------------------------------------
        if (!print["gcode_state"].isNull() && (now - mqttFastPathMs) > MQTT_FASTPATH_HOLD_MS)
        {
            const char *gs = print["gcode_state"];
            if (gs != NULL && strcmp(gs, ns.gcodeState) != 0)
            {
                strlcpy(ns.gcodeState, gs, sizeof(ns.gcodeState));
                activity = true;
                // clear an HMS-derived stage override on every state transition
                if (!strcmp(gs, "RUNNING") || !strcmp(gs, "IDLE") || !strcmp(gs, "FINISH") || !strcmp(gs, "FAILED"))
                    ns.overrideStage = 999;
                if (printerConfig.debugChanges)
                    Serial.printf("[MQTT] gcode_state %s\n", gs);
            }
        }

        // ---- explicit pause command (faster than the status message) -------
        if (print["command"] == "pause")
        {
            strlcpy(ns.gcodeState, "PAUSE", sizeof(ns.gcodeState));
            mqttFastPathMs = now;
            activity = true;
            Serial.println(F("[MQTT] manual PAUSE"));
        }

        // ---- job progress / metadata ---------------------------------------
        if (!print["mc_percent"].isNull())
            ns.progress = (uint8_t)constrain(print["mc_percent"].as<int>(), 0, 100);
        if (!print["mc_remaining_time"].isNull())
            ns.remainingMin = (uint32_t)max(0, print["mc_remaining_time"].as<int>());
        if (!print["layer_num"].isNull())
            ns.layer = (uint16_t)max(0, print["layer_num"].as<int>());
        if (!print["total_layer_num"].isNull())
            ns.totalLayers = (uint16_t)max(0, print["total_layer_num"].as<int>());
        if (!print["print_error"].isNull())
            ns.printError = print["print_error"].as<int32_t>();
        if (!print["spd_lvl"].isNull())
            ns.speedLevel = (uint8_t)constrain(print["spd_lvl"].as<int>(), 0, 9);
        if (!print["sdcard"].isNull())
            ns.sdcard = print["sdcard"].as<bool>();
        if (!print["print_type"].isNull())
            strlcpy(ns.printType, print["print_type"] | "", sizeof(ns.printType));
        if (!print["subtask_name"].isNull())
        {
            const char *name = print["subtask_name"];
            if (name != NULL && name[0] != '\0')
                strlcpy(ns.jobName, name, sizeof(ns.jobName));
        }
        if (ns.jobName[0] == '\0' && !print["gcode_file"].isNull())
        {
            const char *file = print["gcode_file"];
            if (file != NULL)
            {
                const char *slash = strrchr(file, '/');
                strlcpy(ns.jobName, slash ? slash + 1 : file, sizeof(ns.jobName));
            }
        }

        // ---- temperatures (chamber_temper may be null) ----------------------
        if (!print["nozzle_temper"].isNull())
            ns.nozzleTemp = print["nozzle_temper"].as<float>();
        if (!print["nozzle_target_temper"].isNull())
            ns.nozzleTarget = print["nozzle_target_temper"].as<float>();
        if (!print["bed_temper"].isNull())
            ns.bedTemp = print["bed_temper"].as<float>();
        if (!print["bed_target_temper"].isNull())
            ns.bedTarget = print["bed_target_temper"].as<float>();
        if (!print["chamber_temper"].isNull())
            ns.chamberTemp = print["chamber_temper"].as<float>();

        // ---- fans (strings "0".."15") ---------------------------------------
        if (!print["cooling_fan_speed"].isNull())
            ns.fanPart = fanToPercent(print["cooling_fan_speed"]);
        if (!print["big_fan1_speed"].isNull())
            ns.fanAux = fanToPercent(print["big_fan1_speed"]);
        if (!print["big_fan2_speed"].isNull())
            ns.fanChamber = fanToPercent(print["big_fan2_speed"]);
        if (!print["heatbreak_fan_speed"].isNull())
            ns.fanHeatbreak = fanToPercent(print["heatbreak_fan_speed"]);

        // ---- printer's own wifi rssi ("-30dBm") ------------------------------
        if (!print["wifi_signal"].isNull())
        {
            const char *sig = print["wifi_signal"];
            if (sig != NULL)
                ns.wifiSignal = (int8_t)constrain(atoi(sig), -127, 0);
        }

        // ---- AMS summary -----------------------------------------------------
        JsonObjectConst ams = print["ams"];
        if (!ams.isNull())
        {
            int trayNow = 255;
            if (!ams["tray_now"].isNull())
                trayNow = ams["tray_now"].as<int>();
            ns.amsTrayNow = (trayNow >= 0 && trayNow < 250) ? (int8_t)trayNow : (int8_t)-1;

            JsonArrayConst units = ams["ams"];
            ns.amsPresent = !units.isNull() && units.size() > 0;
            if (ns.amsPresent)
            {
                JsonObjectConst unit0 = units[0];
                ns.amsHumidity = (uint8_t)constrain(unit0["humidity"].as<int>(), 0, 5);
                if (ns.amsTrayNow >= 0)
                {
                    for (JsonObjectConst t : unit0["tray"].as<JsonArrayConst>())
                    {
                        if (t["id"].as<int>() != (ns.amsTrayNow % 4))
                            continue;
                        const char *col = t["tray_color"]; // "RRGGBBAA"
                        if (col != NULL && strlen(col) >= 6)
                            snprintf(ns.amsTrayColor, sizeof(ns.amsTrayColor), "#%.6s", col);
                        break;
                    }
                }
            }
        }

        // ---- lights ----------------------------------------------------------
        if (!print["lights_report"].isNull() && (now - mqttFastPathMs) > MQTT_FASTPATH_HOLD_MS)
        {
            for (JsonObjectConst light : print["lights_report"].as<JsonArrayConst>())
            {
                const char *node = light["node"];
                const char *mode = light["mode"];
                if (node == NULL || mode == NULL)
                    continue;
                if (!strcmp(node, "chamber_light"))
                {
                    bool on = !strcmp(mode, "on");
                    if (ns.chamberLight != on)
                    {
                        ns.chamberLight = on;
                        activity = true;
                        if (printerConfig.debugChanges)
                            Serial.printf("[MQTT] chamber_light %s\n", mode);
                    }
                }
                else if (!strcmp(node, "work_light"))
                {
                    bool on = (!strcmp(mode, "on") || !strcmp(mode, "flashing"));
                    if (ns.workLight != on)
                    {
                        ns.workLight = on;
                        activity = true;
                    }
                }
            }
        }

        // ---- HMS --------------------------------------------------------------
        if (!print["hms"].isNull())
        {
            char ignoreList[sizeof(printerConfig.hmsIgnoreList)];
            STATE_LOCK();
            memcpy(ignoreList, printerConfig.hmsIgnoreList, sizeof(ignoreList));
            STATE_UNLOCK();

            memset(ns.hms, 0, sizeof(ns.hms));
            ns.hmsCount = 0;
            uint8_t highest = 0;
            uint64_t highestCode = 0;
            int16_t ovrStage = 999;

            for (JsonObjectConst e : print["hms"].as<JsonArrayConst>())
            {
                uint32_t attr = e["attr"].as<uint32_t>();
                uint32_t code = e["code"].as<uint32_t>();
                uint64_t full = ((uint64_t)attr << 32) | (uint64_t)code;
                uint8_t severity = ParseHMSSeverity(code);
                if (severity == 0)
                    continue;

                char text[24];
                hmsFormatCode(full, text, sizeof(text));
                bool ignored = hmsCodeIsIgnored(text, ignoreList);

                if (ns.hmsCount < HMS_MAX)
                {
                    HmsEntry &entry = ns.hms[ns.hmsCount++];
                    entry.code = full;
                    entry.severity = severity;
                    entry.module = (uint8_t)(attr >> 24);
                    entry.ignored = ignored;
                }
                if (!ignored)
                {
                    if (highest == 0 || severity < highest)
                    {
                        highest = severity; // 1 = fatal is the most severe
                        highestCode = full;
                    }
                    int16_t mapped = hmsOverrideStageFor(full);
                    if (mapped != 999)
                        ovrStage = mapped;
                }
            }

            // most severe first (insertion sort over at most 8 entries)
            for (uint8_t i = 1; i < ns.hmsCount; i++)
            {
                HmsEntry key = ns.hms[i];
                int8_t j = (int8_t)i - 1;
                while (j >= 0 && ns.hms[j].severity > key.severity)
                {
                    ns.hms[j + 1] = ns.hms[j];
                    j--;
                }
                ns.hms[j + 1] = key;
            }

            if (highest != ns.hmsHighestSeverity || highestCode != ns.hmsHighestCode ||
                ovrStage != ns.overrideStage)
            {
                activity = true;
                if (printerConfig.debugChanges || printerConfig.debugVerbose)
                {
                    char text[24];
                    hmsFormatCode(highestCode, text, sizeof(text));
                    Serial.printf("[MQTT] HMS highest: %s %s (override stage %d)\n",
                                  hmsSeverityName(highest), highest ? text : "-", ovrStage);
                }
            }
            ns.hmsHighestSeverity = highest;
            ns.hmsHighestCode = highestCode;
            ns.overrideStage = ovrStage;
        }
    }

    if (activity)
        ns.activityCount++;

    bool changed = (memcmp(&ns, &orig, sizeof(PrinterState)) != 0);
    ns.lastReportMs = now;

    STATE_LOCK();
    memcpy(&printerState, &ns, sizeof(PrinterState));
    STATE_UNLOCK();

    if (changed)
    {
        printerStateDirty = true;
        if (mqttPublishStateChanged)
            mqttPublishStateChanged();
    }
}

static void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    (void)payload;
    (void)length;
    if (stream.overflowed())
        Serial.println(F("[MQTT] payload exceeded the buffer limit - message dropped"));
    else
        ParseCallback(topic, (byte *)stream.get_buffer(), (unsigned int)stream.current_length());
    stream.flush();
}

// ---------------------------------------------------------------------------
// Connection handling (mqttTask only)
// ---------------------------------------------------------------------------

static void mqttSetOnline(bool online, int state)
{
    STATE_LOCK();
    printerState.online = online;
    printerState.mqttState = state;
    if (online)
        printerState.disconnectMs = 0;
    else if (printerState.disconnectMs == 0)
        printerState.disconnectMs = millis();
    STATE_UNLOCK();
    printerStateDirty = true;
    ledDirty = true;
}

static void connectMqtt()
{
    if (WiFi.status() != WL_CONNECTED || WiFi.getMode() != WIFI_MODE_STA)
        return;
    if (strlen(printerConfig.printerIP) == 0 || strlen(printerConfig.accessCode) == 0)
        return; // nothing to connect to yet
    if ((millis() - mqttAttemptMs) < MQTT_RETRY_MS)
        return;

    mqttAttemptMs = millis();
    mqttClient.setServer(printerConfig.printerIP, MQTT_PORT);

    if (mqttClient.connect(mqttClientId, "bblp", printerConfig.accessCode))
    {
        LogSerial.printf("[MQTT] Connected to %s, subscribing to %s\n", printerConfig.printerIP, mqttReportTopic);
        mqttClient.subscribe(mqttReportTopic);
        mqttConnectedMs = millis();
        mqttSetOnline(true, mqttClient.state());
        STATE_LOCK();
        printerState.reconnects++;
        STATE_UNLOCK();

        // P1/A1 only push deltas: ask for the full state right away.
        mqttEnqueue(MqttCmd::PushAll, 1);
        mqttEnqueue(MqttCmd::GetVersion, 0);
    }
    else
    {
        int state = mqttClient.state();
        mqttSetOnline(false, state);
        LogSerial.print(F("[MQTT] Connect failed: "));
        ParseMQTTState(state);
    }
}

static void mqttDrainCommands()
{
    MqttCmdItem item;
    while (mqttDequeue(item))
    {
        switch (item.cmd)
        {
        case MqttCmd::ChamberLight: mqttSendLightControl("chamber_light", item.arg != 0); break;
        case MqttCmd::WorkLight: mqttSendLightControl("work_light", item.arg != 0); break;
        case MqttCmd::PushAll: mqttSendPushAll(item.arg != 0); break;
        case MqttCmd::GetVersion: mqttSendGetVersion(); break;
        case MqttCmd::Reconnect:
            LogSerial.println(F("[MQTT] Reconnect requested"));
            mqttClient.disconnect();
            mqttAttemptMs = 0;
            mqttSetOnline(false, mqttClient.state());
            break;
        case MqttCmd::None:
        default:
            break;
        }
    }
}

static void mqttTask(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        if (WiFi.status() != WL_CONNECTED || WiFi.getMode() != WIFI_MODE_STA)
        {
            if (printerState.online)
                mqttSetOnline(false, mqttClient.state());
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!mqttClient.connected())
        {
            if (printerState.online)
            {
                LogSerial.println(F("[MQTT] Disconnected"));
                mqttSetOnline(false, mqttClient.state());
            }
            connectMqtt();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        mqttClient.loop();
        mqttDrainCommands();

        // Still no gcode_state 10 s after connecting? Ask for a full push.
        if (printerState.gcodeState[0] == '\0' && (millis() - mqttConnectedMs) > MQTT_STATE_UNKNOWN_MS)
            mqttSendPushAll(false);

        // External broker / Home Assistant (api workstream; weak symbol).
        if (mqttPublishLoop)
            mqttPublishLoop();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setupMqtt()
{
    snprintf(mqttClientId, sizeof(mqttClientId), "BLLED-%04X", (unsigned)random(0xffff));
    snprintf(mqttReportTopic, sizeof(mqttReportTopic), "device/%s/report", printerConfig.serialNumber);
    snprintf(mqttRequestTopic, sizeof(mqttRequestTopic), "device/%s/request", printerConfig.serialNumber);

    LogSerial.printf("[MQTT] Printer %s, topic %s\n", printerConfig.printerIP, mqttReportTopic);

    wifiSecureClient.setInsecure();
    wifiSecureClient.setTimeout(15);
    mqttClient.setSocketTimeout(17);
    mqttClient.setBufferSize(1024); // bounds topics + our outbound publishes only
    mqttClient.setServer(printerConfig.printerIP, MQTT_PORT);
    mqttClient.setStream(stream);
    mqttClient.setCallback(mqttCallback);

    STATE_LOCK();
    printerState.disconnectMs = millis(); // start the offline grace period now
    STATE_UNLOCK();

    if (mqttTaskHandle == NULL)
    {
        BaseType_t result =
#if CONFIG_FREERTOS_UNICORE
            xTaskCreate(mqttTask, "mqttTask", TASK_RAM, NULL, 1, &mqttTaskHandle);
#else
            xTaskCreatePinnedToCore(mqttTask, "mqttTask", TASK_RAM, NULL, 1, &mqttTaskHandle, 1);
#endif
        LogSerial.println(result == pdPASS ? F("[MQTT] Task started") : F("[MQTT] Failed to create task!"));
    }

    if (setupMqttPublish)
        setupMqttPublish();
}

#endif
