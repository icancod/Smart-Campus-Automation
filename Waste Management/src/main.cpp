
#include <Arduino.h>
#include "HX711.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>

// =====================================================
// CHRIST SMART CAMPUS - SMART WASTE MANAGEMENT
// =====================================================

// Campus identification
const char* CAMPUS_ID  = "CHRIST-DEMO";
const char* MODULE     = "waste";
const char* BUILDING_ID = "BLOCK-A";
const char* ZONE_ID     = "CANTEEN";
const char* DEVICE_ID   = "WASTE-A-ESP32-01";

// =====================================================
// WIFI / MQTT CONFIGURATION
// =====================================================
WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* MQTT_BROKER = "broker.emqx.io";
const int MQTT_PORT = 8883;

const char* TELEMETRY_TOPIC =
  "christ-smart-campus/v1/waste/BLOCK-A/WASTE-A-ESP32-01/telemetry";
const char* STATUS_TOPIC =
  "christ-smart-campus/v1/waste/BLOCK-A/WASTE-A-ESP32-01/status";

const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long STATUS_INTERVAL = 30000;
unsigned long lastMQTTAttempt = 0;
unsigned long lastStatusPublish = 0;

// =====================================================
// GPIO CONFIGURATION
// =====================================================

// Ultrasonic sensor
#define TRIG_PIN 5
#define ECHO_PIN 18

// HX711 load cell amplifier
#define HX711_DT  16
#define HX711_SCK 17

// Status LEDs
#define GREEN_LED  25
#define YELLOW_LED 26
#define RED_LED    27

// Buzzer
#define BUZZER_PIN 19

// =====================================================
// BIN CALIBRATION
// =====================================================

const float BIN_DEPTH_CM = 40.0;
const float FULL_DISTANCE_CM = 5.0;

const float NEARLY_FULL_THRESHOLD = 60.0;
const float FULL_THRESHOLD = 85.0;

// Weight configuration
// Replace with the actual rated capacity of the physical bin.
const float MAX_BIN_WEIGHT_KG = 20.0;

// Placeholder calibration factor.
// Must be calibrated using known weights on the physical setup.
const float CALIBRATION_FACTOR = 420.0;

// =====================================================
// TIMING CONFIGURATION
// =====================================================

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long TELEMETRY_INTERVAL = 5000;

const unsigned long ULTRASONIC_TIMEOUT = 30000;

const int INVALID_READING_LIMIT = 3;

// =====================================================
// SENSOR OBJECT
// =====================================================

HX711 scale;

// =====================================================
// SYSTEM VARIABLES
// =====================================================

float distanceCm = 0.0;
float fillPercentage = 0.0;
float wasteWeightKg = 0.0;

bool ultrasonicOnline = false;
bool loadCellOnline = false;
bool fillLevelValid = false;

int invalidReadingCount = 0;

unsigned long lastSensorRead = 0;
unsigned long lastTelemetry = 0;

enum BinStatus {
  NORMAL,
  NEARLY_FULL,
  FULL,
  SENSOR_ERROR,
  WEIGHT_LIMIT_EXCEEDED
};

BinStatus currentStatus = SENSOR_ERROR;

// =====================================================
// WIFI, TIME AND MQTT CONNECTION MANAGEMENT
// =====================================================
void connectWiFi() {
  Serial.println();
  Serial.print("Connecting to Wi-Fi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("Wi-Fi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("Synchronizing UTC time...");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 1700000000 && attempts < 20) {
    delay(500);
    now = time(nullptr);
    attempts++;
  }
  Serial.println(now >= 1700000000 ? "Time synchronized successfully." : "WARNING: Time synchronization failed.");
}

String getTimestamp() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char timestamp[25];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(timestamp);
}

bool connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqttClient.connected()) return true;
  Serial.println();
  Serial.println("--- MQTT TLS CONNECTION ---");
  Serial.println("Connecting to broker.emqx.io:8883...");
  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("MQTT TLS CONNECTION SUCCESSFUL!");
    Serial.print("Client ID: "); Serial.println(clientId);
    lastStatusPublish = 0;
    return true;
  }
  Serial.print("MQTT connection failed. State: ");
  Serial.println(mqttClient.state());
  return false;
}

