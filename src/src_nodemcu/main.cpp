#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <FirebaseESP8266.h>

#define FIREBASE_HOST "https://smart-lock-app-4123a-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "HJY2VyeaNsORzCL5HFqUoiUwSGDErXsnxH0WCs5m"

const int TAMPER_WAKE_PIN = D1;
const int REG_MODE_WAKE_PIN = D2;
const int LOCK_STATUS_PIN = D5;
unsigned long lastSerialCheckTime = 0;
const unsigned long SERIAL_CHECK_INTERVAL = 5000; // 5 seconds
bool serialReceivedInLastInterval = false;


FirebaseData fbdo;
FirebaseConfig config;
FirebaseAuth auth;
String lockPath = "/smart_lock";

void setup() {
  initializeSerialAndPins();
  connectWiFi();
  initializeFirebase();
  setInitialFirebaseStatus();
}

void loop() {
  handleFirebaseCommand();
  processWakePins();
  // connectWiFi();
  delay(1000);
}

// =======================
// == INITIALIZATION ====
// =======================
void initializeSerialAndPins() {
  Serial.begin(115200);
  delay(100);

  pinMode(TAMPER_WAKE_PIN, INPUT);
  pinMode(REG_MODE_WAKE_PIN, INPUT);
  pinMode(LOCK_STATUS_PIN, INPUT);
}

void connectWiFi() {
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(600);

  if (!wifiManager.autoConnect("SmartLock-Setup-AP")) {
    Serial.println("WIFI_DISCONNECTED");
    return;
  }

  Serial.println("WIFI_CONNECTED");
}

void initializeFirebase() {
  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(500);
}

void setInitialFirebaseStatus() {
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    if (!Firebase.setBool(fbdo, lockPath + "/status/isOnline", true)) {
      logFirebaseError("Setting isOnline TRUE in setup");
    } else {
      logFirebaseSuccess("isOnline TRUE set in setup");
    }

    if (!Firebase.setInt(fbdo, lockPath + "/status/lastSeen", time(nullptr))) {
      logFirebaseError("Setting lastSeen in setup");
    } else {
      logFirebaseSuccess("lastSeen set in setup");
    }
  } else {
    logFirebaseError("Firebase or Wi-Fi not ready in setup");
  }
}

void handleFirebaseCommand() {
  // Check if there's a command, and only proceed if it's not empty or null
  if (Firebase.getString(fbdo, lockPath + "/command")) {
    String command = fbdo.stringData();

    // Only process if the command is valid (not empty and not 'null' from Firebase)
    if (command.length() > 0 && command != "null") {
      if (command == "lock") {
        Serial.write('L');
        logFirebaseSuccess("Received lock command");
      } else if (command == "unlock") {
        Serial.write('U');
        logFirebaseSuccess("Received unlock command");
      } else {
        logFirebaseError("Unrecognized command from Firebase: " + command);
      }

      // Acknowledge the command by setting it to an empty string or null
      // Setting to empty string is often safer with getString()
      if (!Firebase.setString(fbdo, lockPath + "/command", "")) {
        logFirebaseError("Failed to clear command after processing");
      }
    }
  } else {
    // This might fire if the path doesn't exist or is null, which is fine
    // if Firebase.getString returns false for an empty/null path
    // logFirebaseError("Reading command - fbdo.getString returned false"); // Keep for debugging if needed
  }
}

// ============================
// == FIREBASE COMMUNICATION ==
// ============================
// void handleFirebaseCommand() {
//   if (Firebase.getString(fbdo, lockPath + "/command")) {
//     String command = fbdo.stringData();

//     if (command == "lock") {
//       Serial.write('L');
//       logFirebaseSuccess("Received lock command");
//     } else if (command == "unlock") {
//       Serial.write('U');
//       logFirebaseSuccess("Received unlock command");
//     } else {
//       logFirebaseError("Unrecognized command from Firebase: " + command);
//     }
//   } else {
//     logFirebaseError("Reading command");
//   }
// }

void processWakePins() {
  bool bit2 = digitalRead(REG_MODE_WAKE_PIN);   // MSB
  bool bit1 = digitalRead(TAMPER_WAKE_PIN);
  bool bit0 = digitalRead(LOCK_STATUS_PIN);     // LSB

  int signal = (bit2 << 2) | (bit1 << 1) | bit0;

   switch (signal) {
    case 0b000:
      logFirebaseError("0,0,0 received – Idle state");
      break;

    case 0b001:
      Serial.println("Detected: LOCKED");
      if (!Firebase.setBool(fbdo, lockPath + "/status/isLocked", true)) {
        logFirebaseError("Setting isLocked = true from 3-bit input");
      } else {
        logFirebaseSuccess("isLocked = true from 3-bit input");
      }
      break;

    case 0b010:
      Serial.println("Detected: Tamper alert");
      if (!Firebase.setString(fbdo, lockPath + "/status/alert", "knock")) {
        logFirebaseError("Setting alert to tamper");
      } else {
        logFirebaseSuccess("Alert set to tamper");
      }
      delay(3000);
      Firebase.setString(fbdo, lockPath + "/status/alert", "none");
      break;

    case 0b011:
      Serial.println("Detected: UNLOCKED");
      if (!Firebase.setBool(fbdo, lockPath + "/status/isLocked", false)) {
        logFirebaseError("Setting isLocked = false from 3-bit input");
      } else {
        logFirebaseSuccess("isLocked = false from 3-bit input");
      }
      break;

    case 0b100:
      Serial.println("Detected: Registration mode");
      if (!Firebase.setString(fbdo, lockPath + "/status/mode", "registration")) {
        logFirebaseError("Setting mode to registration");
      } else {
        logFirebaseSuccess("Mode set to registration");
      }
      delay(60000);
      Firebase.setString(fbdo, lockPath + "/status/mode", "normal");
      break;

    case 0b111:
      {WiFiManager wifiManager;
      wifiManager.resetSettings();
      ESP.restart(); // Restart the ESP to force re-connection
      Serial.println("WIFI_DISCONNECTED");
      connectWiFi();
      break;}

    default:
      Serial.println("Detected: Unknown 3-bit signal: " + String(signal, BIN));
      logFirebaseError("Unrecognized 3-bit signal: " + String(signal, BIN));
      break;
  }
}


// ======================
// == ERROR HANDLING ====
// ======================
void safeSetBool(String path, bool value) {
  if (!Firebase.setBool(fbdo, path, value)) {
    logFirebaseError("safeSetBool → " + path);
  } else {
    logFirebaseSuccess("safeSetBool → " + path);
  }
}

void logFirebaseError(String context) {
  String errorMessage = "Context: " + context + " | Error: " + fbdo.errorReason();
  Serial.println(errorMessage);
  Firebase.setString(fbdo, lockPath + "/errorLog", errorMessage);
}

void logFirebaseSuccess(String context) {
  String successMessage = "Success: " + context;
  Serial.println(successMessage);
  Firebase.setString(fbdo, lockPath + "/successLog", successMessage);
} 