// needed to act as BLE server
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
// M5StickC-Plus Library
#include <M5StickCPlus.h>
// Free Fonts
#include "Free_Fonts.h"
// Other librtaries
#include "time.h"
#include <cstring>
#include <string>

// Data Structures to store all information used by the app //
struct readings {
  int glycemia;
  unsigned long time;
};
struct thresholds { // thresholds are defined as integers. If ever these values are sent from BLE, we may need to update if the value is double/float for those using mmol/l
  const int high = 180;
  const int low = 70;
  const int highUrgent = 220;
  const int lowUrgent = 50;  
};
struct xDripTimes {
  unsigned long localTimeToUTC = 3600;  // time difference in seconds, between local time and utc time - to get UTC time, do localTimeFromBLE - localTimeToUTC
  unsigned long localTimeFromBLE = 0;   // local time, in seconds since 1.1.1970 retrieved from client - this value remains fixed once retrieved. Value 0 means not yet retrieved.
  unsigned long millsSinceBLE = 0;      // time in milliseconds since start of the sketch, when localTimeFromBLE was received
  unsigned int sensorDifMin = 0;
  time_t sensTime = 0;
};
struct xDripInfo {
  bool show_mgdl = true; // true = display mg/DL, false = display mmol/L
  int glycemia = 0;
  float glycemiaMmolL = 0;
  readings bgReadings[4] = {{0,0},{0,0},{0,0},{0,0}};
  char sensDir[32];
  int arrowAngle = 180;
  int prevArrowAngle = 180;
  uint8_t rotation = 3; // 1 = horizontal, normal; 2 = 90 clockwise, 3 = upside down, 4 = 270 clockwise or 90 anti-clockwise
  uint8_t lcdBrightness = 7; // 7 - 12
  thresholds limits;
  xDripTimes timeInfo;
} xDrip;

unsigned long msCount = 0;

//////// BLE PROPERTIES  ///////
// used for creating random password
char blepassword[64];
char const *letters = "abcdefghijklmnopqrstuvwxyz0123456789";

BLEServer *pServer = NULL;
BLECharacteristic * pRxTxCharacteristic;
BLEService *pService;
const String BLE_DeviceName = "M5StickC-Plus";
const String BLE_SERVICE_UUID = "AF6E5F78-706A-43FB-B1F4-C27D7D5C762F";
const String BLE_CHARACTERISTIC_UUID = "6D810E9F-0983-4030-BDA7-C7C9A6A19C1C";
const int maxBytesInOneBLEPacket = 20; // maximum number of bytes to send in one BLE packet
bool bleDeviceConnected = false; // Whether a client device is connected via BLE or not
bool bleAuthenticated = false; // is authentication done or not, in case not authenticated, we won't accept any reading or anything else
bool initialBLEStartUpOnGoing = true; // will be set to false as soon as BLE setup is fully finished
byte rxValueAsByteArray[maxBytesInOneBLEPacket]; // will hold value received from BLE client, defined here once to avoid heap fragmentation

// the setup routine runs once when M5Stack starts up
void setup() {
    // initialize the M5Stack object
    M5.begin();
    // set rotation, and text color
    M5.Lcd.setRotation(xDrip.rotation);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Axp.ScreenBreath(xDrip.lcdBrightness);
    // prevent button A "ghost" random presses
    Wire.begin();
    // Lcd display
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextSize(1);
    
    yield();

    M5.Lcd.println(F("Bluetooth is on, open the xdrip app on iOS device and scan for M5Stack"));

    for (int i=xDrip.lcdBrightness; i<=15;i++) {
      xDrip.lcdBrightness = i;
      M5.Axp.ScreenBreath(xDrip.lcdBrightness);  //Set screen brightness every 100ms
      delay(100);
    }

    setupBLE();

    pinMode(M5_LED, OUTPUT);
    digitalWrite(M5_LED, HIGH); // Turn off the LED. For some reason it turns on automatically when I attach the PIN....
};
void loop() {

  if(millis()-msCount>10000) { // Turn the led on and refresh screen data *if needed* every 10 seconds
    if (bleDeviceConnected && (xDrip.glycemia > xDrip.limits.highUrgent || xDrip.glycemia < xDrip.limits.lowUrgent)) {
      // Turn on the red LED and off again in .2 second if glucose is more than 220 or less than 70 or if no reading received yet.
      digitalWrite(M5_LED, LOW);
      delay(200);
      digitalWrite(M5_LED, HIGH);
    }

    if (!initialBLEStartUpOnGoing) {
      updateGlycemia(false, false);
    }

    msCount = millis();
  }

  M5.update();

  if (M5.BtnA.wasPressed()) {
    updateRotation();
    msCount = millis();
  }

  if (M5.BtnB.wasPressed()) {
    updateBrightness();
  }

  delay(100);
};

