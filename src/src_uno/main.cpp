#include <Arduino.h>
/*
PROJECT: Solar-Powered Smart Lock - Arduino Master Controller (FINAL VERSION - CLEANED)
*/

#include <Keypad.h>
#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h> // Optional, not yet implemented

// --- PIN DEFINITIONS ---
const int VIBRATION_PIN = 2;
const int SERVO_PIN = 9;
const int BUZZER_PIN = 13;
const int c = A0;
const int LOCK_STATUS_PIN = A1;
const int TRIGGER_TAMPER_PIN = 7;
const int TRIGGER_REG_MODE_PIN = 6;
const int REED_PIN = A3;
const int RED_LED_PIN = A0;


// --- SERVO ANGLES ---
const int LOCKED_ANGLE = 90;
const int UNLOCKED_ANGLE = 0;

// --- KEYPAD SETUP ---
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'C'},
  {'7', '8', '9', 'B'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {3, 4, 5, 8};
byte colPins[COLS] = {10, 11, 12, A2};
Keypad customKeypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- LCD & SERVO ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo myLockServo;

// --- STATE VARIABLES ---
String inputPassword = "";
String masterPassword = "1234";
String adminCode = "9999";
bool isCurrentlyLocked = true;
bool inEventDisplay = false;
volatile bool tamperDetectedFlag = false;
String lastWiFiStatus = "WiFi: Unknown    ";
String incomingSerial = "";

bool isTyping = false;


unsigned long previousWiFiMillis = 0;
unsigned long previousLockMillis = 0;
const unsigned long wifiInterval = 2000;
const unsigned long lockInterval = 10000;


void setup() {
  Serial.begin(115200);
  myLockServo.attach(SERVO_PIN);
  lcd.init(); lcd.backlight();

  pinMode(LOCK_STATUS_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIGGER_TAMPER_PIN, OUTPUT);
  pinMode(TRIGGER_REG_MODE_PIN, OUTPUT);
  pinMode(REED_PIN, INPUT);

  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(TRIGGER_TAMPER_PIN, LOW);
  digitalWrite(TRIGGER_REG_MODE_PIN, LOW);

  pinMode(VIBRATION_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(VIBRATION_PIN), onVibration, FALLING);
 
  initializeLock();
}

void loop() {
  checkTamper();
  readSerialInput();
  checkKeypad();
  // isLedStatus();

  if (!inEventDisplay && !isTyping) { // <-- Add !isTyping here
    unsigned long currentMillis = millis();
    if (currentMillis - previousWiFiMillis >= wifiInterval) {
      previousWiFiMillis = currentMillis;
      lcd.setCursor(0, 1); lcd.print(lastWiFiStatus);
    }
    if (currentMillis - previousLockMillis >= lockInterval) {
      previousLockMillis = currentMillis;
      refreshLockDisplay();
    }
  }
}

void initializeLock() {
  if (digitalRead(REED_PIN) == LOW) {
    lockServo();
  } else {
    unlockServo();
  }
}

// void checkReedSwitch() {
//   static bool lastState = HIGH;
//   bool currentState = digitalRead(REED_PIN);

//   if (currentState != lastState) {
//     delay(100); // debounce
//     currentState = digitalRead(REED_PIN);
//     if (currentState != lastState) {
//       lastState = currentState;
//       if (currentState == LOW) {
//         // Magnet is near → unlocked
//         unlockServo();
//       } else {
//         // Magnet is far → locked
//         lockServo();
//       }
//     }
//   }
// }

// === INPUT ===
void checkKeypad() {
  char key = customKeypad.getKey();
  if (!key) return;

  if (inputPassword.length() == 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Enter PIN:");
    isTyping = true;  // Start typing
  }

  if (key == '#' && inputPassword.length() > 0) {
    isTyping = false; // Done typing
    processPassword();
  } else if (key == '*') {
    isTyping = false; // Cleared input
    inputPassword = "";
    refreshLockDisplay();
  } else {
    inputPassword += key;
    lcd.setCursor(0, 1);
    lcd.print(inputPassword);
  }
}

void processPassword() {
  if (inputPassword == masterPassword) {
    toggleLock();
    delay(5000);
  } else if (inputPassword == adminCode) {
    enableRegistrationMode();
  } else {
    inEventDisplay = true;
    lcd.clear();
    lcd.print("Wrong PIN!");
    beep(500);
    delay(2000);
    inEventDisplay = false;
    refreshLockDisplay();
  }
  inputPassword = "";
}

void checkTamper() {
  if (tamperDetectedFlag) {
    inEventDisplay = true;
    lcd.clear();
    lcd.print("!!! TAMPER !!!");
    Serial.println("Tamper detected!");
    beep(100); delay(50); beep(100); delay(50); beep(100);
    signalToNodeMCU(false, true, false); // 0 1 0
    delay(1000);
    tamperDetectedFlag = false;
    inEventDisplay = false;
    refreshLockDisplay();
  }
}

void onVibration() {
  tamperDetectedFlag = true;
}

// === SERIAL COMM ===
void readSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();

    // Check for single-character commands from NodeMCU
    if (c == 'L') {
      if (!isCurrentlyLocked) {
        lockServo();
      }
      continue;
    } else if (c == 'U') {
      if (isCurrentlyLocked) {
        unlockServo();
      }
      continue;
    }

    // Else, build a string (e.g., "WIFI_CONNECTED")
    if (c == '\n' || c == '\r') {
      if (incomingSerial.length() > 0) {
        handleSerialCommand(incomingSerial);
        incomingSerial = "";
      }
    } else {
      incomingSerial += c;
    }
  }
}


