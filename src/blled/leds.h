#ifndef _LED
#define _LED

// ---------------------------------------------------------------------------
// leds.h -- non-blocking LED engine + state evaluation (ARCHITECTURE.md §4)
//
// Responsibilities
//   * own the five PWM channels (GPIO 19/18/21/22/23, 5 kHz, 8 bit, core-3
//     ledcAttach/ledcWrite(pin, duty)) and write them only when a value changed.
//   * ledEvaluate(): snapshot printerState, run the priority ladder, produce a
//     LedDecision (colour + effect + reason) and hand it to the engine.
//   * ledTick(): time-based fade, effect modulation, brightness, PWM write.
//   * bookkeeping for finish indication, door edges/double-close toggle,
//     inactivity timeout and the printer's chamber light.
//   * the cross-task override/identify API declared in types.h.
//
// Threading
//   EVERYTHING here runs on the main loop only.  Other tasks never call into
//   this file; they set ledRuntime.override*/identifyRequested under
//   STATE_LOCK() (via ledRequestOverride()/ledClearOverride()/
//   ledRequestIdentify()) and raise ledDirty.  printerState is copied into a
//   local snapshot under the lock; the lock is never held while logging.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include "types.h"
#include "stages.h"
#include "filesystem.h"
#include "logSerial.h"

// ---- hardware --------------------------------------------------------------
const int redPin = 19;
const int greenPin = 18;
const int bluePin = 21;
const int warmPin = 22;
const int coldPin = 23;
static const int LED_PINS[5] = {redPin, greenPin, bluePin, warmPin, coldPin};

const int pwmFreq = 5000;
const int pwmResolution = 8; // 8-bit PWM = 0-255

// ---- named constants (upstream had these scattered / inconsistent) ---------
#define DOOR_DOUBLE_CLOSE_MS 2000UL // close within this of an open = "double close"
#define FINISH_DOOR_WINDOW_MS 6000UL // door edge accepted as "finish acknowledged"
#define IDENTIFY_BLINKS 3 // white blinks for POST /api/led/identify
#define LED_EVAL_INTERVAL_MS 50UL // ladder re-evaluated at >= 10 Hz
#define LED_CMD_QUIET_MS 3000UL // ignore report "activity" we caused ourselves

// ---------------------------------------------------------------------------
// Engine state (main loop only)
// ---------------------------------------------------------------------------
struct LedDecision
{
    COLOR color;
    LedEffect effect = LedEffect::Solid;
    float progress = 0.0f;
    char reason[48] = "";
};

static float ledTargetVal[5] = {0, 0, 0, 0, 0};  // decision colour, pre effect/brightness
static float ledFadeFromVal[5] = {0, 0, 0, 0, 0};
static unsigned long ledFadeStartMs = 0;
static uint8_t ledLastWritten[5] = {0, 0, 0, 0, 0};
static bool ledForceWrite = true;

static unsigned long ledLastEvalMs = 0;
static unsigned long ledIdentifyStartMs = 0;
static unsigned long ledChamberCmdMs = 0; // when we last drove the chamber light
static uint32_t ledSeenActivity = 0;
static char ledLastGcodeState[12] = "";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static inline int ledEffectiveBrightness()
{
    if (ledRuntime.overrideActive && ledRuntime.overrideBrightness >= 0)
        return constrain((int)ledRuntime.overrideBrightness, 0, 100);
    return constrain(printerConfig.brightness, 0, 100);
}

// Effect period in ms for the configured speed (1 = slow .. 10 = fast).
static unsigned long ledEffectPeriod(LedEffect e)
{
    float s = (float)constrain((int)printerConfig.effectSpeed, 1, 10);
    float f = (s - 1.0f) / 9.0f; // 0 .. 1
    switch (e)
    {
    case LedEffect::Breathe: return (unsigned long)(6000.0f - f * 4500.0f);   // 6.0 s -> 1.5 s
    case LedEffect::Blink: return (unsigned long)(1200.0f - f * 900.0f);      // 1.2 s -> 0.3 s
    case LedEffect::FastBlink: return (unsigned long)(300.0f - f * 200.0f);   // 0.3 s -> 0.1 s
    case LedEffect::Rainbow: return (unsigned long)(60000.0f - f * 54000.0f); // 60 s -> 6 s
    default: return 1000;
    }
}