//// FUNCTIONS ////
void updateGlycemia(bool forceRedaw, bool newReadings) {
  // Update the display with the received values from xDrip
  M5.Lcd.setCursor(0, 0);
  uint16_t text_color = TFT_WHITE;
  unsigned long utcTimeInSeconds = getUTCTimeInSeconds();
  bool showError = false;

  // When xDrip4iOS disconnects from the sensor and then connets back, I noticed it sends the same reading multiple times, 
  // so we will just compare the new reading with the previous one and if the values are the same (BG and sensor time) then ignore that new reading 
  if (newReadings && xDrip.glycemia == xDrip.bgReadings[0].glycemia && xDrip.timeInfo.sensTime == xDrip.bgReadings[0].time) {
    newReadings = false;
  }

  // Calculate number of minutes since last bg reading
  if (utcTimeInSeconds >  0L) {
    xDrip.timeInfo.sensorDifMin = (utcTimeInSeconds - xDrip.timeInfo.sensTime) / 60;

    if (xDrip.timeInfo.sensorDifMin > 59) {
      showError = true; // latest nightscout reading is more than 1 hour old, don't show the value - value is "---"
    }
  }

  //********************************************************//
  // if values are new, then display them on screen         //
  //********************************************************//
  if (xDrip.glycemia != xDrip.bgReadings[0].glycemia || xDrip.arrowAngle != xDrip.prevArrowAngle || forceRedaw || showError) {
    // Set the screen to black to print all again
    M5.Lcd.fillRect(0, 0,  M5.Lcd.width(),  M5.Lcd.height(), TFT_BLACK);
    // Determine size and position of text based on device rotation (with when horizontal is 135)
    int xPosition = 0;
    if (M5.Lcd.width() <= 135) {
      M5.Lcd.setTextSize(3);
    } else {
      M5.Lcd.setTextSize(4);
      if (xDrip.glycemia > 99) {
        xPosition = 30;
      } else {
        xPosition = 5;
      }
    }
    // Determine text color based on glycemia values
    if (showError) {
      text_color = TFT_WHITE;
    } else if (xDrip.glycemia >= xDrip.limits.highUrgent || xDrip.glycemia <= xDrip.limits.lowUrgent) {
      text_color = TFT_RED;
    } else if (xDrip.glycemia >= xDrip.limits.high || xDrip.glycemia <= xDrip.limits.low) {
      text_color = TFT_YELLOW;
    } else {
      text_color = TFT_GREEN;
    }
    // Print Glycemia
    M5.Lcd.setTextColor(text_color, TFT_BLACK);
    M5.Lcd.setFreeFont(FSSB12); //FSSB9 //FSSB18 //FSSB24
    M5.Lcd.setTextDatum(MC_DATUM);
    if (showError) {
      M5.Lcd.drawString("---", M5.Lcd.width()/2 - xPosition, M5.Lcd.height()/2 + 10, GFXFF);
    } else {
      if (xDrip.show_mgdl) {
        M5.Lcd.drawNumber(long(xDrip.glycemia), M5.Lcd.width()/2 - xPosition, M5.Lcd.height()/2 + 10);
      } else {
        char glycemiaMmolLStr[30];
        if(xDrip.glycemiaMmolL<10) {
          sprintf(glycemiaMmolLStr, "%3.1f", xDrip.glycemiaMmolL);
        } else {
          M5.Lcd.setFreeFont(FSSB9);
          sprintf(glycemiaMmolLStr, "%4.1f", xDrip.glycemiaMmolL);
        }
        M5.Lcd.drawString(glycemiaMmolLStr, M5.Lcd.width()/2 - xPosition, M5.Lcd.height()/2 + 10, GFXFF);
      }
      // Draw Arrow
      if (xDrip.arrowAngle != 180) {
        drawArrow(M5.Lcd.width(), M5.Lcd.height(), xDrip.arrowAngle + 85, text_color, M5.Lcd.width() <= 135);
      }
    }
  }

  //********************************************************//
  // Print minutes ago from last reading                    //
  //********************************************************//
  if (utcTimeInSeconds >  0L) {
    printMinsAgo();
  }

  //********************************************************//
  // Print delta if there are three consecutive readings    //
  //********************************************************//
  if (xDrip.timeInfo.sensorDifMin > 5) {
    hideDelta();
  } else if (hasPreviousReadings() && newReadings) {
    printDelta();
  }

  //*******************************.*************************//
  // Reset previous values with current values              //
  //********************************************************//  
  if (newReadings) {
    updateReadings();
  }
};