void maintainMQTTConnection() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }
  if (millis() - lastMQTTAttempt >= MQTT_RECONNECT_INTERVAL) {
    lastMQTTAttempt = millis();
    connectMQTT();
  }
}

// =====================================================
// ULTRASONIC SENSOR
// =====================================================

float readDistance() {

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(
    ECHO_PIN,
    HIGH,
    ULTRASONIC_TIMEOUT
  );

  if (duration == 0) {
    return -1.0;
  }

  float distance = duration * 0.0343 / 2.0;

  if (distance < 2.0 || distance > 400.0) {
    return -1.0;
  }

  return distance;
}

// =====================================================
// FILL PERCENTAGE CALCULATION
// =====================================================

bool updateFillLevel() {

  fillLevelValid = false;
  float reading = readDistance();

  if (reading < 0) {

    invalidReadingCount++;

    if (invalidReadingCount >= INVALID_READING_LIMIT) {
      ultrasonicOnline = false;
    }

    return false;
  }

  invalidReadingCount = 0;
  ultrasonicOnline = true;

  distanceCm = reading;

  // Reject readings outside the calibrated bin range.
  if (distanceCm > BIN_DEPTH_CM + 5.0 ||
      distanceCm < FULL_DISTANCE_CM - 2.0) {

    return false;
  }

  fillPercentage =
    ((BIN_DEPTH_CM - distanceCm) /
     (BIN_DEPTH_CM - FULL_DISTANCE_CM)) * 100.0;

  fillPercentage = constrain(
    fillPercentage,
    0.0,
    100.0
  );

  fillLevelValid = true;
  return true;
}

// =====================================================
// LOAD CELL READING
// =====================================================

void updateWeight() {

  if (!scale.is_ready()) {
    loadCellOnline = false;
    return;
  }

  float weight = scale.get_units(5);

  if (!isfinite(weight)) {
    loadCellOnline = false;
    return;
  }

  // Ignore tiny negative offsets caused by calibration noise.
  if (weight < 0.0 && weight > -0.1) {
    weight = 0.0;
  }

  // A significantly negative reading indicates a calibration
  // or wiring problem. Do not report it as valid weight.
  if (weight < 0.0) {
    loadCellOnline = false;
    return;
  }

  wasteWeightKg = weight;
  loadCellOnline = true;
}

// =====================================================
// BIN STATUS EVALUATION
// =====================================================

const char* getStatusName(BinStatus status) {

  switch (status) {

    case NORMAL:
      return "normal";

    case NEARLY_FULL:
      return "nearly_full";

    case FULL:
      return "full";

    case SENSOR_ERROR:
      return "sensor_error";

    case WEIGHT_LIMIT_EXCEEDED:
      return "weight_limit_exceeded";

    default:
      return "unknown";
  }
}

void evaluateBinStatus(bool fillValid) {

  if (!ultrasonicOnline || !fillValid || !loadCellOnline) {
    currentStatus = SENSOR_ERROR;
    return;
  }

  if (wasteWeightKg >= MAX_BIN_WEIGHT_KG) {
    currentStatus = WEIGHT_LIMIT_EXCEEDED;
    return;
  }

  if (fillPercentage >= FULL_THRESHOLD) {
    currentStatus = FULL;
  }
  else if (fillPercentage >= NEARLY_FULL_THRESHOLD) {
    currentStatus = NEARLY_FULL;
  }
  else {
    currentStatus = NORMAL;
  }
}

// =====================================================
// LED AND BUZZER CONTROL
// =====================================================

