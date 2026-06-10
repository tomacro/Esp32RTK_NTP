# STILL IN DEVELOPMENT - NOT GUARANTEED TO WORK

# ESP32 UM980 Multi-Caster RTK & Stratum-1 NTP Server

This repository is an optimized, high-performance fork of the baseline [mctainsh/UM98RTKServer](https://github.com/mctainsh/Esp32/blob/main/UM98RTKServer/README.md). 
<b>YOU MUST REFERENCE THE INITIAL REPO FOR MORE INFORMATION</b>

While the original repository provides an excellent baseline for streaming raw RTCM3 data from a Unicore GNSS module to a single caster, this version introduces concurrent multi-caster casting alongside an isolated local Stratum-1 Network Time Protocol (NTP) server disciplined by a hardware 1PPS pulse.

This was a fairly straightforward and fun project to (1) add a NTP server to my tech stack primarily using hardware already on hand, and (2) rekindle my C++ coding knowledge.

## Key Enhancements

- **Stratum-1 NTP Server:** Leverages the ultra-precise hardware Pulse Per Second (1PPS) line from a UM980 board to serve local network time over UDP Port 123.
- **Asynchronous Core Allocation:** The NTP server daemon runs as an isolated FreeRTOS task pinned exclusively to **Core 0**, ensuring high network throughput and timing precision cannot be degraded by RTK serial processing on Core 1.
- **Anti-Stream Corruption Parser:** Implements an inline ASCII text state machine on the serial ring buffer. This filters out incoming NMEA string configurations (`$GNGGA`) to feed the NTP logic while protecting the raw binary RTCM3 stream from data collisions or falsing.
- **Low-Latency Wireless Tuning:** Forces the ESP32-S3 Wi-Fi radio power management subsystem into an active-locked state (`WIFI_PS_NONE`), minimizing wireless packet jitter and transmission delays.

## Hardware Architecture & Pin Mapping

To support the simultaneous streaming of binary correction frames and dedicated hardware time synchronization, the wiring layout shifts from the original 4-wire specification to a 5-wire topology using a shared 5V power rail split via Dupont splitter cables. Unlike the original setup, a UM980 board was used, with JST-to-dupont connecting to the GPIO pins on the T-Display-S3.

| LilyGo T-Display-S3 Pin | Hardware Function | UM980 Breakout Pin | Connector Source |
| :--- | :--- | :--- | :--- |
| **5V / VBUS** | Main System Power (Split) | **VCCIN** | JST Connector 1 |
| **G (GND)** | Shared System Ground | **GND** | JST Connector 1 & 2 |
| **13** | ESP32 TX (Commands) | **RXD1** | JST Connector 1 |
| **12** | ESP32 RX (RTCM3 Data) | **TXD1** | JST Connector 1 |
| **17** | 1PPS Hardware Interrupt | **1PPS** | JST Connector 2 |

*Note: **GPIO 17** is intentionally selected for the 1PPS line instead of GPIO 21 to completely eliminate electrical conflicts with the physical right-hand button built into the face of the LilyGo T-Display-S3.*

## Deployment & Configuration

1. **Hardware Verification:** Assemble the components using a high-quality USB-C power source (minimum 1.5A) to handle the concurrent power draw of the ESP32-S3 radio amplifier, the UM980 processing core, and an external 5V cooling fan.
2. **Credentials Configuration:** Open `include/Config.h` and update your Wi-Fi credentials along with your individual mountpoint parameters for **Onocoy**, **RTK2Go**, and **RTKDirect**.
3. **Compilation:** Compile and flash the project via PlatformIO.

## Verifying the Local Stratum-1 Status

Once the station achieves a stable multi-constellation RTK fix, the UM980 will begin firing its hardware timing pulse. You can verify that your local network clients are receiving true Stratum-1 atomic time by executing a standard NTP query from a terminal on your computer:

```bash
ntpdate -vd 192.168.1.X  # Replace with your ESP32's local IP address

***A successful response will output a valid RFC 5905 header payload identifying the reference clock source string as GPS or GNSS at Stratum Level 1.