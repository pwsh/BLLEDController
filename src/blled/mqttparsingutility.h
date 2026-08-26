#ifndef _MQTTPARSERUTILITY
#define _MQTTPARSERUTILITY

// ---------------------------------------------------------------------------
// mqttparsingutility.h -- small helpers around PubSubClient state codes.
//
// The HMS severity/module/name helpers moved to stages.h (hmsSeverityName(),
// hmsModuleName(), hmsFormatCode()) so the API and HA layers can reuse them.
//
// Threading: mqttStateText() is pure; ParseMQTTState() logs and must therefore
// only be called for infrequent events (connect/disconnect), never per message.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include "types.h"
#include "logSerial.h"

// PubSubClient::state() -> human readable text (also used by /api/status).
const char *mqttStateText(int code)
{
    switch (code)
    {
    case -4: return "Connection timeout";
    case -3: return "Connection lost";
    case -2: return "Connect failed";
    case -1: return "Disconnected";
    case 0: return "Connected";
    case 1: return "Bad protocol";
    case 2: return "Bad client id";
    case 3: return "Unavailable";
    case 4: return "Bad credentials";
    case 5: return "Unauthorized";
    default: return "Unknown";
    }
}

// HMS severity is the top 16 bits of the 32-bit `code` field.
inline uint8_t ParseHMSSeverity(uint32_t code)
{
    uint32_t level = code >> 16;
    return (level >= 1 && level <= 4) ? (uint8_t)level : (uint8_t)0;
}

void ParseMQTTState(int code)
{
    LogSerial.printf("[MQTT] %s (%d)\n", mqttStateText(code), code);
}

#endif