void handleSerialCommand(String cmd) {
  if (cmd == "WIFI_CONNECTED") {
    lastWiFiStatus = "WiFi: Connected   ";
  } else if (cmd == "WIFI_DISCONNECTED") {
    lastWiFiStatus = "WiFi: Disconnected";
  } else {
    Serial.print("Unknown command: "); Serial.println(cmd);
  }
}
// === STATE CONTROL ===
void toggleLock() {
  if (isCurrentlyLocked) {unlockServo();
  // delay(5000);
  }
  else lockServo();
}

void lockServo() {
  myLockServo.attach(SERVO_PIN);
  myLockServo.write(LOCKED_ANGLE);
  delay(200);
  myLockServo.detach();
  isCurrentlyLocked = true;
  signalToNodeMCU(false, false, true); // 0 0 1
  delay(500);
  beep(200);
  refreshLockDisplay();
  // sendLockState(); 
}

void unlockServo() {
  myLockServo.attach(SERVO_PIN);
  myLockServo.write(UNLOCKED_ANGLE);
  delay(200);
  myLockServo.detach();
  isCurrentlyLocked = false;
  signalToNodeMCU(false, true, true); // 0 1 1
  delay(500);
  beep(200);

  refreshLockDisplay();

  
  // sendLockState(); 
}

void enableRegistrationMode() {
  inEventDisplay = true;
  Serial.println("Enabling Registration Mode...");
  lcd.clear();
  lcd.print("Reg. Mode ON");
  beep(100); delay(50); beep(100);
  signalToNodeMCU(true, false, false); // 1 0 0
  delay(2000);
  inEventDisplay = false;
  refreshLockDisplay();
}

// === DISPLAY & SIGNAL ===
void refreshLockDisplay() {
  lcd.setCursor(0, 0);
  lcd.print("Status:         ");
  lcd.setCursor(0, 0);
  if (isCurrentlyLocked) {
    lcd.print("Status: LOCKED  ");
    digitalWrite(RED_LED_PIN, HIGH); 
    // digitalWrite(BLUE_LED_PIN, LOW);
  } else {
    lcd.print("Status: UNLOCKED");
    digitalWrite(RED_LED_PIN, LOW);
    // digitalWrite(BLUE_LED_PIN, HIGH);
  }
}

void signalToNodeMCU(bool bit6, bool bit7, bool bitA1) {
  digitalWrite(TRIGGER_REG_MODE_PIN, bit6);  //D2     // Pin 6 → Bit 2
  digitalWrite(TRIGGER_TAMPER_PIN, bit7);   //D1      // Pin 7 → Bit 1
  digitalWrite(LOCK_STATUS_PIN, bitA1);  // D5            // A1 → Bit 0
  delay(2000);
  // Clear all signals back to LOW
  digitalWrite(LOCK_STATUS_PIN, LOW); 
  digitalWrite(TRIGGER_TAMPER_PIN, LOW);
  digitalWrite(TRIGGER_REG_MODE_PIN, LOW);
}


void beep(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}