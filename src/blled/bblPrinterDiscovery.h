#ifndef _BBLPRINTERDISCOVERY_H
#define _BBLPRINTERDISCOVERY_H

// ---------------------------------------------------------------------------
// bblPrinterDiscovery.h -- Bambu SSDP discovery as a non-blocking state machine
//
// Upstream blocked loop() for ~5.5 s every 10 s (REVIEW #3).  This version
// sends the two M-SEARCH datagrams and then polls the socket from the main loop
// for LISTEN_MS without ever delaying, and only runs when it is useful:
//   * once at boot,
//   * every DISCOVERY_INTERVAL_MS while the printer MQTT is down or no IP is
//     configured,
//   * on demand (discoveryRequest(), e.g. POST /api/action {"action":"discover"}).
//
// The cache (IP + USN(serial) + model) feeds GET /api/printers.  When the USN
// matches our configured serial and printerAutoIp is on, the printer IP is
// updated and configDirty is raised (the main loop saves).
//
// Threading: main loop only.  discoveryRequest() may be called from the async
// task; it just sets a flag.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

#include "types.h"
#include "stages.h"
#include "logSerial.h"
#include "filesystem.h"

#define BBL_SSDP_PORT 2021
#define BBL_SSDP_MCAST_IP IPAddress(239, 255, 255, 250)
#define BBL_MAX_PRINTERS 10
#define BBL_DISCOVERY_INTERVAL_MS 300000UL // 5 min, only while disconnected
#define BBL_SEND_GAP_MS 250UL
#define BBL_LISTEN_MS 5000UL

struct BBLPrinter
{
    IPAddress ip;
    char usn[40];
    char model[12];
};

static WiFiUDP bblUdp;
static bool bblUdpInitialized = false;
static BBLPrinter bblLastKnownPrinters[BBL_MAX_PRINTERS];
static int bblKnownPrinterCount = 0;

enum class DiscoveryState : uint8_t
{
    Idle = 0,
    Send1,
    Send2,
    Listen
};

static DiscoveryState bblState = DiscoveryState::Idle;
static unsigned long bblStateMs = 0;
static unsigned long bblLastRunMs = 0;
static bool bblRequested = true; // run once at boot
static int bblSessionFound = 0;

static const char *BBL_MSEARCH =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:2021\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 5\r\n"
    "ST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";

// Ask for a discovery round; picked up by the next discoveryLoop().
void discoveryRequest()
{
    bblRequested = true;
}

static int bblFindPrinter(IPAddress ip)
{
    for (int i = 0; i < bblKnownPrinterCount; i++)
        if (bblLastKnownPrinters[i].ip == ip)
            return i;
    return -1;
}

// Discovery cache -> [{"ip":..,"usn":..,"model":..}] (used by GET /api/printers).
void discoveryListJson(JsonDocument &doc)
{
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < bblKnownPrinterCount; i++)
    {
        JsonObject obj = arr.add<JsonObject>();
        obj["ip"] = bblLastKnownPrinters[i].ip.toString();
        obj["usn"] = bblLastKnownPrinters[i].usn;
        obj["model"] = bblLastKnownPrinters[i].model;
    }
}

int discoveryCount() { return bblKnownPrinterCount; }
bool discoveryIsRunning() { return bblState != DiscoveryState::Idle; }

static void bblSendSearch()
{
    bblUdp.beginPacket(BBL_SSDP_MCAST_IP, BBL_SSDP_PORT);
    bblUdp.print(BBL_MSEARCH);
    bblUdp.endPacket();
}

