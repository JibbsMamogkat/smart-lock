/*
  PROJECT: Solar-Powered Smart Lock - NodeMCU Firebase Bridge (FSM Version)
  DESCRIPTION: This version uses a formal state machine to manage WiFi/Firebase
  connections and communication with the Arduino, making it non-blocking and resilient.
*/

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <FirebaseESP8266.h>

// --- FIREBASE CONFIG ---
#define FIREBASE_HOST "https://smart-lock-app-4123a-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "HJY2VyeaNsORzCL5HFqUoiUwSGDErXsnxH0WCs5m"
String lockPath = "/smart_lock";

// --- PIN DEFINITIONS (from Arduino) ---
const int TAMPER_PIN = D1;
const int REG_MODE_PIN = D2;
const int LOCK_STATUS_PIN = D5;

// =================================================================
// --- STATE MACHINE DEFINITIONS ---
// =================================================================

enum State {
  STATE_WIFI_CONNECT,
  STATE_FIREBASE_CONNECT,
  STATE_OPERATIONAL,
  STATE_DISCONNECTED
};

State currentState;

// --- GLOBAL VARIABLES & TIMERS ---
FirebaseData fbdo;
FirebaseConfig config;
FirebaseAuth auth;

unsigned long g_reconnectTimer = 0;
unsigned long g_regModeTimer = 0;
unsigned long g_tamperAlertTimer = 0;

bool g_inRegMode = false;
bool g_inTamperAlert = false;

// =================================================================
// --- SETUP & MAIN LOOP ---
// =================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(TAMPER_PIN, INPUT);
  pinMode(REG_MODE_PIN, INPUT);
  pinMode(LOCK_STATUS_PIN, INPUT);
  
  enterState_WifiConnect();
}

void loop() {
  switch (currentState) {
    case STATE_WIFI_CONNECT:      handleState_WifiConnect();      break;
    case STATE_FIREBASE_CONNECT:  handleState_FirebaseConnect();  break;
    case STATE_OPERATIONAL:       handleState_Operational();      break;
    case STATE_DISCONNECTED:      handleState_Disconnected();     break;
  }
}

// =================================================================
// --- STATE HANDLER FUNCTIONS ---
// =================================================================

void handleState_WifiConnect() {
  // WiFiManager is blocking, so this state will transition immediately
  // upon success or failure.
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(300); // 5-minute timeout for AP

  if (wifiManager.autoConnect("SmartLock-Setup-AP")) {
    enterState_FirebaseConnect();
  } else {
    enterState_Disconnected();
  }
}

void handleState_FirebaseConnect() {
  // Try to connect to Firebase
  if (Firebase.ready()) {
    enterState_Operational();
  } else {
    // If it fails after a few seconds, go to disconnected state to retry
    if (millis() - g_reconnectTimer > 5000) {
      enterState_Disconnected();
    }
  }
}

void handleState_Operational() {
  // Check for connection loss first
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) {
    enterState_Disconnected();
    return;
  }
  
  // Normal operations
  util_handleFirebaseCommands();
  util_handleArduinoSignal();

  // Handle non-blocking timers for temporary modes
  util_checkRegModeTimeout();
  util_checkTamperAlertTimeout();
}

void handleState_Disconnected() {
  // Try to reconnect every 10 seconds
  if (millis() - g_reconnectTimer > 10000) {
    enterState_WifiConnect();
  }
}

// =================================================================
// --- STATE ENTRY FUNCTIONS ---
// =================================================================

void enterState_WifiConnect() {
  currentState = STATE_WIFI_CONNECT;
  Serial.println("WIFI_DISCONNECTED"); // Inform Arduino we are disconnected
  // The handle function will now run and block until WiFiManager finishes.
}

