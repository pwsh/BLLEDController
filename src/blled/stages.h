#ifndef _BLLED_STAGES
#define _BLLED_STAGES

// Name tables shared by the LED logic, the status API and Home Assistant discovery.
// Sources: ha-bambulab CURRENT_STAGE_IDS (36-77 verified against the source on 2026-08-26) / GCODE_STATE_OPTIONS / HMS_SEVERITY_LEVELS / HMS_MODULES,
// OpenBambuAPI mqtt.md. Codes 36+ belong to newer (H2D-era) firmware and are best-effort.

#include <Arduino.h>

struct StageName
{
    int16_t code;
    const char *name;
};

static const StageName STAGE_NAMES[] = {
    {-2, "Offline"},
    {-1, "Idle"},
    {0, "Printing"},
    {1, "Auto bed leveling"},
    {2, "Heatbed preheating"},
    {3, "Sweeping XY mech mode"},
    {4, "Changing filament"},
    {5, "M400 pause"},
    {6, "Paused: filament runout"},
    {7, "Heating hotend"},
    {8, "Calibrating extrusion"},
    {9, "Scanning bed surface"},
    {10, "Inspecting first layer"},
    {11, "Identifying build plate"},
    {12, "Calibrating Micro Lidar"},
    {13, "Homing toolhead"},
    {14, "Cleaning nozzle tip"},
    {15, "Checking extruder temperature"},
    {16, "Paused by user"},
    {17, "Paused: front cover falling"},
    {18, "Calibrating Micro Lidar"},
    {19, "Calibrating extrusion flow"},
    {20, "Paused: nozzle temperature malfunction"},
    {21, "Paused: heat bed temperature malfunction"},
    {22, "Filament unloading"},
    {23, "Paused: skipped step"},
    {24, "Filament loading"},
    {25, "Calibrating motor noise"},
    {26, "Paused: AMS lost"},
    {27, "Paused: low fan speed (heat break)"},
    {28, "Paused: chamber temperature control error"},
    {29, "Cooling chamber"},
    {30, "Paused by G-code"},
    {31, "Motor noise showoff"},
    {32, "Paused: nozzle filament covered detected"},
    {33, "Paused: cutter error"},
    {34, "Paused: first layer error"},
    {35, "Paused: nozzle clog"},
    {36, "Check absolute accuracy before calibration"},
    {37, "Absolute accuracy calibration"},
    {38, "Check absolute accuracy after calibration"},
    {39, "Calibrate nozzle offset"},
    {40, "Bed level (high temperature)"},
    {41, "Check quick release"},
    {42, "Check door and cover"},
    {43, "Laser calibration"},
    {44, "Check platform"},
    {45, "Check bird's-eye camera position"},
    {46, "Calibrate bird's-eye camera"},
    {47, "Bed level phase 1"},
    {48, "Bed level phase 2"},
    {49, "Heating chamber"},
    {50, "Heatbed cooling"},
    {51, "Print calibration lines"},
    {52, "Check material"},
    {53, "Calibrating live-view camera"},
    {54, "Waiting for heatbed temperature"},
    {55, "Check material position"},
    {56, "Calibrating cutter model offset"},
    {57, "Measuring surface"},
    {58, "Thermal preconditioning"},
    {59, "Homing blade holder"},
    {60, "Calibrating camera offset"},
    {61, "Calibrating blade holder position"},
    {62, "Hotend pick/place test"},
    {63, "Waiting for chamber temperature to equalize"},
    {64, "Preparing hotend"},
    {65, "Calibrating nozzle clumping detection"},
    {66, "Purifying chamber air"},
    {67, "Measuring rotary attachment"},
    {68, "Moving toolhead above purge chute"},
    {69, "Cooling nozzle"},
    {70, "Moving toolhead to center of heatbed"},
    {71, "Active arc fitting"},
    {72, "Hotend type detection"},
    {73, "Build plate alignment detection"},
    {74, "Heatbed surface foreign object detection"},
    {75, "Heatbed underside foreign object detection"},
    {76, "Pre-extrusion before printing"},
    {77, "Preparing AMS"},
    {255, "Idle"},
};

