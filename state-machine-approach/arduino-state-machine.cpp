/*
  PROJECT: Solar-Powered Smart Lock - Refactored with State Machine
  DESCRIPTION: This version uses a formal state machine to manage the lock's
  behavior, making it non-blocking, responsive, and easier to maintain.
*/

#include <Keypad.h>
#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --- PIN DEFINITIONS ---
const int VIBRATION_PIN = 2;
const int SERVO_PIN = 9;
const int BUZZER_PIN = 13;
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

// =================================================================
// --- STATE MACHINE DEFINITIONS ---
// =================================================================

enum State {
  STATE_LOCKED,
  STATE_UNLOCKED,
  STATE_AWAITING_PIN,
  STATE_ADMIN_MODE,
  STATE_SHOWING_MESSAGE,
  STATE_ALARM
};

State currentState;
State previousState; // To know where to return to after a message

// --- GLOBAL VARIABLES & CONFIG ---
String g_inputPassword = "";
String g_masterPassword = "1234";
String g_adminCode = "9999";
String g_lastWiFiStatus = "WiFi: Unknown";

// Timers for non-blocking operations
unsigned long g_stateTimer = 0;
unsigned long g_wifiDisplayTimer = 0;

// Interrupt flag for tamper detection
volatile bool g_tamperDetectedFlag = false;

// =================================================================
// --- SETUP & MAIN LOOP ---
// =================================================================

void setup() {
  Serial.begin(115200);
  myLockServo.attach(SERVO_PIN);
  lcd.init();
  lcd.backlight();

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LOCK_STATUS_PIN, OUTPUT);
  pinMode(TRIGGER_TAMPER_PIN, OUTPUT);
  pinMode(TRIGGER_REG_MODE_PIN, OUTPUT);
  pinMode(REED_PIN, INPUT_PULLUP); // Use INPUT_PULLUP for switches

  // Attach interrupt for vibration sensor
  pinMode(VIBRATION_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(VIBRATION_PIN), onVibration, FALLING);

  // Initialize the lock to its correct starting state based on the reed switch
  if (digitalRead(REED_PIN) == LOW) { // Assuming LOW means door is closed
      enterState_Locked();
  } else {
      enterState_Unlocked();
  }
}

void loop() {
  // The state machine's "engine". It calls the handler for the current state.
  switch (currentState) {
    case STATE_LOCKED:          handleState_Locked();           break;
    case STATE_UNLOCKED:        handleState_Unlocked();         break;
    case STATE_AWAITING_PIN:    handleState_AwaitingPin();      break;
    case STATE_ADMIN_MODE:      handleState_AdminMode();        break;
    case STATE_SHOWING_MESSAGE: handleState_ShowingMessage();   break;
    case STATE_ALARM:           handleState_Alarm();            break;
  }
  
  // This non-state-dependent task can run on every loop
  util_updateWifiDisplay();
}

// =================================================================
// --- STATE HANDLER FUNCTIONS ---
// (Decide what to do in each state)
// =================================================================

void handleState_Locked() {
  // Check for trigger events
  if (input_vibrationDetected()) {
    enterState_Alarm();
    return;
  }
  
  String cmd = input_readSerial();
  if (cmd == "U") {
    enterState_Unlocked();
    return;
  } else {
    util_handleWifiCommand(cmd);
  }

  char key = input_checkKeypad();
  if (key != NO_KEY) {
    g_inputPassword += key;
    enterState_AwaitingPin();
    return;
  }
}

void handleState_Unlocked() {
  // Check for trigger events
  String cmd = input_readSerial();
  if (cmd == "L") {
    enterState_Locked();
    return;
  } else {
    util_handleWifiCommand(cmd);
  }

  // Auto-lock after 10 seconds (non-blocking)
  if (millis() - g_stateTimer > 10000) {
    // Check if the door is actually closed before locking
    if (digitalRead(REED_PIN) == LOW) {
      enterState_Locked();
      return;
    }
  }
}

void handleState_AwaitingPin() {
  char key = input_checkKeypad();
  if (key != NO_KEY) {
    g_stateTimer = millis(); // Reset timeout on keypress

    if (key == '#') {
      util_processPassword();
      return;
    } else if (key == '*') {
      enterState_Locked(); // Cancel and go back to locked
      return;
    } else {
      g_inputPassword += key;
      output_updateLCD("", g_inputPassword);
    }
  }

  // Timeout after 10 seconds of inactivity
  if (millis() - g_stateTimer > 10000) {
    enterState_ShowingMessage("Timeout!", 2000, currentState);
  }
}

void handleState_AdminMode() {
  // Stay in this mode for 5 seconds then return to locked
  if (millis() - g_stateTimer > 5000) {
    enterState_Locked();
  }
}

void handleState_ShowingMessage() {
  // This state does nothing but wait for the timer to expire
  if (millis() - g_stateTimer > 0) { // g_stateTimer is set to the DURATION
    // Return to the state we were in before showing the message
    if (previousState == STATE_LOCKED) enterState_Locked();
    else if (previousState == STATE_UNLOCKED) enterState_Unlocked();
    else enterState_Locked(); // Default fallback
  }
}

void handleState_Alarm() {
  // Action: Beep periodically
  output_beep(100, 100);

  // Trigger: Disarm via serial command
  if (input_readSerial() == "DISARM") {
    enterState_Locked();
  }
}