// Read and record one pending response, if any.
static void bblReadResponses()
{
    int size = bblUdp.parsePacket();
    while (size > 0)
    {
        IPAddress senderIP = bblUdp.remoteIP();
        char buffer[512];
        int len = bblUdp.read(buffer, sizeof(buffer) - 1);
        if (len < 0)
            len = 0;
        buffer[len] = '\0';

        // USN: <serial>
        char usn[40] = "";
        const char *usnPos = strstr(buffer, "USN:");
        if (usnPos != NULL)
        {
            usnPos += 4;
            while (*usnPos == ' ')
                usnPos++;
            size_t n = 0;
            while (usnPos[n] != '\0' && usnPos[n] != '\r' && usnPos[n] != '\n' && n + 1 < sizeof(usn))
            {
                usn[n] = usnPos[n];
                n++;
            }
            usn[n] = '\0';
        }

        int idx = bblFindPrinter(senderIP);
        bool isNew = (idx < 0);
        if (isNew && bblKnownPrinterCount < BBL_MAX_PRINTERS)
        {
            idx = bblKnownPrinterCount++;
            bblLastKnownPrinters[idx].ip = senderIP;
        }
        if (idx >= 0)
        {
            strlcpy(bblLastKnownPrinters[idx].usn, usn, sizeof(bblLastKnownPrinters[idx].usn));
            strlcpy(bblLastKnownPrinters[idx].model, printerModelFromSerial(usn),
                    sizeof(bblLastKnownPrinters[idx].model));
        }
        bblSessionFound++;

        if (printerConfig.debugVerbose || (printerConfig.debugChanges && isNew))
            LogSerial.printf("[BBLScan] %s  [USN: %s] [%s]\n", senderIP.toString().c_str(), usn,
                             printerModelFromSerial(usn));

        // Our printer moved to a new IP?
        if (printerConfig.printerAutoIp && usn[0] != '\0' && strlen(printerConfig.serialNumber) > 0 &&
            strcmp(printerConfig.serialNumber, usn) == 0)
        {
            String currentIP = senderIP.toString();
            if (strcmp(printerConfig.printerIP, currentIP.c_str()) != 0)
            {
                LogSerial.printf("[BBLScan] Printer %s moved: %s -> %s (saving)\n", usn,
                                 printerConfig.printerIP, currentIP.c_str());
                strlcpy(printerConfig.printerIP, currentIP.c_str(), sizeof(printerConfig.printerIP));
                configDirty = true;
                mqttEnqueue(MqttCmd::Reconnect, 0);
            }
        }

        size = bblUdp.parsePacket();
    }
}

// Non-blocking state machine; call every main-loop iteration.
void discoveryLoop()
{
    if (WiFi.status() != WL_CONNECTED || WiFi.getMode() == WIFI_AP)
        return;

    unsigned long now = millis();

    if (bblState == DiscoveryState::Idle)
    {
        bool printerUnreachable = (!printerState.online || strlen(printerConfig.printerIP) == 0);
        bool periodicDue = printerUnreachable && (now - bblLastRunMs) > BBL_DISCOVERY_INTERVAL_MS;
        if (!bblRequested && !periodicDue)
            return;

        bblRequested = false;
        bblLastRunMs = now;
        bblSessionFound = 0;

        if (!bblUdpInitialized)
        {
            bblUdp.beginMulticast(BBL_SSDP_MCAST_IP, BBL_SSDP_PORT);
            bblUdpInitialized = true;
        }
        if (printerConfig.debugChanges || printerConfig.debugVerbose)
            LogSerial.println(F("[BBLScan] Searching for printers..."));

        bblSendSearch();
        bblState = DiscoveryState::Send2;
        bblStateMs = now;
        return;
    }

    if (bblState == DiscoveryState::Send2)
    {
        bblReadResponses();
        if ((now - bblStateMs) < BBL_SEND_GAP_MS)
            return;
        bblSendSearch();
        bblState = DiscoveryState::Listen;
        bblStateMs = now;
        return;
    }

    if (bblState == DiscoveryState::Listen)
    {
        bblReadResponses();
        if ((now - bblStateMs) < BBL_LISTEN_MS)
            return;
        bblState = DiscoveryState::Idle;
        if (printerConfig.debugVerbose)
            LogSerial.printf("[BBLScan] Finished, %d response(s), %d printer(s) known\n",
                             bblSessionFound, bblKnownPrinterCount);
    }
}

#endif