inline const char *stageName(int code)
{
    for (size_t i = 0; i < sizeof(STAGE_NAMES) / sizeof(STAGE_NAMES[0]); i++)
    {
        if (STAGE_NAMES[i].code == code)
            return STAGE_NAMES[i].name;
    }
    return "Unknown";
}

// Stages that mean "printer is paused" (LED logic + HA).
inline bool stageIsPause(int code)
{
    switch (code)
    {
    case 5: case 6: case 16: case 17: case 20: case 21: case 23: case 26: case 27:
    case 28: case 30: case 32: case 33: case 34: case 35:
        return true;
    default:
        return false;
    }
}

// Stages where the Micro Lidar / camera is measuring and users typically want the LEDs off/dim.
inline bool stageUsesLidar(int code)
{
    return code == 1 || code == 8 || code == 9 || code == 10 || code == 12 || code == 14 || code == 18;
}

static const char *const GCODE_STATES[] = {"IDLE", "PREPARE", "SLICING", "RUNNING", "PAUSE", "FINISH", "FAILED", "INIT", "OFFLINE"};

// HMS severity: (code >> 16) of the 32-bit `code` field. 0 = none.
inline const char *hmsSeverityName(uint8_t level)
{
    switch (level)
    {
    case 1: return "Fatal";
    case 2: return "Serious";
    case 3: return "Common";
    case 4: return "Info";
    default: return "None";
    }
}

// HMS module: top byte of the 32-bit `attr` field.
inline const char *hmsModuleName(uint8_t module)
{
    switch (module)
    {
    case 0x03: return "Motion Controller";
    case 0x05: return "Mainboard";
    case 0x07: return "AMS";
    case 0x08: return "Toolhead";
    case 0x0C: return "XCam";
    case 0x12: return "AMS";
    default: return "Unknown";
    }
}

// Format the 64-bit composite HMS code as HMS_AAAA_BBBB_CCCC_DDDD (buf >= 24 bytes).
inline void hmsFormatCode(uint64_t code, char *buf, size_t len)
{
    snprintf(buf, len, "HMS_%04X_%04X_%04X_%04X",
             (unsigned)((code >> 48) & 0xFFFF), (unsigned)((code >> 32) & 0xFFFF),
             (unsigned)((code >> 16) & 0xFFFF), (unsigned)(code & 0xFFFF));
}

// Best-effort printer model from the serial number prefix (community-observed; not authoritative).
inline const char *printerModelFromSerial(const char *serial)
{
    if (!serial || strlen(serial) < 3)
        return "";
    if (strncmp(serial, "00M", 3) == 0) return "X1C";
    if (strncmp(serial, "00W", 3) == 0) return "X1";
    if (strncmp(serial, "03W", 3) == 0) return "X1E";
    if (strncmp(serial, "01S", 3) == 0) return "P1P";
    if (strncmp(serial, "01P", 3) == 0) return "P1S";
    if (strncmp(serial, "030", 3) == 0) return "A1";
    if (strncmp(serial, "039", 3) == 0) return "A1 mini";
    if (strncmp(serial, "094", 3) == 0) return "H2D";
    return "Bambu";
}

// home_flag bits (ha-bambulab Home_Flag_Values); home_flag arrives as a signed 32-bit int.
#define HOME_FLAG_X_HOMED (1u << 0)
#define HOME_FLAG_Y_HOMED (1u << 1)
#define HOME_FLAG_Z_HOMED (1u << 2)
#define HOME_FLAG_CAMERA_RECORDING (1u << 5)
#define HOME_FLAG_SDCARD_PRESENT (1u << 8)
#define HOME_FLAG_SDCARD_ABNORMAL (1u << 9)
#define HOME_FLAG_AMS_AUTO_SWITCH (1u << 10)
#define HOME_FLAG_WIRED_NETWORK (1u << 18)
#define HOME_FLAG_FILAMENT_TANGLE (1u << 20)
#define HOME_FLAG_DOOR_OPEN (1u << 23)

#endif
