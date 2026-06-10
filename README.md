# An RTK receiver and NTP source using ESP 32

## Forked from [UM98x RTK Server](https://github.com/mctainsh/Esp32/tree/main/UM98RTKServer)

Connect a UM980 RTK receiver to a TTGO T-Display-S3 to create a base station server. Server can send RTK connections to two different RTK Casters. (Currently Onocoy and RTK2GO)
This fork adds NTP functionality with the addition of a connection between PPS pin on the UM980 board and the T-Display-S3, along with custom code to enable NTP functionality. Allows for up to 3 NTRIPs to provide RTK data.
