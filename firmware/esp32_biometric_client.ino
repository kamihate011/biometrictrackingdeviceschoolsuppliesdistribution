#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Fingerprint.h>

// ---- User config ----
const char* WIFI_SSID = "Nonexistent person";
const char* WIFI_PASSWORD = "kamidenzetsui";
const char* SERVER_URL = "http://172.17.185.77:3000/api/students/scan";

// Fingerprint sensor on UART2 (GPIO16 RX2, GPIO17 TX2)
HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fpSerial);
const int FP_RX_PIN_A = 16;
const int FP_TX_PIN_A = 17;
const int FP_RX_PIN_B = 17; // fallback if TX/RX wiring is swapped
const int FP_TX_PIN_B = 16; // fallback if TX/RX wiring is swapped

// OLED SSD1306 (I2C)
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

bool fingerprintReady = false;
uint32_t fingerprintBaud = 0;
int fingerprintRxPin = FP_RX_PIN_A;
int fingerprintTxPin = FP_TX_PIN_A;

unsigned long lastPostMs = 0;
const unsigned long postCooldownMs = 3000;
unsigned long lastSensorLogMs = 0;
const unsigned long sensorLogIntervalMs = 2500;
unsigned long lastWiFiRetryMs = 0;
const unsigned long wifiRetryIntervalMs = 10000;
const unsigned long wifiConnectTimeoutMs = 15000;
unsigned long lastFingerprintRetryMs = 0;
const unsigned long fingerprintRetryIntervalMs = 10000;
const unsigned long noFingerDelayMs = 70;
const unsigned long sensorErrorDelayMs = 120;

const uint32_t fingerprintBaudCandidates[] = {57600, 115200, 38400, 19200, 9600};
int pendingFingerprintId = -1;
bool fingerLatch = false;

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
  drawStatus("WiFi", "Connecting...");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < wifiConnectTimeoutMs) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected.");
    Serial.println(WiFi.localIP());
    drawStatus("WiFi", "Connected", WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi timeout.");
    drawStatus("WiFi", "Timeout", "Retrying...");
  }
}

bool postFingerprintMatch(int fingerprintId) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(4000);
  String payload = "{\"fingerprintId\":" + String(fingerprintId) + "}";
  int code = http.POST(payload);
  String responseBody = http.getString();
  http.end();
  Serial.print("POST status: ");
  Serial.println(code);
  Serial.print("POST body: ");
  Serial.println(responseBody);
  return code >= 200 && code < 300;
}

// Returns:
// >0 matched fingerprint ID
// 0 no finger on sensor
// -2 finger present but no enrolled template matched
// -1 sensor/protocol error
int scanFingerprint() {
  if (!fingerprintReady) return -1;

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

bool initFingerprintSensor() {
  const int pinPairs[][2] = {
    {FP_RX_PIN_A, FP_TX_PIN_A},
    {FP_RX_PIN_B, FP_TX_PIN_B}
  };

  // Some R307S modules need a longer boot stabilization time.
  delay(1200);

  for (size_t pairIndex = 0; pairIndex < (sizeof(pinPairs) / sizeof(pinPairs[0])); pairIndex++) {
    int rxPin = pinPairs[pairIndex][0];
    int txPin = pinPairs[pairIndex][1];

    for (size_t i = 0; i < (sizeof(fingerprintBaudCandidates) / sizeof(fingerprintBaudCandidates[0])); i++) {
      uint32_t baud = fingerprintBaudCandidates[i];
      fpSerial.end();
      fpSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
      delay(450);

      Serial.print("Trying fingerprint init: RX=");
      Serial.print(rxPin);
      Serial.print(" TX=");
      Serial.print(txPin);
      Serial.print(" baud=");
      Serial.println(baud);

      if (finger.verifyPassword()) {
        fingerprintReady = true;
        fingerprintBaud = baud;
        fingerprintRxPin = rxPin;
        fingerprintTxPin = txPin;
        Serial.print("Fingerprint sensor ready at baud ");
        Serial.println(fingerprintBaud);
        Serial.print("Active UART pins RX/TX: ");
        Serial.print(fingerprintRxPin);
        Serial.print("/");
        Serial.println(fingerprintTxPin);
        finger.getTemplateCount();
        Serial.print("Enrolled templates: ");
        Serial.println(finger.templateCount);
        drawStatus("Fingerprint", "Sensor OK", String(fingerprintBaud));
        return true;
      }
    }
  }

  fingerprintReady = false;
  fingerprintBaud = 0;
  Serial.println("Fingerprint sensor not found at tested baud rates.");
  drawStatus("Fingerprint", "Sensor not found");
  return false;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (1) {
      delay(10);
    }
  }

  initFingerprintSensor();

  connectWiFi();
  drawStatus("System Ready", "Place finger");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWiFiRetryMs >= wifiRetryIntervalMs) {
      lastWiFiRetryMs = now;
      connectWiFi();
    }
  }

  if (!fingerprintReady) {
    unsigned long now = millis();
    if (now - lastFingerprintRetryMs >= fingerprintRetryIntervalMs) {
      lastFingerprintRetryMs = now;
      initFingerprintSensor();
    }
    delay(100);
    return;
  }

  int scanResult = scanFingerprint();

  if (scanResult == 0) {
    // Allow next scan when finger is removed from sensor.
    fingerLatch = false;
    delay(noFingerDelayMs);
  } else if (scanResult > 0) {
    if (!fingerLatch) {
      pendingFingerprintId = scanResult;
      fingerLatch = true;
      Serial.print("Fingerprint matched locally. ID: ");
      Serial.println(scanResult);
      drawStatus("Matched", "Fingerprint ID:", String(scanResult));
    }
  } else if (scanResult == -2) {
    if (!fingerLatch) {
      fingerLatch = true;
      unsigned long now = millis();
      if (now - lastSensorLogMs > sensorLogIntervalMs) {
        lastSensorLogMs = now;
        Serial.println("Fingerprint present but no enrolled match found.");
      }
      drawStatus("No Template Match", "Try another finger");
    }
    delay(sensorErrorDelayMs);
  } else {
    unsigned long now = millis();
    if (now - lastSensorLogMs > sensorLogIntervalMs) {
      lastSensorLogMs = now;
      Serial.println("Fingerprint read error. Check wiring/sensor power.");
    }
    delay(sensorErrorDelayMs);
  }

  unsigned long nowMs = millis();
  if (pendingFingerprintId <= 0) {
    return;
  }

  if (nowMs - lastPostMs < postCooldownMs) {
    return;
  }

  bool ok = postFingerprintMatch(pendingFingerprintId);
  lastPostMs = nowMs;
  if (ok) {
    Serial.print("Match sent for ID: ");
    Serial.println(pendingFingerprintId);
    drawStatus("Match Sent", "Fingerprint ID:", String(pendingFingerprintId));
    pendingFingerprintId = -1;
  } else {
    Serial.print("Failed to send ID: ");
    Serial.println(pendingFingerprintId);
    drawStatus("Post Failed", "Fingerprint ID:", String(pendingFingerprintId));
  }
}
