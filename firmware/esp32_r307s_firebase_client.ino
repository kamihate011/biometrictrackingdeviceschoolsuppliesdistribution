#include <Arduino.h>
#include <WiFi.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Fingerprint.h>

// Firebase ESP Client dependencies
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ---- Wi-Fi ----
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ---- Firebase ----
#define API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define DATABASE_URL "https://your-project-id-default-rtdb.firebaseio.com/"

// If using email/password auth (recommended):
#define USER_EMAIL "device-uploader@yourdomain.com"
#define USER_PASSWORD "YOUR_STRONG_PASSWORD"

// ---- Fingerprint sensor UART ----
HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fpSerial);
const int FP_RX_PIN_A = 16; // ESP32 RX2 <- R307S TX
const int FP_TX_PIN_A = 17; // ESP32 TX2 -> R307S RX
const int FP_RX_PIN_B = 17; // fallback if TX/RX swapped
const int FP_TX_PIN_B = 16; // fallback if TX/RX swapped
const uint32_t fingerprintBaudCandidates[] = {57600, 115200, 38400, 19200, 9600};
const int FP_RX_PIN = 16; // ESP32 RX2 <- R307S TX
const int FP_TX_PIN = 17; // ESP32 TX2 -> R307S RX

// ---- OLED + Buzzer ----
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
const int OLED_SDA_PIN = 21;
const int OLED_SCL_PIN = 22;
const int BUZZER_PIN = 25;
const bool BUZZER_ENABLED = false;    // safety default: set true only after verified buzzer wiring
const bool BUZZER_ACTIVE_HIGH = true; // set false for active-low buzzer modules
const int FP_POWER_PIN = -1;          // optional transistor/MOSFET control pin for R307S VCC (-1 = direct power)
const int BUZZER_CHANNEL = 0;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

bool oledReady = false;
bool firebaseReady = false;
bool fingerprintReady = false;
bool fingerLatch = false;
int pendingFingerprintId = -1;

int activeFpRxPin = FP_RX_PIN_A;
int activeFpTxPin = FP_TX_PIN_A;
uint32_t activeFpBaud = 57600;

unsigned long lastUploadMs = 0;
unsigned long lastFingerprintRetryMs = 0;
unsigned long lastWiFiRetryMs = 0;
unsigned long lastNoMatchMs = 0;
unsigned long lastSensorErrorMs = 0;
const unsigned long uploadCooldownMs = 3000;
const unsigned long fingerprintRetryIntervalMs = 10000;
const unsigned long wifiRetryIntervalMs = 10000;
const unsigned long noFingerDelayMs = 70;
const unsigned long sensorErrorDelayMs = 120;
const unsigned long statusLogIntervalMs = 2500;

void drawStatus(const String& title, const String& line1 = "", const String& line2 = "") {
  if (!oledReady) return;

bool firebaseReady = false;
bool fingerLatched = false;
unsigned long lastUploadMs = 0;
const unsigned long uploadCooldownMs = 3000;

void beep(int frequency, int durationMs, int pauseMs = 0) {
  ledcWriteTone(BUZZER_CHANNEL, frequency);
  delay(durationMs);
  ledcWriteTone(BUZZER_CHANNEL, 0);
  if (pauseMs > 0) delay(pauseMs);
}

void beepSuccess() {
  beep(1800, 80, 40);
  beep(2400, 120);
}

void beepError() {
  beep(500, 200, 40);
  beep(400, 250);
}

void beepNoMatch() {
  beep(700, 130);
}

void drawStatus(const String& title, const String& line1 = "", const String& line2 = "") {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println(title);
  oled.println("----------------");
  oled.println(line1);
  oled.println(line2);
  oled.display();
}

void buzzerOn() {
  if (!BUZZER_ENABLED) return;
  digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_HIGH ? HIGH : LOW);
}

void buzzerOff() {
  if (!BUZZER_ENABLED) return;
  digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_HIGH ? LOW : HIGH);
}

void buzz(unsigned long onMs, unsigned long offMs = 0) {
  if (BUZZER_ENABLED) {
    buzzerOn();
    delay(onMs);
    buzzerOff();
  } else {
    delay(onMs);
  }
  if (offMs > 0) delay(offMs);
}

void beepSuccess() {
  buzz(80, 40);
  buzz(120, 0);
}

void beepError() {
  buzz(220, 60);
  buzz(220, 0);
}

void beepNoMatch() {
  buzz(130, 0);
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  drawStatus("Wi-Fi", "Connecting...");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(400);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    drawStatus("Wi-Fi", "Connected", WiFi.localIP().toString());
    buzz(80);
  } else {
    Serial.println("\nWi-Fi timeout");
    drawStatus("Wi-Fi", "Timeout", "Retrying...");
  }
}