// =================================================================
// --- STATE ENTRY FUNCTIONS ---
// (Perform one-time actions when entering a state)
// =================================================================

void enterState_Locked() {
  currentState = STATE_LOCKED;
  g_inputPassword = "";
  output_moveServo(LOCKED_ANGLE);
  output_updateLCD("Status: LOCKED", g_lastWiFiStatus);
  digitalWrite(RED_LED_PIN, HIGH);
  output_signalToNodeMCU(false, false, true); // 0 0 1
}

void enterState_Unlocked() {
  currentState = STATE_UNLOCKED;
  g_inputPassword = "";
  g_stateTimer = millis(); // Start auto-lock timer
  output_moveServo(UNLOCKED_ANGLE);
  output_updateLCD("Status: UNLOCKED", g_lastWiFiStatus);
  digitalWrite(RED_LED_PIN, LOW);
  output_signalToNodeMCU(false, true, true); // 0 1 1
}

void enterState_AwaitingPin() {
  currentState = STATE_AWAITING_PIN;
  g_stateTimer = millis(); // Start input timeout timer
  output_updateLCD("Enter PIN:", "");
}

void enterState_AdminMode() {
  currentState = STATE_ADMIN_MODE;
  g_stateTimer = millis(); // Start mode timer
  output_updateLCD("Reg. Mode ON", "");
  output_beep(100, 50);
  output_signalToNodeMCU(true, false, false); // 1 0 0
}

void enterState_ShowingMessage(String msg, int duration, State prevState) {
  previousState = prevState; // Save where we came from
  currentState = STATE_SHOWING_MESSAGE;
  g_stateTimer = duration; // The timer check will use this duration
  output_updateLCD(msg, "");
}

void enterState_Alarm() {
  currentState = STATE_ALARM;
  output_updateLCD("!!! TAMPER !!!", "");
  output_signalToNodeMCU(false, true, false); // 0 1 0
}


// =================================================================
// --- INPUT FUNCTIONS ---
// (Read from the outside world)
// =================================================================

char input_checkKeypad() {
  return customKeypad.getKey();
}

bool input_vibrationDetected() {
  if (g_tamperDetectedFlag) {
    g_tamperDetectedFlag = false; // Reset flag after reading
    return true;
  }
  return false;
}

String input_readSerial() {
  static String incomingSerial = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (incomingSerial.length() > 0) {
        String cmd = incomingSerial;
        incomingSerial = "";
        return cmd;
      }
    } else {
      incomingSerial += c;
    }
  }
  return ""; // Return empty string if no command is ready
}

// --- INTERRUPT SERVICE ROUTINE ---
void onVibration() {
  // Keep this extremely short and fast!
  g_tamperDetectedFlag = true;
}

// =================================================================
// --- OUTPUT FUNCTIONS ---
// (Control hardware)
// =================================================================

void output_moveServo(int angle) {
  myLockServo.attach(SERVO_PIN);
  myLockServo.write(angle);
  // The delay and detach logic is often complex. For a simple system,
  // leaving it attached is fine and more responsive.
  // delay(200);
  // myLockServo.detach();
}

void output_updateLCD(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void output_beep(int duration, int pause_after = 0) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration); // Delay is acceptable here for short, simple beeps
  digitalWrite(BUZZER_PIN, LOW);
  if (pause_after > 0) {
    delay(pause_after);
  }
}

void output_signalToNodeMCU(bool bit6, bool bit7, bool bitA1) {
  digitalWrite(TRIGGER_REG_MODE_PIN, bit6);
  digitalWrite(TRIGGER_TAMPER_PIN, bit7);
  digitalWrite(LOCK_STATUS_PIN, bitA1);
  // This delay might be necessary for the NodeMCU to read the pins.
  // A non-blocking alternative would be more complex (e.g., another state).
  delay(200); 
  digitalWrite(TRIGGER_REG_MODE_PIN, LOW);
  digitalWrite(TRIGGER_TAMPER_PIN, LOW);
  digitalWrite(LOCK_STATUS_PIN, LOW);
}


// =================================================================
// --- UTILITY FUNCTIONS ---
// (Internal logic and helpers)
// =================================================================

void util_processPassword() {
  if (g_inputPassword == g_masterPassword) {
    output_beep(200);
    // Toggle the lock state
    if (currentState == STATE_LOCKED || currentState == STATE_AWAITING_PIN) {
        enterState_Unlocked();
    } else {
        enterState_Locked();
    }
  } else if (g_inputPassword == g_adminCode) {
    enterState_AdminMode();
  } else {
    output_beep(500);
    enterState_ShowingMessage("Wrong PIN!", 2000, STATE_LOCKED);
  }
}

void util_updateWifiDisplay() {
  if (millis() - g_wifiDisplayTimer > 2000) {
    g_wifiDisplayTimer = millis();
    // Only update the second line if we are in a "calm" state
    if (currentState == STATE_LOCKED || currentState == STATE_UNLOCKED) {
       lcd.setCursor(0, 1);
       lcd.print(g_lastWiFiStatus);
    }
  }
}

void util_handleWifiCommand(String cmd) {
  if (cmd == "WIFI_CONNECTED") {
    g_lastWiFiStatus = "WiFi: Connected";
  } else if (cmd == "WIFI_DISCONNECTED") {
    g_lastWiFiStatus = "WiFi: Disconnected";
  }
}
