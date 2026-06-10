/**
 * @file Config.h
 * @brief Unified Infrastructure Configuration Registry
 * * ============================================================================
 * THE METHOD TO THE MADNESS: HARDWARE & STRUCTURAL OVERHAUL
 * ============================================================================
 * This file handles system parameters and runtime states for a high-performance 
 * dual-purpose RTK Base Station and local Stratum-1 NTP Server.
 * * Derived from the baseline 4-wire configuration established by John McTainsh:
 * @see Original Repository: https://github.com/mctainsh/Esp32/blob/main/UM98RTKServer/README.md
 * * HARDWARE LAYOUT MODIFICATIONS (UM980 Breakout to LilyGo T-Display-S3):
 * 1. Power Distribution: A 5V always-on cooling fan and the UM980 VCCIN share 
 * the S3's single 5V VBUS rail via a 1-to-2 Dupont splitter cable harness.
 * 2. 1PPS Line Isolation: The hardware timing pulse is explicitly routed to 
 * GPIO 17 instead of the original repo's GPIO 21. This eliminates critical 
 * electrical conflicts with the physical right-hand button on the face 
 * of the LilyGo T-Display-S3.
 * * 5-WIRE TOPOLOGY MATRIX:
 * [T-Display-S3]              [UM980 Breakout]        [Connector Source]
 * 5V (VBUS)   <--Split-->     VCCIN & 5V Fan          JST Connector 1
 * G (GND)     <--Split-->     Shared Ground           JST Connector 1 & 2
 * GPIO 13 (TX) ------------->  RXD1                    JST Connector 1
 * GPIO 12 (RX) <-------------  TXD1                    JST Connector 1
 * GPIO 17 (INT)<-------------  1PPS                    JST Connector 2
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// WiFi Configuration
const char* const WIFI_SSID     = "YOUR_WIFI_SSID";
const char* const WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* const MDNS_HOSTNAME = "rtkbase";

// Fixed Hardware Pin Mapping Topology
#define PPS_PIN   17  // Securely bypasses GPIO 21 physical button conflict
#define RX1_PIN   12  // Connect to UM980 TXD1 (Connector 1)
#define TX1_PIN   13  // Connect to UM980 RXD1 (Connector 1)
#define NTP_PORT  123

// Local Distribution Enum Modes
enum LocalRtkMode {
    LOCAL_RTK_DISABLED = 0,
    LOCAL_RTK_RAW_TCP  = 1
};

// Global Architecture Infrastructure Control Block Struct
struct LocalInfraConfig {
    bool ntpEnabled;
    bool ntpPpsStrict;
    LocalRtkMode rtkMode;
    int localPort;
    int clientTimeoutSec;
    bool useSurveyIn;
    int surveyInDurationSec;
    double explicitLat;
    double explicitLon;
    double explicitAlt;
};

// NTRIP Caster Target Definition Layout
struct NtripCaster {
    const char* host;
    int port;
    const char* mountpoint;
    const char* username;
    const char* password;
    bool connected;
    uint32_t lastReconnectAttempt;
};

// Expose shared runtime parameters to execution loops
extern volatile LocalInfraConfig infraState;
extern NtripCaster casters[];
extern const int numCasters;

#endif