void updateIndicators() {

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(RED_LED, LOW);

  switch (currentStatus) {

    case NORMAL:
      digitalWrite(GREEN_LED, HIGH);
      noTone(BUZZER_PIN);
      break;

    case NEARLY_FULL:
      digitalWrite(YELLOW_LED, HIGH);
      noTone(BUZZER_PIN);
      break;

    case FULL:
      digitalWrite(RED_LED, HIGH);
      tone(BUZZER_PIN, 2000);
      break;

    case WEIGHT_LIMIT_EXCEEDED:
      digitalWrite(RED_LED, HIGH);
      tone(BUZZER_PIN, 2500);
      break;

    case SENSOR_ERROR:
      digitalWrite(RED_LED, HIGH);
      noTone(BUZZER_PIN);
      break;
  }
}

// =====================================================
// SERIAL MONITOR OUTPUT
// =====================================================

void printSystemStatus() {

  Serial.println();
  Serial.println("======================================");
  Serial.println(" CHRIST SMART CAMPUS - WASTE MONITOR");
  Serial.println("======================================");

  Serial.print("Campus       : ");
  Serial.println(CAMPUS_ID);

  Serial.print("Building     : ");
  Serial.println(BUILDING_ID);

  Serial.print("Location     : ");
  Serial.println(ZONE_ID);

  Serial.print("Device ID    : ");
  Serial.println(DEVICE_ID);

  Serial.println("--------------------------------------");

  Serial.print("Ultrasonic   : ");
  Serial.println(ultrasonicOnline ? "ONLINE" : "OFFLINE");

  Serial.print("Load Cell    : ");
  Serial.println(loadCellOnline ? "ONLINE" : "OFFLINE");

  if (ultrasonicOnline) {
    Serial.print("Distance     : ");
    Serial.print(distanceCm);
    Serial.println(" cm");

    Serial.print("Fill Level   : ");
    Serial.print(fillPercentage);
    Serial.println(" %");
  }

  if (loadCellOnline) {
    Serial.print("Waste Weight : ");
    Serial.print(wasteWeightKg);
    Serial.println(" kg");

    Serial.print("Capacity Used: ");
    Serial.print(
      (wasteWeightKg / MAX_BIN_WEIGHT_KG) * 100.0
    );
    Serial.println(" %");
  }

  Serial.println("--------------------------------------");

  Serial.print("BIN STATUS   : ");
  Serial.println(getStatusName(currentStatus));

  Serial.println("======================================");
}

// =====================================================
// DASHBOARD TELEMETRY - ONE AGGREGATED JSON RECORD
// =====================================================
void publishWasteTelemetry() {
  String payload;
  payload.reserve(1200);
  payload = "{";
  payload += "\"message_type\":\"telemetry\",\"schema_version\":\"1.0\",";
  payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",";
  payload += "\"module\":\"" + String(MODULE) + "\",\"building_id\":\"" + String(BUILDING_ID) + "\",";
  payload += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  String timestamp = getTimestamp();
  payload += "\"timestamp\":";
  payload += timestamp.length() ? "\"" + timestamp + "\"" : "null";
  payload += ",\"timestamp_ms\":" + String(millis()) + ",\"metrics\":{";

  payload += "\"fill_percentage\":{\"value\":";
  payload += fillLevelValid ? String(fillPercentage, 2) : "null";
  payload += ",\"unit\":\"%\",\"status\":\"";
  payload += fillLevelValid ? "normal" : "sensor_unavailable";
  payload += "\"},";

  payload += "\"distance_to_waste\":{\"value\":";
  payload += fillLevelValid ? String(distanceCm, 2) : "null";
  payload += ",\"unit\":\"cm\",\"status\":\"";
  payload += fillLevelValid ? "normal" : "sensor_unavailable";
  payload += "\"},";

  payload += "\"waste_weight\":{\"value\":";
  payload += loadCellOnline ? String(wasteWeightKg, 3) : "null";
  payload += ",\"unit\":\"kg\",\"status\":\"";
  payload += loadCellOnline ? "normal" : "sensor_unavailable";
  payload += "\"},";

  payload += "\"weight_capacity_used\":{\"value\":";
  payload += loadCellOnline ? String((wasteWeightKg / MAX_BIN_WEIGHT_KG) * 100.0, 2) : "null";
  payload += ",\"unit\":\"%\",\"status\":\"";
  payload += loadCellOnline ? "normal" : "sensor_unavailable";
  payload += "\"},";

  payload += "\"collection_status\":{\"value\":\"" + String(getStatusName(currentStatus)) + "\",\"unit\":\"state\",\"status\":\"";
  payload += currentStatus == SENSOR_ERROR ? "sensor_unavailable" : "normal";
  payload += "\"},";

  payload += "\"collection_required\":{\"value\":";
  payload += ((fillLevelValid && fillPercentage >= 80.0) || currentStatus == WEIGHT_LIMIT_EXCEEDED) ? "true" : "false";
  payload += ",\"unit\":\"boolean\",\"status\":\"normal\"},";

  payload += "\"ultrasonic_sensor_online\":{\"value\":";
  payload += ultrasonicOnline ? "true" : "false";
  payload += ",\"unit\":\"boolean\",\"status\":\"normal\"},";

  payload += "\"load_cell_online\":{\"value\":";
  payload += loadCellOnline ? "true" : "false";
  payload += ",\"unit\":\"boolean\",\"status\":\"normal\"}";
  payload += "}}";
  Serial.println(payload);

  if (mqttClient.connected()) {
    if (mqttClient.publish(TELEMETRY_TOPIC, payload.c_str())) {
      Serial.println("Waste telemetry published to MQTT.");
    } else {
      Serial.println("Waste telemetry MQTT publish failed.");
    }
  } else {
    Serial.println("MQTT disconnected. Waste telemetry not published.");
  }
}

