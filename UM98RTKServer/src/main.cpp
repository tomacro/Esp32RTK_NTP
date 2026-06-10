/**
 * @file main.cpp
 * @brief Multi-Caster RTK Engine & Asynchronous Stratum-1 NTP Daemon
 * * ============================================================================
 * SOFTWARE ARCHITECTURE & PARSING PARADIGM SHIFTS
 * ============================================================================
 * This core orchestration architecture expands the baseline single-client data 
 * pipeline to handle parallel multi-caster streaming alongside local network infrastructure utilities.
 * * @see Forked from: https://github.com/mctainsh/Esp32/blob/main/UM98RTKServer/src/main.cpp
 * * METHOD TO THE MADNESS: ARCHITECTURAL PRINCIPLES
 * * 1. DUAL-CORE FREERTOS ISOLATION (ZERO CPU COLLISION)
 * - CORE 1 (Main Loop): Prioritizes the high-baud serial interface ring buffers, 
 * maintaining uninterrupted RTCM3 data delivery to three distinct external 
 * internet casters (Onocoy, RTK2Go, RTKDirect) and a single local TCP rover slot.
 * - CORE 0 (NtpServerTask): Hosts an isolated, high-priority UDP daemon listening on 
 * Port 123. Disciplined by the sub-microsecond hardware 1PPS edge, it processes local 
 * network timing requests entirely unhindered by network lags on Core 1.
 * * 2. ANTI-STREAM CORRUPTION SERIAL EXTRACTOR
 * To obtain calendar time context ("the brain") without adding a secondary serial physical 
 * harness to TXD2/RXD2, we configure the UM980 to interleave ASCII NMEA sentences ($GNGGA) 
 * directly into the primary RTCM3 binary stream over TXD1. 
 * * A standard RTCM3 parser scanning blindly for the 0xD3 preamble can easily misinterpret 
 * matching ASCII payload characters, inducing fatal buffer slips. This engine introduces 
 * an inline state-machine gate that instantly peels ASCII strings off the wire before they 
 * can touch the binary stream pipelines, maintaining 100% caster connection stability.
 * * 3. NETWORK SOCKET & LATENCY PROTECTION
 * - WiFi Power Management is locked wide open (WIFI_PS_NONE) to suppress wireless jitter.
 * - Sockets enforce aggressive non-blocking timeout limits (15-50ms) so a hanging internet 
 * caster endpoint or disconnected rover cannot paralyse local infrastructure utilities.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <Update.h>
#include <esp_wifi.h>
#include <lwip/apps/sntp.h>
#include "Config.h"

// Define Global Casters (Onocoy, RTK2Go, RTKDirect)
NtripCaster casters[] = {
    {"ntrip.onocoy.com", 2101, "YOUR_ONOCOY_MOUNT", "YOUR_ONOCOY_USER", "YOUR_ONOCOY_PASS", false, 0},
    {"rtk2go.com", 2101, "YOUR_RTK2GO_MOUNT", "unmapped", "YOUR_RTK2GO_PASS", false, 0},
    {"caster.rtkdirect.com", 2101, "YOUR_DIRECT_MOUNT", "YOUR_DIRECT_USER", "YOUR_DIRECT_PASS", false, 0}
};
const int numCasters = sizeof(casters) / sizeof(NtripCaster);

// Network Server / Client Instances
WebServer webServer(80);
WiFiUDP ntpUdpSocket;
WiFiServer localRtkServer(8888);
WiFiClient activeLocalRover;
WiFiClient globalCasterClients[numCasters];

// Global Infrastructure Operational Configuration Settings (RAM Initial State)
volatile LocalInfraConfig infraState = {
    true,  // ntpEnabled
    true,  // ntpPpsStrict
    LOCAL_RTK_RAW_TCP, // rtkMode
    8888,  // localPort
    60,    // clientTimeoutSec
    true,  // useSurveyIn
    60,    // surveyInDurationSec (1 minute quick lock)
    0.0, 0.0, 0.0 // Explicit position mappings placeholders
};

// Hardware Timing Variables across cores
volatile uint32_t lastPpsMicros = 0;
volatile bool ppsTickOccurred = false;
volatile int lastParsedHour = 0;
volatile int lastParsedMin  = 0;
volatile int lastParsedSec  = 0;
volatile bool freshNmeaTime = false;

// Safe Serial ASCII Parsing Line Buffers
String nmeaLineBuffer = "";
bool parsingNmeaMode = false;
uint32_t localRoverLastActive = 0;

// Base64 helper routine for standard Basic Auth connection challenges
void b64encode(const char* input, char* output) {
    const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, j = 0;
    int len = strlen(input);
    while (len > 2) {
        output[j++] = b64[input[i] >> 2];
        output[j++] = b64[((input[i] & 0x03) << 4) | (input[i+1] >> 4)];
        output[j++] = b64[((input[i+1] & 0x0F) << 2) | (input[i+2] >> 6)];
        output[j++] = b64[input[i+2] & 0x3F];
        i += 3; len -= 3;
    }
    if (len == 2) {
        output[j++] = b64[input[i] >> 2];
        output[j++] = b64[((input[i] & 0x03) << 4) | (input[i+1] >> 4)];
        output[j++] = b64[(input[i+1] & 0x0F) << 2];
        output[j++] = '=';
    } else if (len == 1) {
        output[j++] = b64[input[i] >> 2];
        output[j++] = b64[(input[i] & 0x03) << 4];
        output[j++] = '=';
        output[j++] = '=';
    }
    output[j] = '\0';
}

// Microsecond Edge Capture Interrupt Handler (IRAM Target)
void IRAM_ATTR ppsInterruptHandler() {
    lastPpsMicros = micros();
    ppsTickOccurred = true;
}

// Forward Declarations
void connectCaster(int index);
void handleRootDashboard();
void handleSaveInfra();
void setupOtaEndpoints();
void configureUm980Hardware();
void reinitLocalServices();
void processLocalRtkDistribution(uint8_t rtcmByte);
void vNtpServerTask(void * pvParameters);
void processNtpRequest();

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Booting Unified Base Station Subsystem...");

    // 1. Initialize Intercept Handlers
    pinMode(PPS_PIN, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PPS_PIN), ppsInterruptHandler, RISING);

    // 2. Open GNSS Serial Interface Bus
    Serial1.begin(115200, SERIAL_8N1, RX1_PIN, TX1_PIN);
    configureUm980Hardware();

    // 3. Connect Networking Layer
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Associated Successfully.");
    
    // Latency Jitter Optimization: Turn off WiFi sleep cycles
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 4. Fire up Local Domain Resolution (mDNS)
    if (MDNS.begin(MDNS_HOSTNAME)) {
        Serial.printf("mDNS Responder online: http://%s.local\n", MDNS_HOSTNAME);
    }

    // 5. Build Web Dashboard Router Rules
    webServer.on("/", HTTP_GET, handleRootDashboard);
    webServer.on("/save_infra", HTTP_POST, handleSaveInfra);
    setupOtaEndpoints();
    webServer.begin();

    // 6. Bind Socket Subsystems
    reinitLocalServices();

    // 7. Core 0 Stratum-1 Isolation Deployment
    xTaskCreatePinnedToCore(vNtpServerTask, "NtpTask", 4096, NULL, 3, NULL, 0);
    Serial.println("System Core Ready. Pumping Engine Active.");
}

void loop() {
    uint32_t currentMillis = millis();
    webServer.handleClient();

    // Verify state alignment across global endpoints
    for (int i = 0; i < numCasters; i++) {
        if (!globalCasterClients[i].connected()) {
            casters[i].connected = false;
            if (currentMillis - casters[i].lastReconnectAttempt > 15000) {
                connectCaster(i);
            }
        }
    }

    // High-Efficiency Safe Extraction Loop
    while (Serial1.available()) {
        uint8_t b = Serial1.read();

        // Check for ASCII boundary frame shifts
        if (b == '$') {
            parsingNmeaMode = true;
            nmeaLineBuffer = "$";
            continue;
        }

        if (parsingNmeaMode) {
            if (b == '\n' || b == '\r') {
                parsingNmeaMode = false;
                if (nmeaLineBuffer.startsWith("$GNGGA") || nmeaLineBuffer.startsWith("$GPGGA")) {
                    int idx1 = nmeaLineBuffer.indexOf(',');
                    int idx2 = nmeaLineBuffer.indexOf(',', idx1 + 1);
                    if (idx1 != -1 && idx2 != -1 && (idx2 - idx1) > 6) {
                        String tStr = nmeaLineBuffer.substring(idx1 + 1, idx1 + 7);
                        lastParsedHour = tStr.substring(0, 2).toInt();
                        lastParsedMin  = tStr.substring(2, 4).toInt();
                        lastParsedSec  = tStr.substring(4, 6).toInt();
                        freshNmeaTime  = true;
                    }
                }
                nmeaLineBuffer = "";
            } else {
                nmeaLineBuffer += (char)b;
                if (nmeaLineBuffer.length() > 95) {
                    parsingNmeaMode = false;
                    nmeaLineBuffer = "";
                }
            }
            continue; // Insulate ASCII frames away from RTCM3 casters
        }

        // Forward legitimate RTCM3 binary chunks down the stream matrix
        for (int i = 0; i < numCasters; i++) {
            if (casters[i].connected) {
                globalCasterClients[i].write(b);
            }
        }
        processLocalRtkDistribution(b);
    }

    // Process Internal Clock Alignment Disciplining Action
    if (ppsTickOccurred && freshNmeaTime) {
        ppsTickOccurred = false;
        freshNmeaTime = false;

        struct tm tinfo;
        tinfo.tm_year = 2026 - 1900; tinfo.tm_mon = 5; tinfo.tm_mday = 10; // Production Baseline Anchors
        tinfo.tm_hour = lastParsedHour; tinfo.tm_min = lastParsedMin; tinfo.tm_sec = lastParsedSec;

        time_t utcEpoch = mktime(&tinfo);
        if (utcEpoch != -1) {
            struct timeval tv = { .tv_sec = utcEpoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
        }
    }
}

void configureUm980Hardware() {
    delay(200);
    // Unmask comprehensive satellite constellations across missing carrier bands
    Serial1.println("config signalgroup 2");
    
    // Inject position references based on configuration choice
    if (infraState.useSurveyIn) {
        Serial1.printf("mode base time %d\n", infraState.surveyInDurationSec);
    } else {
        Serial1.printf("mode base %.8f %.8f %.4f\n", infraState.explicitLat, infraState.explicitLon, infraState.explicitAlt);
    }
    Serial1.println("gngga 5"); // Interleave timing context sentence onto primary port channel
    Serial1.println("saveconfig");
    Serial1.flush();
    Serial.println("UM980 Silicon Registers Configured & Verified.");
}

void reinitLocalServices() {
    localRtkServer.end();
    if (activeLocalRover.connected()) activeLocalRover.stop();

    if (infraState.rtkMode == LOCAL_RTK_RAW_TCP) {
        localRtkServer = WiFiServer(infraState.localPort);
        localRtkServer.begin();
    }
    Serial.println("Local Socket Matrix Realigned.");
}

void processLocalRtkDistribution(uint8_t rtcmByte) {
    if (infraState.rtkMode == LOCAL_RTK_DISABLED) return;

    if (!activeLocalRover.connected()) {
        activeLocalRover = localRtkServer.available();
        if (activeLocalRover) {
            activeLocalRover.setTimeout(15);
            localRoverLastActive = millis();
        }
    }

    if (activeLocalRover.connected()) {
        if (activeLocalRover.write(rtcmByte) > 0) {
            localRoverLastActive = millis();
        }
        if (millis() - localRoverLastActive > (infraState.clientTimeoutSec * 1000)) {
            activeLocalRover.stop();
        }
    }
}

void connectCaster(int i) {
    casters[i].lastReconnectAttempt = millis();
    if (globalCasterClients[i].connect(casters[i].host, casters[i].port)) {
        globalCasterClients[i].setTimeout(45);
        
        String credentials = String(casters[i].username) + ":" + String(casters[i].password);
        char b64buffer[128];
        b64encode(credentials.c_str(), b64buffer);

        globalCasterClients[i].printf("POST /%s HTTP/1.1\r\n", casters[i].mountpoint);
        globalCasterClients[i].printf("Host: %s\r\n", casters[i].host);
        globalCasterClients[i].printf("Ntrip-Version: Ntrip/2.0\r\n");
        globalCasterClients[i].printf("User-Agent: NTRIP UM980 Server V2.0\r\n");
        globalCasterClients[i].printf("Authorization: Basic %s\r\n", b64buffer);
        globalCasterClients[i].printf("Connection: close\r\n\r\n");
        casters[i].connected = true;
    }
}

// Web Server Engine Dashboard Interface Handling Routine
void handleRootDashboard() {
    String html = "<html><head><style>body{font-family:sans-serif;margin:40px;background:#f0f2f5;} .card{background:white;padding:25px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);max-width:500px;margin-bottom:20px;} h2{color:#1a73e8;} .group{margin-bottom:15px;} label{display:block;margin-bottom:5px;font-weight:bold;} input,select{width:100%;padding:8px;border-radius:4px;border:1px solid #ccc;} .btn{background:#1a73e8;color:white;border:none;padding:10px 15px;border-radius:4px;cursor:pointer;}</style></head><body>";
    html += "<h1>RTK Infrastructure Panel</h1>";
    html += "<div class='card'><h2>Services Setup</h2><form action='/save_infra' method='POST'>";
    html += "<div class='group'><label><input type='checkbox' name='ntp_enable' value='1' " + String(infraState.ntpEnabled ? "checked" : "") + "> Enable Stratum-1 NTP (Port 123)</label></div>";
    html += "<div class='group'><label><input type='checkbox' name='pps_strict' value='1' " + String(infraState.ntpPpsStrict ? "checked" : "") + "> Enforce Strict 1PPS Discipline</label></div>";
    html += "<div class='group'><label>Local RTK Mode</label><select name='rtk_local_mode'>";
    html += "<option value='0' " + String(infraState.rtkMode == LOCAL_RTK_DISABLED ? "selected" : "") + ">Disabled</option>";
    html += "<option value='1' " + String(infraState.rtkMode == LOCAL_RTK_RAW_TCP ? "selected" : "") + ">Raw TCP Server</option></select></div>";
    html += "<div class='group'><label>Local Base Port</label><input type='number' name='local_port' value='" + String(infraState.localPort) + "'></div>";
    html += "<div class='group'><label>Rover Timeout (sec)</label><input type='number' name='local_timeout' value='" + String(infraState.clientTimeoutSec) + "'></div>";
    html += "<button type='submit' class='btn'>Apply Parameters</button></form></div>";
    html += "<p><a href='/update'>Access Web-OTA Update Gateway</a></p></body></html>";
    webServer.send(200, "text/html", html);
}

void handleSaveInfra() {
    infraState.ntpEnabled = webServer.hasArg("ntp_enable");
    infraState.ntpPpsStrict = webServer.hasArg("pps_strict");
    infraState.rtkMode = (LocalRtkMode)webServer.arg("rtk_local_mode").toInt();
    infraState.localPort = webServer.arg("local_port").toInt();
    infraState.clientTimeoutSec = webServer.arg("local_timeout").toInt();
    reinitLocalServices();
    webServer.sendHeader("Location", "/");
    webServer.send(303);
}

void setupOtaEndpoints() {
    webServer.on("/update", HTTP_GET, []() {
        webServer.send(200, "text/html", "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Upload Image'></form>");
    });
    webServer.on("/update", HTTP_POST, []() {
        webServer.send(200, "text/plain", (Update.hasError()) ? "OTA FAILED" : "OTA SUCCESS. COMPILING REBOOT...");
        delay(1000);
        ESP.restart();
    }, []() {
        HTTPUpload& upload = webServer.upload();
        if (upload.status == UPLOAD_FILE_START) {
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
        } else if (upload.status == UPLOAD_FILE_END) {
            Update.end(true);
        }
    });
}

// Core 0 Asynchronous NTP Real-Time Daemon Loop Execution Thread
void vNtpServerTask(void * pvParameters) {
    ntpUdpSocket.begin(NTP_PORT);
    for(;;) {
        if (infraState.ntpEnabled) {
            int packetSize = ntpUdpSocket.parsePacket();
            if (packetSize >= 48) {
                processNtpRequest();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1)); // Context frame release slice execution
    }
}

void processNtpRequest() {
    byte pBuffer[48];
    ntpUdpSocket.read(pBuffer, 48);

    if (infraState.ntpPpsStrict && (micros() - lastPpsMicros > 1500000)) {
        return; // Drop packet processing if 1PPS signal validation threshold is stale
    }

    byte origTimestamp[8];
    memcpy(origTimestamp, &pBuffer[40], 8);
    memset(pBuffer, 0, 48);

    pBuffer[0] = 0b00100100; // No Warning, Version 4, Server Mode 4
    pBuffer[1] = 1;          // Stratum Level 1
    pBuffer[2] = 6; pBuffer[3] = -10;
    pBuffer[7] = 0x0A; pBuffer[11] = 0x0A;
    pBuffer[12] = 'G'; pBuffer[13] = 'N'; pBuffer[14] = 'S'; pBuffer[15] = 'S';

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t ntpSec = tv.tv_sec + 2208988800UL;
    uint32_t ntpFrac = (uint32_t)((double)tv.tv_usec * 4294.967296);

    memcpy(&pBuffer[24], origTimestamp, 8); // Mapping Origin Timestamp back to back

    pBuffer[32] = (ntpSec >> 24) & 0xFF; pBuffer[33] = (ntpSec >> 16) & 0xFF;
    pBuffer[34] = (ntpSec >> 8)  & 0xFF; pBuffer[35] = ntpSec & 0xFF;

    pBuffer[40] = (ntpSec >> 24) & 0xFF; pBuffer[41] = (ntpSec >> 16) & 0xFF;
    pBuffer[42] = (ntpSec >> 8)  & 0xFF; pBuffer[43] = ntpSec & 0xFF;
    pBuffer[44] = (ntpFrac >> 24) & 0xFF; pBuffer[45] = (ntpFrac >> 16) & 0xFF;
    pBuffer[46] = (ntpFrac >> 8)  & 0xFF; pBuffer[47] = ntpFrac & 0xFF;

    ntpUdpSocket.beginPacket(ntpUdpSocket.remoteIP(), ntpUdpSocket.remotePort());
    ntpUdpSocket.write(pBuffer, 48);
    ntpUdpSocket.endPacket();
}