// Scalar 0..1 applied to every channel for the current effect.
static float ledEffectScalar(LedEffect e, unsigned long now)
{
    unsigned long period = ledEffectPeriod(e);
    if (period == 0)
        return 1.0f;
    float phase = (float)(now % period) / (float)period; // 0 .. 1
    switch (e)
    {
    case LedEffect::Breathe:
        return 0.25f + 0.75f * (0.5f + 0.5f * sinf(phase * 2.0f * (float)PI));
    case LedEffect::Blink:
    case LedEffect::FastBlink:
        return (phase < 0.5f) ? 1.0f : 0.0f;
    default:
        return 1.0f;
    }
}

// Hue (degrees) -> RGB 0..255. Always positive (upstream fed cos() straight into
// ledcWrite, so negative values wrapped to a huge duty cycle - REVIEW #13).
static void ledHueToRgb(float hue, float *rgb)
{
    hue = fmodf(hue, 360.0f);
    if (hue < 0.0f)
        hue += 360.0f;
    float c = 1.0f;
    float x = 1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    if (hue < 60) { r = c; g = x; }
    else if (hue < 120) { r = x; g = c; }
    else if (hue < 180) { g = c; b = x; }
    else if (hue < 240) { g = x; b = c; }
    else if (hue < 300) { r = x; b = c; }
    else { r = c; b = x; }
    rgb[0] = r * 255.0f;
    rgb[1] = g * 255.0f;
    rgb[2] = b * 255.0f;
}