//// HELPER FUNCTIONS ////
void updateRotation() {
  if (xDrip.rotation == 4) {
    xDrip.rotation = 1;
  } else {
    xDrip.rotation++;
  }
  // set new rotation
  M5.Lcd.setRotation(xDrip.rotation);
  // Force a redisplay of the screen when calling updateGlycemia
  updateGlycemia(true, false);
};
void updateBrightness() {
  if (xDrip.lcdBrightness == 12) {
    xDrip.lcdBrightness = 7;
  } else {
    xDrip.lcdBrightness++;
  }
  M5.Axp.ScreenBreath(xDrip.lcdBrightness);
};
void setArrowAngle() {
  xDrip.arrowAngle = 180;
  if (strcmp(xDrip.sensDir,"DoubleDown")==0) {
    xDrip.arrowAngle = 91;
  } else if (strcmp(xDrip.sensDir,"SingleDown")==0) {
    xDrip.arrowAngle = 90;
  } else if (strcmp(xDrip.sensDir,"FortyFiveDown")==0) {
    xDrip.arrowAngle = 45;
  } else if (strcmp(xDrip.sensDir,"Flat")==0) {
    xDrip.arrowAngle = 0;
  } else if (strcmp(xDrip.sensDir,"FortyFiveUp")==0) {
    xDrip.arrowAngle = -45;
  } else if (strcmp(xDrip.sensDir,"SingleUp")==0) {
    xDrip.arrowAngle = -90;
  } else if (strcmp(xDrip.sensDir,"DoubleUp")==0) {
    xDrip.arrowAngle = -91;
  }
};
void drawArrow(int width, int height, int angle, uint16_t color, bool isSmall) {
  if (xDrip.arrowAngle == 91) { // Double Down
    if (isSmall) {
      drawArrow(width / 2 + 2, height / 2 + 60, angle - 1, color, 2);
    } else {
      drawArrow(width - 40, height / 2 - 4, angle - 1, color, 2);
    }
  } else if (xDrip.arrowAngle == 90) { // Down
    if (isSmall) {
      drawArrow(width / 2 + 2, height / 2 + 60, angle, color, 1);
    } else {
      drawArrow(width - 40, height / 2 - 4, angle, color, 1);
    }
  } else if (xDrip.arrowAngle == 45) { // 45 Down
    if (isSmall) {
      drawArrow(width / 2 - 12, height / 2 + 60, angle, color, 1);
    } else {
      drawArrow(width - 50, height / 2 + 2, angle, color, 1);
    }
  } else if (xDrip.arrowAngle == 0) { // Flat
    if (isSmall) {
      drawArrow(width / 2 - 15, height / 2 + 80, angle, color, 1);
    } else {
      drawArrow(width - 56, height / 2 + 10, angle, color, 1);
    }
  } else if(xDrip.arrowAngle == -45) { // 45 Up.
    if (isSmall) {
      drawArrow(width / 2 - 18, height / 2 + 94, angle, color, 1);
    } else {
      drawArrow(width - 50, height / 2 + 30, angle, color, 1);
    }
  } else if (xDrip.arrowAngle == -90) { // Up
    if (isSmall) {
      drawArrow(width / 2 - 2, height / 2 + 100, angle, color, 1);
    } else {
      drawArrow(width - 36, height / 2 + 32, angle, color, 1);
    }
  } else if (xDrip.arrowAngle == -91) { // Double Up
    if (isSmall) {
      drawArrow(width / 2 - 2, height / 2 + 100, angle + 1, color, 2);
    } else {
      drawArrow(width - 36, height / 2 + 32, angle + 1, color, 2);
    }
  }
};
void drawArrow(int x, int y, int aangle, uint16_t color, int arrowCount) {
  float dx = 2*cos(aangle-90)*PI/180+x; // calculate X position
  float dy = 2*sin(aangle-90)*PI/180+y; // calculate Y position
  float x1 = 0;   float y1 = 40;
  float x2 = 20;  float y2 = 20;
  float x3 = -20; float y3 = 20;
  float angle = aangle*PI/180-135;
  float xx1 = x1*cos(angle)-y1*sin(angle)+dx;
  float yy1 = y1*cos(angle)+x1*sin(angle)+dy;
  float xx2 = x2*cos(angle)-y2*sin(angle)+dx;
  float yy2 = y2*cos(angle)+x2*sin(angle)+dy;
  float xx3 = x3*cos(angle)-y3*sin(angle)+dx;
  float yy3 = y3*cos(angle)+x3*sin(angle)+dy;
  M5.Lcd.fillTriangle(xx1,yy1,xx3,yy3,xx2,yy2, color);
  if (arrowCount == 2) {
    if (aangle > 0) {
      drawArrowLine(x, y, xx1, yy1, -6, -12, color);
      drawArrowLine(x, y, xx1, yy1, 6, -12, color);
    } else {
      drawArrowLine(x, y, xx1, yy1, -6, 12, color);
      drawArrowLine(x, y, xx1, yy1, 6, 12, color);
    }
  } else {
    drawArrowLine(x, y, xx1, yy1, 0, 0, color);
  }
};
void drawArrowLine(int x, int y, int xx1, int yy1, int xIncrease, int yIncrease, uint16_t color) {
    M5.Lcd.drawLine(x + xIncrease, y, xx1 + xIncrease, yy1 + yIncrease, color);
    M5.Lcd.drawLine(x+1 + xIncrease, y, xx1+1 + xIncrease, yy1 + yIncrease, color);
    M5.Lcd.drawLine(x + xIncrease, y+1, xx1 + xIncrease, yy1+1 + yIncrease, color);
    M5.Lcd.drawLine(x-1 + xIncrease, y, xx1-1 + xIncrease, yy1 + yIncrease, color);
    M5.Lcd.drawLine(x + xIncrease, y-1, xx1 + xIncrease, yy1-1 + yIncrease, color);
    M5.Lcd.drawLine(x+2 + xIncrease, y, xx1+2 + xIncrease, yy1 + yIncrease, color);
    M5.Lcd.drawLine(x + xIncrease, y+2, xx1 + xIncrease, yy1+2 + yIncrease, color);
    M5.Lcd.drawLine(x-2 + xIncrease, y, xx1-2 + xIncrease, yy1 + yIncrease, color);
    M5.Lcd.drawLine(x + xIncrease, y-2, xx1 + xIncrease, yy1-2 + yIncrease, color);
};
unsigned long getLocalTimeInSeconds() {
  // Start by trying timestamp received from BLE client, if so calculate it and return if not get it from ble server
  // it is only after having passed here two times, that this can succeed, if localTimeFromBLE = 0, and ble is connected then we'll get the time
  if (xDrip.timeInfo.localTimeFromBLE > 0L) {
    return xDrip.timeInfo.localTimeFromBLE + (millis() - xDrip.timeInfo.millsSinceBLE)/1000;
  }
  /// using ble, only if connected and authenticated, ask client to send time
  if (bleDeviceConnected && bleAuthenticated && pRxTxCharacteristic != NULL) {
    char emptyString[] = "";
    sendTextToBLEClient(emptyString, 0x11, 0);
  }
  return 0;
};
unsigned long getUTCTimeInSeconds() {
  unsigned long localTimeInSeconds = getLocalTimeInSeconds();
  if (localTimeInSeconds > 0) {
    return localTimeInSeconds - xDrip.timeInfo.localTimeToUTC;
  } else {
    return 0;
  }
};
int sizeOfStringInCharArray(char stringtext[], int arraysize) {
  // returns index of first occurrence of value '0', which can be considered as end of string 
  // maxsize must be set to size of the array, for some reason getting the size of stringtext by call .lenght() or sizeof doesn't work. To avoid run-time errors, the maxsize must be passed as parameter
 int returnValue = 0;
  while (uint8_t(stringtext[returnValue]) != 0 && returnValue < arraysize) {
    returnValue = returnValue + 1;
  }
  return returnValue;
};
void hideDelta() {
  if (M5.Lcd.width() > 135) {
    M5.Lcd.fillRect(110, 1, M5.Lcd.width() - 110, 40, TFT_BLACK);
  } else {
    M5.Lcd.fillRect(1, 48, M5.Lcd.width(), 30, TFT_BLACK);
  }
};
void printDelta() {
  int delta = calculateDelta();
  if (delta == 1000) {
    hideDelta();
  } else {
    char deltaStr[255];
    if (delta > 0) {
      if (xDrip.show_mgdl) {
        sprintf(deltaStr, "+%d mg/dl", delta);
      } else {
        sprintf(deltaStr, "+%4.1f mmol/l", delta/18.0);
      }
    } else {
      if (xDrip.show_mgdl) {
        sprintf(deltaStr, "%d mg/dl", delta);
      } else {
        sprintf(deltaStr, "%4.1f mmol/l", delta/18.0);
      }
    }

    hideDelta();
    if (xDrip.show_mgdl) {
      M5.Lcd.setFreeFont(FSSB12);
    } else {
      M5.Lcd.setFreeFont(FSSB9);
    }
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    if (M5.Lcd.width() > 135) {
      M5.Lcd.setTextDatum(MR_DATUM);
      M5.Lcd.drawString(deltaStr, M5.Lcd.width() - 16, 21, GFXFF);
    } else {
      M5.Lcd.setTextDatum(MC_DATUM);
      M5.Lcd.drawString(deltaStr, M5.Lcd.width() / 2, 60, GFXFF);
    }
  }
};
bool hasPreviousReadings() {
  bool isCompleted = true;
  for (int i = 0; i < 4; i++) {
    if (xDrip.bgReadings[i].glycemia == 0) {
      isCompleted = false;
    }
  }
  return isCompleted;
};
int calculateDelta() {
  int delta = 1000;
  for (int i=3; i>=0; i--) {
    if (xDrip.timeInfo.sensTime - xDrip.bgReadings[i].time <= 300) { // Get earlier reading that is 5 minutes or less away from current reading
      delta = xDrip.glycemia - xDrip.bgReadings[i].glycemia;
      break;
    }
  }
  return delta;
};
void updateReadings() {
  for (int i = 3; i > 0; i--) {
    xDrip.bgReadings[i] = xDrip.bgReadings[i-1];
  }
  xDrip.bgReadings[0].glycemia = xDrip.glycemia;
  xDrip.bgReadings[0].time = xDrip.timeInfo.sensTime;
  xDrip.prevArrowAngle = xDrip.arrowAngle;
};
void printMinsAgo() {
  uint16_t tdColor = TFT_DARKGREY;
  if (xDrip.timeInfo.sensorDifMin > 5) {
    tdColor = TFT_WHITE;
    if (xDrip.timeInfo.sensorDifMin > 15) {
      tdColor = TFT_RED;
    }
  }

  char sensorDifStr[255];
  if(xDrip.timeInfo.sensorDifMin > 59) {
    strcpy(sensorDifStr, "Error");
  } else {
    sprintf(sensorDifStr, "%d min", xDrip.timeInfo.sensorDifMin);
  }
      
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setFreeFont(FSSB12);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_BLACK, tdColor);
  if (M5.Lcd.width() > 135) {
    M5.Lcd.fillRoundRect(16, 8, 90, 26, 5, tdColor);
    M5.Lcd.drawString(sensorDifStr, 58, 19, GFXFF);
  } else {
    M5.Lcd.fillRoundRect(M5.Lcd.width() / 2 - 45, 15, 90, 26, 5, tdColor);
    M5.Lcd.drawString(sensorDifStr, M5.Lcd.width() / 2, 26, GFXFF);
  }
};