void publishDeviceStatus() {
  if (!mqttClient.connected()) return;
  String timestamp = getTimestamp();
  String payload = "{";
  payload += "\"message_type\":\"device_status\",\"schema_version\":\"1.0\",";
  payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",";
  payload += "\"module\":\"waste\",\"building_id\":\"" + String(BUILDING_ID) + "\",";
  payload += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"timestamp\":";
  payload += timestamp.length() ? "\"" + timestamp + "\"" : "null";
  payload += ",\"timestamp_ms\":" + String(millis()) + ",\"status\":\"online\"}";
  if (mqttClient.publish(STATUS_TOPIC, payload.c_str())) {
    Serial.println("Waste device online status published.");
  } else {
    Serial.println("Waste device status MQTT publish failed.");
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(TRIG_PIN, LOW);

  updateIndicators();

  Serial.println();
  Serial.println("Starting Smart Campus Waste System...");

  // Initialise HX711
  scale.begin(HX711_DT, HX711_SCK);

  Serial.println("Initialising load cell...");

  if (scale.wait_ready_timeout(3000)) {

    scale.set_scale(CALIBRATION_FACTOR);

    // Tare establishes the empty platform as zero.
    // Do not place waste on the bin during this operation.
    scale.tare();

    loadCellOnline = true;

    Serial.println("Load cell initialised and tared.");

  } else {

    loadCellOnline = false;

    Serial.println("WARNING: HX711 not detected.");
  }

  lastSensorRead = millis();
  lastTelemetry = millis();

  Serial.println("Waste monitoring system ready.");

  connectWiFi();
  syncTime();

  // Prototype only: certificate verification is disabled.
  // Configure a trusted CA certificate before production use.
  secureClient.setInsecure();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);
  mqttClient.setBufferSize(2048);
  connectMQTT();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  unsigned long currentTime = millis();

  maintainMQTTConnection();

  if (currentTime - lastSensorRead >= SENSOR_INTERVAL) {

    lastSensorRead = currentTime;

    bool fillValid = updateFillLevel();

    updateWeight();

    evaluateBinStatus(fillValid);

    updateIndicators();

    printSystemStatus();
  }

  if (currentTime - lastTelemetry >= TELEMETRY_INTERVAL) {

    lastTelemetry = currentTime;

    publishWasteTelemetry();
  }

  if (mqttClient.connected() &&
      currentTime - lastStatusPublish >= STATUS_INTERVAL) {
    lastStatusPublish = currentTime;
    publishDeviceStatus();
  }
}