static inline void ledColorToArray(const COLOR &c, float *out)
{
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.ww;
    out[4] = c.cw;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

void setupLeds()
{
    for (int i = 0; i < 5; i++)
    {
        ledcAttach(LED_PINS[i], pwmFreq, pwmResolution);
        ledcWrite(LED_PINS[i], 0);
        ledLastWritten[i] = 0;
        ledTargetVal[i] = 0;
        ledFadeFromVal[i] = 0;
    }
    ledFadeStartMs = millis();
    ledForceWrite = true;
    ledRuntime.inactivityStartMs = millis(); // idle timer counts from boot, not from 0
}

// One PWM update: fade -> effect -> brightness -> ledcWrite (only on change).
void ledTick()
{
    unsigned long now = millis();

    // 1) time-based fade (REVIEW #4: upstream's integer "tween" was a 256 ms no-op)
    float value[5];
    float t = 1.0f;
    if (printerConfig.fadeMs > 0)
    {
        unsigned long elapsed = now - ledFadeStartMs;
        t = (elapsed >= printerConfig.fadeMs) ? 1.0f : (float)elapsed / (float)printerConfig.fadeMs;
    }
    for (int i = 0; i < 5; i++)
        value[i] = ledFadeFromVal[i] + (ledTargetVal[i] - ledFadeFromVal[i]) * t;

    // 2) effect
    LedEffect effect = ledRuntime.effect;
    if (effect == LedEffect::Rainbow)
    {
        unsigned long period = ledEffectPeriod(LedEffect::Rainbow);
        float hue = (period > 0) ? ((float)(now % period) / (float)period) * 360.0f : 0.0f;
        float rgb[3];
        ledHueToRgb(hue, rgb);
        value[0] = rgb[0];
        value[1] = rgb[1];
        value[2] = rgb[2];
        value[3] = 0;
        value[4] = 0;
    }
    else
    {
        float m = ledEffectScalar(effect, now);
        for (int i = 0; i < 5; i++)
            value[i] *= m;
    }

    // 3) brightness last -- brightness 0 must switch everything off
    float bright = (float)ledEffectiveBrightness() / 100.0f;
    for (int i = 0; i < 5; i++)
    {
        int v = (int)lroundf(value[i] * bright);
        v = constrain(v, 0, 255);
        uint8_t duty = (uint8_t)v;
        if (ledForceWrite || duty != ledLastWritten[i])
        {
            ledcWrite(LED_PINS[i], duty);
            ledLastWritten[i] = duty;
            ledRuntime.output[i] = duty;
        }
    }
    ledForceWrite = false;
}

// Set the engine target. No-op when nothing changed (so the fade is not restarted).
static void ledApply(const LedDecision &d)
{
    bool same = (ledRuntime.targetColor.r == d.color.r && ledRuntime.targetColor.g == d.color.g &&
                 ledRuntime.targetColor.b == d.color.b && ledRuntime.targetColor.ww == d.color.ww &&
                 ledRuntime.targetColor.cw == d.color.cw && ledRuntime.effect == d.effect);

    if (!same)
    {
        // start the fade from wherever we are right now
        float now5[5];
        float t = 1.0f;
        if (printerConfig.fadeMs > 0)
        {
            unsigned long elapsed = millis() - ledFadeStartMs;
            t = (elapsed >= printerConfig.fadeMs) ? 1.0f : (float)elapsed / (float)printerConfig.fadeMs;
        }
        for (int i = 0; i < 5; i++)
            now5[i] = ledFadeFromVal[i] + (ledTargetVal[i] - ledFadeFromVal[i]) * t;

        memcpy(ledFadeFromVal, now5, sizeof(now5));
        ledColorToArray(d.color, ledTargetVal);
        ledFadeStartMs = millis();

        ledRuntime.targetColor = d.color;
        ledRuntime.effect = d.effect;
    }

    ledRuntime.progress = d.progress;

    bool reasonChanged = (strncmp(ledRuntime.reason, d.reason, sizeof(ledRuntime.reason)) != 0);
    if (reasonChanged)
    {
        strlcpy(ledRuntime.reason, d.reason, sizeof(ledRuntime.reason));
        if (printerConfig.debugChanges || printerConfig.debugVerbose)
        {
            LogSerial.printf("[LED] %s -> r:%d g:%d b:%d ww:%d cw:%d (%s, %d%%)\n",
                             d.reason, d.color.r, d.color.g, d.color.b, d.color.ww, d.color.cw,
                             ledEffectToString(d.effect), ledEffectiveBrightness());
        }
    }
    // Only hint the external publisher on a real change (this runs at 20 Hz).
    if ((!same || reasonChanged) && mqttPublishStateChanged)
        mqttPublishStateChanged();
}

// Boot sequence helper: set a colour immediately (no fade, no evaluation).
// Only used by setup() before the main loop is running.
void ledBootColor(short r, short g, short b, short ww, short cw)
{
    COLOR c;
    c.r = r; c.g = g; c.b = b; c.ww = ww; c.cw = cw;
    colorSyncHex(c);
    ledRuntime.targetColor = c;
    ledRuntime.effect = LedEffect::Solid;
    ledColorToArray(c, ledTargetVal);
    memcpy(ledFadeFromVal, ledTargetVal, sizeof(ledTargetVal));
    ledFadeStartMs = millis();
    ledForceWrite = true;
    ledTick();
}

void ledBootColor(const COLOR &c)
{
    ledBootColor(c.r, c.g, c.b, c.ww, c.cw);
}

// ---------------------------------------------------------------------------
// Cross-task API (declared in types.h). These only touch ledRuntime + ledDirty.
// ---------------------------------------------------------------------------

void ledRequestOverride(const COLOR &c, LedEffect e, uint32_t durationMs, int8_t brightness)
{
    STATE_LOCK();
    ledRuntime.overrideColor = c;
    ledRuntime.overrideEffect = e;
    ledRuntime.overrideActive = true;
    ledRuntime.overrideUntilMs = (durationMs == 0) ? 0 : (millis() + durationMs);
    ledRuntime.overrideBrightness = brightness;
    STATE_UNLOCK();
    ledDirty = true;
}

void ledClearOverride()
{
    STATE_LOCK();
    ledRuntime.overrideActive = false;
    ledRuntime.overrideUntilMs = 0;
    ledRuntime.overrideBrightness = -1;
    STATE_UNLOCK();
    ledDirty = true;
}

void ledRequestIdentify()
{
    STATE_LOCK();
    ledRuntime.identifyRequested = true;
    STATE_UNLOCK();
    ledIdentifyStartMs = 0; // (re)start on the next evaluation
    ledDirty = true;
}

// ---------------------------------------------------------------------------
// Bookkeeping (finish / door / inactivity / chamber light)
// ---------------------------------------------------------------------------

// Drive the printer's chamber light and remember when, so the resulting
// lights_report does not look like user activity and restart the idle timer.
static void ledDriveChamberLight(bool on)
{
    ledChamberCmdMs = millis();
    ledRuntime.chamberLightLocked = on;
    controlChamberLight(on);
    if (printerConfig.debugChanges)
        LogSerial.printf("[LED] Chamber light %s requested\n", on ? "ON" : "OFF");
}

static void ledUpdateRuntime(const PrinterState &st)
{
    unsigned long now = millis();

    // --- override / identify expiry (fields may be written by other tasks) ---
    STATE_LOCK();
    if (ledRuntime.overrideActive && ledRuntime.overrideUntilMs != 0 &&
        (long)(now - ledRuntime.overrideUntilMs) >= 0)
    {
        ledRuntime.overrideActive = false;
        ledRuntime.overrideUntilMs = 0;
        ledRuntime.overrideBrightness = -1;
    }
    bool identify = ledRuntime.identifyRequested;
    STATE_UNLOCK();

    if (identify)
    {
        if (ledIdentifyStartMs == 0)
            ledIdentifyStartMs = now;
        if (now - ledIdentifyStartMs >= IDENTIFY_BLINKS * ledEffectPeriod(LedEffect::Blink))
        {
            STATE_LOCK();
            ledRuntime.identifyRequested = false;
            STATE_UNLOCK();
            ledIdentifyStartMs = 0;
        }
    }

    // --- MQTT activity: any meaningful report change wakes the LEDs up -------
    if (st.activityCount != ledSeenActivity)
    {
        bool selfInflicted = (now - ledChamberCmdMs) < LED_CMD_QUIET_MS;
        ledSeenActivity = st.activityCount;
        if (!selfInflicted)
        {
            ledRuntime.inactivityStartMs = now;
            ledRuntime.idleOffActive = false;
        }
    }

    // --- gcode_state transitions -------------------------------------------
    if (strcmp(st.gcodeState, ledLastGcodeState) != 0)
    {
        // The first state we ever see is the printer's *current* state, not a
        // transition: a printer that finished yesterday must not light up green
        // at boot (upstream seeded gcodeState="FINISH" for the same reason).
        bool initial = (ledLastGcodeState[0] == '\0');
        bool toFinish = !initial && (strcmp(st.gcodeState, "FINISH") == 0);
        bool toRunning = !initial && (strcmp(st.gcodeState, "RUNNING") == 0);
        strlcpy(ledLastGcodeState, st.gcodeState, sizeof(ledLastGcodeState));

        ledRuntime.inactivityStartMs = now;
        ledRuntime.idleOffActive = false;

        if (toFinish)
        {
            ledRuntime.finishActive = true;
            ledRuntime.finishStartMs = now;
        }
        if (toRunning)
        {
            ledRuntime.finishActive = false;
            ledRuntime.doorToggleOff = false;
            if (printerConfig.controlChamberLight && !st.chamberLight)
                ledDriveChamberLight(true); // light the chamber at print start
        }
    }

    // never idle-out while the printer is actually doing something
    if (strcmp(st.gcodeState, "RUNNING") == 0 || strcmp(st.gcodeState, "PAUSE") == 0)
    {
        ledRuntime.inactivityStartMs = now;
        ledRuntime.idleOffActive = false;
    }

    // --- door edges ---------------------------------------------------------
    if (st.doorEdgeCount != ledRuntime.seenDoorEdges)
    {
        ledRuntime.seenDoorEdges = st.doorEdgeCount;
        ledRuntime.inactivityStartMs = now;
        ledRuntime.idleOffActive = false;

        bool doubleClose = (!st.doorOpen && st.lastDoorOpenMs != 0 &&
                            st.lastDoorCloseMs >= st.lastDoorOpenMs &&
                            (st.lastDoorCloseMs - st.lastDoorOpenMs) < DOOR_DOUBLE_CLOSE_MS);

        if (printerConfig.doorToggleEnabled && doubleClose && !printerConfig.isP1Printer)
        {
            // closed twice within 2 s -> flip the LED bar (upstream behaviour)
            ledRuntime.doorToggleOff = !ledRuntime.doorToggleOff;
            if (ledRuntime.doorToggleOff)
                ledRuntime.idleOffActive = true;
            if (printerConfig.controlChamberLight)
                ledDriveChamberLight(!ledRuntime.doorToggleOff);
            if (printerConfig.debugChanges)
                LogSerial.printf("[LED] Door double-close -> LEDs %s\n", ledRuntime.doorToggleOff ? "OFF" : "ON");
        }
        else
        {
            ledRuntime.doorToggleOff = false; // any door interaction wakes the LEDs
            if (printerConfig.controlChamberLight && st.doorOpen && !st.chamberLight)
                ledDriveChamberLight(true);
        }

        // a door edge acknowledges the finish indication
        if (ledRuntime.finishActive && printerConfig.finishExitMode == FinishExitMode::Door &&
            (now - st.lastDoorCloseMs < FINISH_DOOR_WINDOW_MS || now - st.lastDoorOpenMs < FINISH_DOOR_WINDOW_MS))
        {
            ledRuntime.finishActive = false;
            ledRuntime.inactivityStartMs = now;
            if (printerConfig.debugChanges)
                LogSerial.println(F("[LED] Finish acknowledged by door - idle timer restarted"));
        }
    }

    // --- finish timer -------------------------------------------------------
    bool finishByTimer = (printerConfig.finishExitMode == FinishExitMode::Timer || printerConfig.isP1Printer);
    if (ledRuntime.finishActive && finishByTimer &&
        (now - ledRuntime.finishStartMs) > ((unsigned long)printerConfig.finishTimerMins * 60000UL))
    {
        ledRuntime.finishActive = false;
        ledRuntime.inactivityStartMs = now;
        ledRuntime.idleOffActive = false;
        if (printerConfig.controlChamberLight)
            ledDriveChamberLight(false);
        if (printerConfig.debugChanges)
            LogSerial.println(F("[LED] Finish timer expired - idle timer restarted"));
    }

    // --- inactivity timeout -------------------------------------------------
    bool idleStage = (st.stage == -1 || st.stage == 255);
    if (printerConfig.inactivityEnabled && idleStage && !ledRuntime.finishActive &&
        !ledRuntime.idleOffActive &&
        (now - ledRuntime.inactivityStartMs) > ((unsigned long)printerConfig.inactivityMins * 60000UL))
    {
        ledRuntime.idleOffActive = true;
        if (printerConfig.controlChamberLight)
            ledDriveChamberLight(false);
        if (printerConfig.debugChanges || printerConfig.debugVerbose)
            LogSerial.printf("[LED] Idle timeout (%u min) - LEDs off\n", (unsigned)printerConfig.inactivityMins);
    }
}

// ---------------------------------------------------------------------------
// Priority ladder (ARCHITECTURE.md §4.1). First match wins.
// ---------------------------------------------------------------------------

static inline void ledSetDecision(LedDecision &d, const COLOR &c, LedEffect e, const char *reason)
{
    d.color = c;
    d.effect = e;
    strlcpy(d.reason, reason, sizeof(d.reason));
}

static COLOR ledBlend(const COLOR &a, const COLOR &b, float f)
{
    f = constrain(f, 0.0f, 1.0f);
    COLOR out;
    out.r = (short)lroundf(a.r + (b.r - a.r) * f);
    out.g = (short)lroundf(a.g + (b.g - a.g) * f);
    out.b = (short)lroundf(a.b + (b.b - a.b) * f);
    out.ww = (short)lroundf(a.ww + (b.ww - a.ww) * f);
    out.cw = (short)lroundf(a.cw + (b.cw - a.cw) * f);
    colorSyncHex(out);
    return out;
}

static COLOR ledScale(const COLOR &a, float f)
{
    COLOR out;
    out.r = (short)lroundf(a.r * f);
    out.g = (short)lroundf(a.g * f);
    out.b = (short)lroundf(a.b * f);
    out.ww = (short)lroundf(a.ww * f);
    out.cw = (short)lroundf(a.cw * f);
    colorSyncHex(out);
    return out;
}

static const COLOR LED_BLACK = {0, 0, 0, 0, 0, "#000000"};

// The colour shown while printing, honouring printingVisual.
static COLOR ledPrintingColor(const PrinterState &st, LedDecision &d)
{
    if (printerConfig.printingVisual == PrintingVisual::ProgressBlend)
    {
        d.progress = constrain((float)st.progress / 100.0f, 0.0f, 1.0f);
        return ledBlend(printerConfig.runningColor, printerConfig.finishColor, d.progress);
    }
    if (printerConfig.printingVisual == PrintingVisual::Breathe)
        d.effect = LedEffect::Breathe;
    return printerConfig.runningColor;
}

// The colour shown while preheating, honouring preheatVisual.
static COLOR ledPreheatColor(const PrinterState &st)
{
    if (printerConfig.preheatVisual != PreheatVisual::TempGlow)
        return printerConfig.runningColor;

    float ratio = 0.0f;
    if (!isnan(st.nozzleTemp) && !isnan(st.nozzleTarget) && st.nozzleTarget > 1.0f)
        ratio = max(ratio, st.nozzleTemp / st.nozzleTarget);
    if (!isnan(st.bedTemp) && !isnan(st.bedTarget) && st.bedTarget > 1.0f)
        ratio = max(ratio, st.bedTemp / st.bedTarget);
    ratio = constrain(ratio, 0.0f, 1.0f);

    COLOR c = ledScale(printerConfig.runningColor, 0.15f + 0.85f * ratio);
    if (ratio < 0.30f)
        c.r = (short)constrain((int)c.r + 60, 0, 255); // cold = slight red tint
    colorSyncHex(c);
    return c;
}

static void ledDecide(const PrinterState &st, LedDecision &d)
{
    unsigned long now = millis();
    char buf[48];

    // 1 - LED mode off
    if (printerConfig.ledMode == LedMode::Off)
    {
        ledSetDecision(d, LED_BLACK, LedEffect::Solid, "LED mode: off");
        return;
    }

    // 2 - API/MQTT override
    if (ledRuntime.overrideActive)
    {
        ledSetDecision(d, ledRuntime.overrideColor, ledRuntime.overrideEffect, "Manual override");
        return;
    }

    // 3 - identify (3 white blinks; the Blink effect is not affected by the fade)
    if (ledRuntime.identifyRequested)
    {
        COLOR white;
        white.r = 255; white.g = 255; white.b = 255; white.ww = 255; white.cw = 255;
        colorSyncHex(white);
        ledSetDecision(d, white, LedEffect::Blink, "Identify");
        return;
    }

    // 4 - maintenance mode
    if (printerConfig.ledMode == LedMode::Maintenance)
    {
        ledSetDecision(d, printerConfig.maintenanceColor, LedEffect::Solid, "Maintenance mode");
        return;
    }

    // 5 - test colour
    if (printerConfig.ledMode == LedMode::Test)
    {
        ledSetDecision(d, printerConfig.testColor, LedEffect::Solid, "Test colour");
        return;
    }

    // 6 - WiFi strength visualisation (upstream thresholds)
    if (printerConfig.ledMode == LedMode::WifiStrength)
    {
        COLOR c;
        c.ww = 0; c.cw = 0;
        if (WiFi.status() != WL_CONNECTED)
        {
            c.r = 0; c.g = 0; c.b = 255; // blue = no link
        }
        else
        {
            long rssi = WiFi.RSSI();
            if (rssi >= -50) { c.r = 0; c.g = 255; c.b = 0; }
            else if (rssi >= -60) { c.r = 128; c.g = 255; c.b = 0; }
            else if (rssi >= -70) { c.r = 255; c.g = 255; c.b = 0; }
            else if (rssi >= -80) { c.r = 255; c.g = 128; c.b = 0; }
            else { c.r = 255; c.g = 0; c.b = 0; }
        }
        colorSyncHex(c);
        snprintf(buf, sizeof(buf), "WiFi strength %d dBm", (int)WiFi.RSSI());
        ledSetDecision(d, c, LedEffect::Solid, buf);
        return;
    }

    // 7 - rainbow cycle
    if (printerConfig.ledMode == LedMode::Rainbow)
    {
        ledSetDecision(d, printerConfig.runningColor, LedEffect::Rainbow, "Rainbow cycle");
        return;
    }

    // 8 - still booting / connecting / setup portal
    if (!globalVariables.started)
    {
        if (globalVariables.apMode)
        {
            // upstream showed pink while the setup access point is running
            COLOR pink;
            pink.r = 100; pink.g = 0; pink.b = 100; pink.ww = 0; pink.cw = 0;
            colorSyncHex(pink);
            ledSetDecision(d, pink, LedEffect::Solid, "Setup access point");
            return;
        }
        ledSetDecision(d, printerConfig.wifiColor, LedEffect::Solid, "Starting up");
        return;
    }

    // 9 - toggled off by a door double-close
    if (ledRuntime.doorToggleOff)
    {
        ledSetDecision(d, LED_BLACK, LedEffect::Solid, "Toggled off via door");
        return;
    }

    // 10 - errors
    if (printerConfig.errorDetection)
    {
        int stage = st.stage;
        int ovr = st.overrideStage;
        if (stage == 6 || ovr == 6)
        {
            ledSetDecision(d, printerConfig.filamentRunoutColor, printerConfig.errorEffect, "Stage 6: filament runout");
            return;
        }
        if (stage == 17 || ovr == 17)
        {
            ledSetDecision(d, printerConfig.frontCoverColor, printerConfig.errorEffect, "Stage 17: front cover");
            return;
        }
        if (stage == 20 || ovr == 20)
        {
            ledSetDecision(d, printerConfig.nozzleTempColor, printerConfig.errorEffect, "Stage 20: nozzle temp fail");
            return;
        }
        if (stage == 21 || ovr == 21)
        {
            ledSetDecision(d, printerConfig.bedTempColor, printerConfig.errorEffect, "Stage 21: bed temp fail");
            return;
        }
        // HMS: highest severity across the whole list (REVIEW #9)
        if (st.hmsHighestSeverity == 1)
        {
            ledSetDecision(d, printerConfig.hmsFatalColor, printerConfig.errorEffect, "Printer alert: fatal");
            return;
        }
        if (st.hmsHighestSeverity == 2)
        {
            ledSetDecision(d, printerConfig.hmsSeriousColor, printerConfig.errorEffect, "Printer alert: serious");
            return;
        }
        if (st.hmsHighestSeverity == 3 && printerConfig.hmsCommonEnabled)
        {
            ledSetDecision(d, printerConfig.hmsCommonColor, printerConfig.errorEffect, "Printer alert: common");
            return;
        }
    }

    // 11 - paused (most specific stage first)
    if (st.stage == 34)
    {
        ledSetDecision(d, printerConfig.firstLayerColor, printerConfig.pauseEffect, "Stage 34: first layer error");
        return;
    }
    if (st.stage == 35)
    {
        ledSetDecision(d, printerConfig.nozzleClogColor, printerConfig.pauseEffect, "Stage 35: nozzle clog");
        return;
    }
    if (st.stage == 16 || st.stage == 30 || strcmp(st.gcodeState, "PAUSE") == 0)
    {
        ledSetDecision(d, printerConfig.pauseColor, printerConfig.pauseEffect, "Paused");
        return;
    }

    // 12 - printer offline for longer than the grace period
    if (!st.online && st.disconnectMs != 0 &&
        (now - st.disconnectMs) >= ((unsigned long)printerConfig.offlineTimeoutSec * 1000UL))
    {
        ledSetDecision(d, LED_BLACK, LedEffect::Solid, "Printer offline");
        return;
    }

    // 13 - follow the printer's chamber light
    if (printerConfig.followChamberLight && !st.chamberLight)
    {
        ledSetDecision(d, LED_BLACK, LedEffect::Solid, "Chamber light off");
        return;
    }

    // 14 - lidar / calibration stages
    {
        const COLOR *stageColor = NULL;
        const char *stageReason = NULL;
        switch (st.stage)
        {
        case 14: stageColor = &printerConfig.stage14Color; stageReason = "Stage 14: cleaning nozzle"; break;
        case 1: stageColor = &printerConfig.stage1Color; stageReason = "Stage 1: bed levelling"; break;
        case 8: stageColor = &printerConfig.stage8Color; stageReason = "Stage 8: calibrating extrusion"; break;
        case 9: stageColor = &printerConfig.stage9Color; stageReason = "Stage 9: scanning bed surface"; break;
        case 10: stageColor = &printerConfig.stage10Color; stageReason = "Stage 10: first layer inspection"; break;
        case 12: stageColor = &printerConfig.stage10Color; stageReason = "Stage 12: calibrating lidar"; break;
        default: break;
        }
        if (stageColor == NULL && st.overrideStage == 10)
        {
            stageColor = &printerConfig.stage10Color;
            stageReason = "Alert 0C00: first layer inspection";
        }
        // P1-series printers have no Micro Lidar: never dim for these stages.
        if (stageColor != NULL && !printerConfig.isP1Printer &&
            (printerConfig.lidarStagesEnabled || !colorIsBlack(*stageColor)))
        {
            ledSetDecision(d, *stageColor, LedEffect::Solid, stageReason);
            return;
        }
    }

    // 15 - inactivity timeout
    if (ledRuntime.idleOffActive)
    {
        ledSetDecision(d, LED_BLACK, LedEffect::Solid, "Idle timeout");
        return;
    }

    // 16 - finish indication
    if (ledRuntime.finishActive && printerConfig.finishIndication)
    {
        ledSetDecision(d, printerConfig.finishColor, printerConfig.finishEffect, "Print finished");
        return;
    }

    // 17 - preheating
    if (st.stage == 2 || st.stage == 7)
    {
        d.effect = LedEffect::Solid;
        COLOR c = ledPreheatColor(st);
        ledSetDecision(d, c, d.effect, st.stage == 2 ? "Stage 2: preheating bed" : "Stage 7: heating hotend");
        return;
    }

    // 18 - printing (main print stage or a RUNNING sub-stage)
    bool runningSubStage = (st.stage == 3 || st.stage == 4 || st.stage == 11 || st.stage == 13 ||
                            st.stage == 15 || st.stage == 19 || st.stage == 22 || st.stage == 24);
    if ((st.stage == 0 && strcmp(st.gcodeState, "RUNNING") == 0) ||
        (runningSubStage && strcmp(st.gcodeState, "RUNNING") == 0))
    {
        d.effect = LedEffect::Solid;
        COLOR c = ledPrintingColor(st, d);
        snprintf(buf, sizeof(buf), "Printing (stage %d)", st.stage);
        ledSetDecision(d, c, d.effect, buf);
        return;
    }

    // 19 - idle / finished-but-acknowledged / failed / preparing / offline
    bool idleStage = (st.stage == -1 || st.stage == 255 || st.stage == -2);
    bool idleGcode = (st.gcodeState[0] == '\0' || strcmp(st.gcodeState, "IDLE") == 0 ||
                      strcmp(st.gcodeState, "FINISH") == 0 || strcmp(st.gcodeState, "FAILED") == 0 ||
                      strcmp(st.gcodeState, "PREPARE") == 0 || strcmp(st.gcodeState, "SLICING") == 0 ||
                      strcmp(st.gcodeState, "INIT") == 0 || strcmp(st.gcodeState, "OFFLINE") == 0);
    if (idleStage || idleGcode)
    {
        snprintf(buf, sizeof(buf), "%s (stage %d)",
                 st.gcodeState[0] ? st.gcodeState : "Idle", st.stage);
        ledSetDecision(d, printerConfig.runningColor, LedEffect::Solid, buf);
        return;
    }

    // 20 - nothing matched: keep whatever is on the strip
    ledSetDecision(d, ledRuntime.targetColor, ledRuntime.effect, "No rule");
}

// Snapshot -> bookkeeping -> ladder -> engine. Called from the main loop.
void ledEvaluate()
{
    PrinterState snapshot;
    STATE_LOCK();
    memcpy(&snapshot, &printerState, sizeof(PrinterState));
    STATE_UNLOCK();

    ledUpdateRuntime(snapshot);

    LedDecision decision;
    ledDecide(snapshot, decision);
    ledApply(decision);

    ledLastEvalMs = millis();
}

// Convenience for the main loop: evaluate when something changed or at >=10 Hz.
void ledLoop()
{
    if (ledDirty || printerStateDirty || (millis() - ledLastEvalMs) >= LED_EVAL_INTERVAL_MS)
    {
        ledDirty = false;
        ledEvaluate();
    }
    ledTick();
}

#endif
