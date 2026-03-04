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
const int FP_RX_PIN = 16; // ESP32 RX2 <- R307S TX
const int FP_TX_PIN = 17; // ESP32 TX2 -> R307S RX

// ---- OLED + Buzzer ----
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
const int OLED_SDA_PIN = 21;
const int OLED_SCL_PIN = 22;
const int BUZZER_PIN = 25;
const int BUZZER_CHANNEL = 0;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

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

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  drawStatus("Wi-Fi", "Connecting...");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(400);
  }

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
  unsigned long started = millis();
  while (!Firebase.ready() && millis() - started < 15000) {
    delay(200);
  }

  firebaseReady = Firebase.ready();
  Serial.println(firebaseReady ? "Firebase ready" : "Firebase init timeout");
  if (firebaseReady) {
    drawStatus("Firebase", "Ready");
    beep(1600, 80);
  } else {
    drawStatus("Firebase", "Init timeout", "Check credentials");
    beepError();
  }
}

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

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, 0);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
  } else {
    drawStatus("Boot", "Initializing...");
  }

  connectWiFi();

  if (!initFingerprintSensor()) {
    // Keep running to allow hot-fix/retry while powered
    Serial.println("Fingerprint init failed; device will keep retrying scans.");
  }

  initFirebase();
  drawStatus("System Ready", "Place finger");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!Firebase.ready()) {
    firebaseReady = false;
    initFirebase();
  }

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