//// BLE declarations ////
class BLECharacteristicCallBack: public BLECharacteristicCallbacks {
  
  void onWrite(BLECharacteristic *pCharacteristic) {
    
    std::string rxValue = pCharacteristic->getValue();

    if (rxValue.length() > 0) {
        
      for (int i = 0; i < maxBytesInOneBLEPacket;i++) {
        rxValueAsByteArray[i] = 0x00;
      }
      memcpy(rxValueAsByteArray, rxValue.c_str(), rxValue.length());

      // see what server has been sending
      byte opCode = rxValueAsByteArray[0];
      char emptyString[] = "";

      switch (opCode) {
        
        /// codes for writing from client to server
        case 0x03:{
          char * unitismgdl = new char[6];// value is literally "true" or "false"
          std::strcpy (unitismgdl, rxValue.c_str() + 3);// starts at postion 4, because split in packets is used
          if (strcmp(unitismgdl, "true") == 0) {
            xDrip.show_mgdl = true;
          } else {
            xDrip.show_mgdl = false;
          }
          updateGlycemia(true, false);
          delete[] unitismgdl;
        }
        break;

        case 0x09: { // Read BLE Passsword
          // client is requesting the blepassword - if cfg.blepassword has length > 0, then it means it should already be known by the app, we won't send it. 
          // this is to prevent that some other app requests it
          if (sizeOfStringInCharArray(blepassword, 64) == 0) {// here in this case useConfiguredBlePassword must be false
            for(int i = 0; i<10  ; i++) {
              blepassword[i] = letters[random(0, 36)];
            }
            sendTextToBLEClient(blepassword, 0x0E, 64);
            // considering this case as authenticated
            bleAuthenticated = true;     
          } else { // useConfiguredBlePassword is false and cfg.blepassword already exists, means it's already randomly generated
            sendTextToBLEClient(emptyString, 0x0F, 0);
          }
        }
        break;
            
        case 0x0A: { //authenticateTx
          // client is trying authentication, in case cfg.blepassword is currently 0, then we're in a situation where there's no password in the config, a new random password hasn't been
          // generated yet, the app still has an old stored temporary password, but which needs to be renewed
          if (sizeOfStringInCharArray(blepassword, 64) == 0) {
            for(int i = 0; i<10; i++) {
              blepassword[i] = letters[random(0, 36)];
            }
            sendTextToBLEClient(blepassword, 0x0E, 64);
            // considering this case as authenticated
            bleAuthenticated = true;
          } else {
            // verify the password
            bool passwordMatch = false;
            if ((sizeOfStringInCharArray(blepassword, 64) + 1) == rxValue.length()) {// rxValue is opCode + password, meaning should be 1 longer than actual password, if that's not the case then password doesn't match
              passwordMatch = true;
              for (int i = 0; i < sizeOfStringInCharArray(blepassword, 64); i++) {
                if (blepassword[i] != rxValueAsByteArray[i+1]) {
                  passwordMatch = false;
                  break;
                }
              }
            }

            if (passwordMatch) {
              sendTextToBLEClient(emptyString, 0x0B, 0);
              bleAuthenticated = true;
            } else {
              sendTextToBLEClient(emptyString, 0x0C, 0);
              bleAuthenticated = false;
            }
          }
        }
        break;

        case 0x10: { // BG Reading
          // should be one packet, will not start to compose packets, string starts at byte 3 (ie index 2), here it's multiplied with 2
          if (bleAuthenticated) {
            // first field is bgReading in mgdl as Int, which starts in byte 3 of rxValue.c_str(),
            // first copy to c string because functions used require this
            char * cstr = new char [rxValue.length() + 1 - 3];
            std::strcpy (cstr, rxValue.c_str() + 3);

            // splitByBlanc is assigned to first field before the blanc
            char * splitByBlanc = strtok (cstr," ");
            xDrip.glycemia = atoi(splitByBlanc);
            xDrip.glycemiaMmolL = xDrip.glycemia/18.0;
            // strange but this assigned splitByBlanc to the next field
            splitByBlanc = strtok (NULL," ");
            xDrip.timeInfo.sensTime = uint64_t(strtoul(splitByBlanc, NULL, 10));

            updateGlycemia(false, true);
            delete[] cstr;
          } else {
            Serial.println(F("BLE Not authenticated"));
          }
        }
        break;

        case 0x12: {
          // should be one packet, will not start to compose packets, string starts at byte 3 (ie index 2), here it's multiplied with 2
          if (bleAuthenticated) {
            xDrip.timeInfo.localTimeFromBLE = strtoul(rxValue.c_str() + 3, NULL, 0);
            xDrip.timeInfo.millsSinceBLE = millis();

            if (initialBLEStartUpOnGoing) {
              // ask upate all parameters
              sendTextToBLEClient(emptyString, 0x16, 0);
              initialBLEStartUpOnGoing = false;
            }
          }
        }
        break;

        case 0x13: { // Arrow Slope
          if (bleAuthenticated) {
            strlcpy(xDrip.sensDir, rxValue.c_str() + 3, 32);
            setArrowAngle();  
          }        
        }
        break;

        case 0x14: {
          if (bleAuthenticated) {
            xDrip.timeInfo.localTimeToUTC = strtoul(rxValue.c_str() + 3, NULL, 0);
          }
        }
        break;
      }
    }
  }
};
class BLEServerCallBack: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    Serial.println(F("BLE connect"));
    bleDeviceConnected = true;
  };

  void onDisconnect(BLEServer* pServer) {
    Serial.println(F("BLE disconnect"));
    bleDeviceConnected = false;

    // need to set authenticated to false, because the M5Stack will immediately start advertising again, so another client might connect
    bleAuthenticated = false;

    if (pServer != NULL) {
      pServer->getAdvertising()->start();
    }
      
  }
};
void setupBLE() {
  Serial.begin(115200);
  Serial.println(F("Starting BLE"));

  BLEDevice::init(BLE_DeviceName.c_str());
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BLEServerCallBack());
  pService = pServer->createService(BLE_SERVICE_UUID.c_str());
  // PROPERTY_WRITE_NR : client can write a value to the characteristic, client does not expect response
  // PROPERTY_NOTIFY : server can inform client that there's a new value, confirmation from client back to server is not needed
  // in fact pRxTxCharacteristic will be used to exchange data. Write is from client to server, for example if client needs to know information, it will write a specific command
  //   to the server, the server will respond with a notify  
  pRxTxCharacteristic = pService->createCharacteristic(BLE_CHARACTERISTIC_UUID.c_str(), BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  // needed to make it work as notifier
  pRxTxCharacteristic->addDescriptor(new BLE2902()); 
  pRxTxCharacteristic->setCallbacks(new BLECharacteristicCallBack());
  // Start the service
  pService->start();
  // Start advertising
  pServer->getAdvertising()->start();
};
void sendTextToBLEClient(char * textToSend, uint8_t opCode, int maximumSizeOfTextToSend) {
  // sends opCode and textToSend to characteristic. Possibly split in multiple packets
  // first byte = opcode, second byte = packet number, third byte = number of packets in total

  int sizeOfTextToSend = sizeOfStringInCharArray(textToSend, maximumSizeOfTextToSend);

  // if sizeOfTextToSend = 0, then it means there's just an opcode being sent
  if (sizeOfTextToSend == 0) {
    uint8_t dataToSend[3];
    dataToSend[0] = opCode;
    dataToSend[1] = 0x01;
    dataToSend[2] = 0x01;
    pRxTxCharacteristic->setValue(dataToSend, 3);
    pRxTxCharacteristic->notify();
    return;
  }

  // text may be longer than maximum ble packet size, need to split up, this variable tells us how many characters are already sent
  int charactersSent = 0;

  // number of packets is total size / (maxBytesInOneBLEPacket - 3) , possibly + 1
  int totalNumberOfPacketsToSent = sizeOfTextToSend/(maxBytesInOneBLEPacket - 3);
  if (sizeOfTextToSend > totalNumberOfPacketsToSent * (maxBytesInOneBLEPacket - 3)) {
    totalNumberOfPacketsToSent++;
  }

  // number of the next packet to send
  int numberOfNextPacketToSend = 1;

  // send them one by one
  while (charactersSent < sizeOfTextToSend) {
    // calculate size of packet to send
    int sizeOfNextPacketToSend = maxBytesInOneBLEPacket;
    if (numberOfNextPacketToSend == totalNumberOfPacketsToSent) {
      sizeOfNextPacketToSend = 3 + (sizeOfTextToSend - charactersSent);//First byte = opcode, second byte is packet number, third byte = total number of packets, rest is content
    }

    // craete and populate dataToSend
    uint8_t dataToSend[sizeOfNextPacketToSend];
    dataToSend[0] = opCode;
    dataToSend[1] = numberOfNextPacketToSend;
    dataToSend[2] = totalNumberOfPacketsToSent;
    for (int i = 0; i < sizeOfNextPacketToSend - 3; i++) {
      dataToSend[i + 3] = uint8_t(textToSend[charactersSent + i]);
    }

    // send the data 
    pRxTxCharacteristic->setValue(dataToSend, sizeOfNextPacketToSend);
    pRxTxCharacteristic->notify();

    // increase charactersSent
    charactersSent = charactersSent + (sizeOfNextPacketToSend - 3);

    // increase numberOfNextPacketToSend
    numberOfNextPacketToSend++;             
  }

};