# Firmware for M5StickC which allows bluetooth connection to xDrip for iOS
##### M5StickC xDrip monitor<br/>Copyright (C) Sergio Tomé Gómez

This is a modified version of https://github.com/JohanDegraeve/M5_StickC_xdrip_iOS, which is a modified version of https://github.com/mlukasek/M5StickC_NightscoutMon 

### What is available in this project

There's only one screen, with the bloodglucose value, an arrow, the delta and the time since last reading. Button A allows for screen rotation and button B updates the screen brightness
A bluetooth connection can be made with an iOS device that has xdrip installed : https://github.com/JohanDegraeve/xdripswift. 
The iOS device can send readings directly to the M5Stick

With xdrip one can
 * Scan for and connect to multiple M5Stacks and/or M5Sticks
 * Send readings : the M5Stickc receives readings from xdrip for iOS

Notes: 
  * This project currently support M5StickC-Plus since I don't have a M5StickC to check display in their smaller screen.
  * The project is also set up to use mg/dl. To use a different measure some code would need to be updates, although that doesn't sound too complicated.
  * The delta and time since last reading is not 100% accurate. Ideally those values would come from xDrip4iOS so that they match.
  * BG treasholds are hardcoded to the values I personally use. Those are constant values that can be updated, but ideally those would also come from xDrip4iOS.
  * This project doesn't support Wifi connections, only BLE. If Wifi is required to connect to nightscout it is recommented to look at the original version from mlukasek linked above

### To compile

To compile, in Arduino IDE set Tools - Partition Scheme - No OTA (Large APP). This is because the BLE library needs a lot of memory