void enterState_FirebaseConnect() {
  currentState = STATE_FIREBASE_CONNECT;
  g_reconnectTimer = millis(); // Start a connection timeout
  
  Serial.println("WIFI_CONNECTED"); // Inform Arduino we are connected

  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void enterState_Operational() {
  currentState = STATE_OPERATIONAL;
  util_logFirebaseSuccess("System Online and Operational.");
  output_updateFirebaseBool("status/isOnline", true);
  output_updateFirebaseInt("status/lastSeen", time(nullptr));
}

void enterState_Disconnected() {
  currentState = STATE_DISCONNECTED;
  g_reconnectTimer = millis(); // Start the retry timer
  Serial.println("WIFI_DISCONNECTED"); // Inform Arduino we are disconnected
  output_updateFirebaseBool("status/isOnline", false);
}

// =================================================================
// --- INPUT & OUTPUT FUNCTIONS ---
// =================================================================

int input_readArduinoSignal() {
  bool bit2 = digitalRead(REG_MODE_PIN);  // MSB
  bool bit1 = digitalRead(TAMPER_PIN);
  bool bit0 = digitalRead(LOCK_STATUS_PIN); // LSB
  return (bit2 << 2) | (bit1 << 1) | bit0;
}

void output_sendToArduino(char cmd) {
  Serial.write(cmd);
}

void output_updateFirebaseBool(String path, bool value) {
  if (!Firebase.setBool(fbdo, lockPath + "/" + path, value)) {
    util_logFirebaseError("setBool: " + path);
  }
}

void output_updateFirebaseString(String path, String value) {
  if (!Firebase.setString(fbdo, lockPath + "/" + path, value)) {
    util_logFirebaseError("setString: " + path);
  }
}

void output_updateFirebaseInt(String path, int value) {
    if (!Firebase.setInt(fbdo, lockPath + "/" + path, value)) {
        util_logFirebaseError("setInt: " + path);
    }
}


// =================================================================
// --- UTILITY FUNCTIONS ---
// =================================================================

void util_handleFirebaseCommands() {
  if (Firebase.getString(fbdo, lockPath + "/command")) {
    String command = fbdo.stringData();
    if (command.length() > 0 && command != "null") {
      if (command == "lock") {
        output_sendToArduino('L');
        util_logFirebaseSuccess("Sent 'L' to Arduino");
      } else if (command == "unlock") {
        output_sendToArduino('U');
        util_logFirebaseSuccess("Sent 'U' to Arduino");
      }
      // Clear the command after processing
      output_updateFirebaseString("command", "");
    }
  }
}

void util_handleArduinoSignal() {
  int signal = input_readArduinoSignal();
  if (signal == 0) return; // Ignore idle state

  switch (signal) {
    case 0b001: // Locked
      output_updateFirebaseBool("status/isLocked", true);
      break;
    case 0b010: // Tamper
      if (!g_inTamperAlert) {
          g_inTamperAlert = true;
          g_tamperAlertTimer = millis();
          output_updateFirebaseString("status/alert", "knock");
      }
      break;
    case 0b011: // Unlocked
      output_updateFirebaseBool("status/isLocked", false);
      break;
    case 0b100: // Registration Mode
      if (!g_inRegMode) {
          g_inRegMode = true;
          g_regModeTimer = millis();
          output_updateFirebaseString("status/mode", "registration");
      }
      break;
    case 0b111: // WiFi Reset
      {
        WiFiManager wifiManager;
        wifiManager.resetSettings();
        ESP.restart();
      }
      break;
  }
}

void util_checkRegModeTimeout() {
  if (g_inRegMode && (millis() - g_regModeTimer > 60000)) {
    g_inRegMode = false;
    output_updateFirebaseString("status/mode", "normal");
    util_logFirebaseSuccess("Registration mode timed out.");
  }
}

void util_checkTamperAlertTimeout() {
  if (g_inTamperAlert && (millis() - g_tamperAlertTimer > 5000)) {
    g_inTamperAlert = false;
    output_updateFirebaseString("status/alert", "none");
    util_logFirebaseSuccess("Tamper alert cleared.");
  }
}

void util_logFirebaseError(String context) {
  String errorMessage = "CTX: " + context + " | ERR: " + fbdo.errorReason();
  Serial.println(errorMessage);
  // Optional: Log to a separate path in Firebase
  // Firebase.pushString(fbdo, lockPath + "/errorLog", errorMessage);
}

void util_logFirebaseSuccess(String context) {
  Serial.println("OK: " + context);
}