void sensorPowerOn() {
  if (FP_POWER_PIN >= 0) {
    pinMode(FP_POWER_PIN, OUTPUT);
    digitalWrite(FP_POWER_PIN, HIGH);
    delay(1200);
  }
}

void sensorPowerOff() {
  if (FP_POWER_PIN >= 0) {
    pinMode(FP_POWER_PIN, OUTPUT);
    digitalWrite(FP_POWER_PIN, LOW);
  }
}

bool initFingerprintSensor() {
  delay(1200); // allow R307S startup before probing UART

  const int pinPairs[][2] = {
    {FP_RX_PIN_A, FP_TX_PIN_A},
    {FP_RX_PIN_B, FP_TX_PIN_B}
  };


  for (size_t pairIndex = 0; pairIndex < (sizeof(pinPairs) / sizeof(pinPairs[0])); pairIndex++) {
    int rxPin = pinPairs[pairIndex][0];
    int txPin = pinPairs[pairIndex][1];

    for (size_t i = 0; i < (sizeof(fingerprintBaudCandidates) / sizeof(fingerprintBaudCandidates[0])); i++) {
      uint32_t baud = fingerprintBaudCandidates[i];
      fpSerial.end();
      fpSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
      delay(450);

      Serial.print("Trying fingerprint init RX=");
      Serial.print(rxPin);
      Serial.print(" TX=");
      Serial.print(txPin);
      Serial.print(" baud=");
      Serial.println(baud);

      if (finger.verifyPassword()) {
        activeFpRxPin = rxPin;
        activeFpTxPin = txPin;
        activeFpBaud = baud;
        fingerprintReady = true;

        finger.getTemplateCount();
        Serial.print("Fingerprint ready. Templates: ");
        Serial.println(finger.templateCount);
        Serial.print("Active RX/TX: ");
        Serial.print(activeFpRxPin);
        Serial.print("/");
        Serial.print(activeFpTxPin);
        Serial.print(" baud ");
        Serial.println(activeFpBaud);

        drawStatus("Fingerprint", "Ready", "Tpl: " + String(finger.templateCount));
        buzz(80);
        return true;
      }
    }
  }

  fingerprintReady = false;
  Serial.println("Fingerprint sensor not found.");
  drawStatus("Fingerprint", "Not found", "Check wiring");
  beepError();
  return false;
  Serial.println("\nWi-Fi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  drawStatus("Wi-Fi", "Connected", WiFi.localIP().toString());
  beep(1500, 80);
}

bool initFingerprintSensor() {
  fpSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  delay(400);

  if (!finger.verifyPassword()) {
    Serial.println("Fingerprint sensor not detected. Check power/wiring.");
    drawStatus("Fingerprint", "Sensor not found");
    beepError();
    return false;
  }

  finger.getTemplateCount();
  Serial.print("Fingerprint sensor ready. Enrolled templates: ");
  Serial.println(finger.templateCount);
  drawStatus("Fingerprint", "Ready", "Templates: " + String(finger.templateCount));
  beep(1400, 80);
  return true;
}

void initFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(2048);
  config.timeout.serverResponse = 10 * 1000;

  Firebase.begin(&config, &auth);
  Serial.println("Waiting for Firebase auth...");
  drawStatus("Firebase", "Authenticating...");


  Serial.println("Waiting for Firebase auth...");
  drawStatus("Firebase", "Authenticating...");
  unsigned long started = millis();
  while (!Firebase.ready() && millis() - started < 15000) {
    delay(200);
  }

  firebaseReady = Firebase.ready();
  if (firebaseReady) {
    Serial.println("Firebase ready");
    drawStatus("Firebase", "Ready");
    buzz(80);
  } else {
    Serial.println("Firebase init timeout");
  Serial.println(firebaseReady ? "Firebase ready" : "Firebase init timeout");
  if (firebaseReady) {
    drawStatus("Firebase", "Ready");
    beep(1600, 80);
  } else {
    drawStatus("Firebase", "Init timeout", "Check credentials");
    beepError();
  }
}

// Returns: >0 matched ID, 0 no finger, -2 finger present no match, -1 sensor error
int scanFingerprint() {
  if (!fingerprintReady) return -1;

int scanFingerprint() {
  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return 0;
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerSearch();
  if (p == FINGERPRINT_NOTFOUND) return -2;
  if (p != FINGERPRINT_OK) return -1;

  return finger.fingerID;
}

bool uploadLog(int userId) {
  if (!firebaseReady || WiFi.status() != WL_CONNECTED) return false;

  FirebaseJson json;
  json.set("user_id", userId);
  json.set("timestamp/.sv", "timestamp");

  // Push to /logs and let Firebase create a unique key
  if (!Firebase.RTDB.pushJSON(&fbdo, "/logs", &json)) {
    Serial.print("RTDB push failed: ");
    Serial.println(fbdo.errorReason());
    return false;
  }

  Serial.print("Upload success. New log key: ");
  Serial.println(fbdo.pushName());
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUZZER_PIN, OUTPUT);
  if (BUZZER_ENABLED) buzzerOff();

  sensorPowerOn();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  oledReady = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledReady) {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, 0);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
  } else {
    drawStatus("Boot", "Initializing...");
  }

  connectWiFi();
  initFingerprintSensor();

  if (!initFingerprintSensor()) {
    // Keep running to allow hot-fix/retry while powered
    Serial.println("Fingerprint init failed; device will keep retrying scans.");
  }

  initFirebase();
  drawStatus("System Ready", "Place finger");
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED && now - lastWiFiRetryMs >= wifiRetryIntervalMs) {
    lastWiFiRetryMs = now;
    connectWiFi();
  }

  if (!firebaseReady || !Firebase.ready()) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!Firebase.ready()) {
    firebaseReady = false;
    initFirebase();
  }

  if (!fingerprintReady) {
    if (now - lastFingerprintRetryMs >= fingerprintRetryIntervalMs) {
      lastFingerprintRetryMs = now;
      sensorPowerOff();
      delay(300);
      sensorPowerOn();
      initFingerprintSensor();
    }
    delay(100);
    return;
  }

  int result = scanFingerprint();

  if (result == 0) {
    fingerLatch = false;
    delay(noFingerDelayMs);
  } else if (result > 0) {
    if (!fingerLatch) {
      pendingFingerprintId = result;
      fingerLatch = true;
      Serial.print("Match found. user_id=");
      Serial.println(result);
      drawStatus("Match Found", "user_id: " + String(result), "Queue upload");
    }
  } else if (result == -2) {
    if (!fingerLatch) {
      fingerLatch = true;
      if (now - lastNoMatchMs >= statusLogIntervalMs) {
        lastNoMatchMs = now;
        Serial.println("Finger detected but no enrolled template match.");
      }
      drawStatus("No Match", "Try another finger");
      beepNoMatch();
    }
    delay(sensorErrorDelayMs);
  } else {
    if (now - lastSensorErrorMs >= statusLogIntervalMs) {
      lastSensorErrorMs = now;
      Serial.println("Fingerprint read error. Check wiring/sensor power.");
    }
    delay(sensorErrorDelayMs);
  }

  if (pendingFingerprintId <= 0) return;
  if (millis() - lastUploadMs < uploadCooldownMs) return;

  bool ok = uploadLog(pendingFingerprintId);
  lastUploadMs = millis();

  if (ok) {
    Serial.print("Upload complete for user_id=");
    Serial.println(pendingFingerprintId);
    drawStatus("Upload Success", "user_id: " + String(pendingFingerprintId));
    beepSuccess();
    pendingFingerprintId = -1;
  } else {
    Serial.print("Upload failed for user_id=");
    Serial.println(pendingFingerprintId);
    drawStatus("Upload Failed", "user_id: " + String(pendingFingerprintId));
  int result = scanFingerprint();

  if (result == 0) {
    fingerLatched = false;
    delay(60);
    return;
  }

  if (result > 0) {
    if (!fingerLatched && millis() - lastUploadMs > uploadCooldownMs) {
      fingerLatched = true;
      lastUploadMs = millis();
      Serial.print("Match found. user_id=");
      Serial.println(result);
      drawStatus("Match Found", "user_id: " + String(result), "Uploading...");

      if (uploadLog(result)) {
        Serial.println("Log uploaded to /logs");
        drawStatus("Upload Success", "user_id: " + String(result));
        beepSuccess();
      } else {
        Serial.println("Upload failed; will retry on next scan.");
        drawStatus("Upload Failed", "user_id: " + String(result));
        beepError();
      }
    }
  } else if (result == -2) {
    if (!fingerLatched) {
      fingerLatched = true;
      Serial.println("Finger detected but no enrolled template match.");
      drawStatus("No Match", "Try another finger");
      beepNoMatch();
    }
  } else {
    Serial.println("Fingerprint read error.");
    drawStatus("Sensor Error", "Check wiring");
    beepError();
  }

  delay(120);
}
