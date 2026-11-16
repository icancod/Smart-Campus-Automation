
#include <Arduino.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>

// =====================================================
// CHRIST SMART CAMPUS - CAMPUS SAFETY MODULE
// =====================================================

// Campus identification
const char* CAMPUS_ID = "CHRIST-DEMO";
const char* MODULE = "safety";
const char* BUILDING_ID = "BLOCK-A";
const char* ZONE_ID = "SAFETY-ZONE-A";
const char* DEVICE_ID = "SAFETY-A-ESP32-01";

// =====================================================
// GPIO CONFIGURATION
// =====================================================

#define DHT_PIN 15
#define DHT_TYPE DHT22

#define EMERGENCY_BUTTON 32
#define RESET_BUTTON 33

#define GREEN_LED 25
#define YELLOW_LED 26
#define RED_LED 27

#define BUZZER_PIN 19

// =====================================================
// TEMPERATURE THRESHOLDS
// Demonstration values only - NOT fire safety limits.
// =====================================================

const float WARNING_TEMP_C = 35.0;
const float EMERGENCY_TEMP_C = 50.0;

// =====================================================
// TIMING
// =====================================================

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long TELEMETRY_INTERVAL = 5000;
const unsigned long BUTTON_DEBOUNCE = 250;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long STATUS_INTERVAL = 30000;

// Prototype Wi-Fi and MQTT configuration
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* MQTT_BROKER = "broker.emqx.io";
const int MQTT_PORT = 8883;
const char* TELEMETRY_TOPIC = "christ-smart-campus/v1/safety/BLOCK-A/SAFETY-A-ESP32-01/telemetry";
const char* STATUS_TOPIC = "christ-smart-campus/v1/safety/BLOCK-A/SAFETY-A-ESP32-01/status";
const char* ALERT_TOPIC = "christ-smart-campus/v1/safety/BLOCK-A/SAFETY-A-ESP32-01/alerts";

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);
unsigned long lastMQTTReconnect = 0;
unsigned long lastStatusPublish = 0;

// =====================================================
// SENSOR
// =====================================================

DHT dht(DHT_PIN, DHT_TYPE);

// =====================================================
// SYSTEM STATE
// =====================================================

float temperatureC = 0.0;
float humidity = 0.0;

bool temperatureValid = false;
bool warningActive = false;

bool emergencyActive = false;
bool emergencyButtonWasPressed = false;
bool resetButtonWasPressed = false;

unsigned long lastSensorRead = 0;
unsigned long lastTelemetry = 0;
unsigned long lastEmergencyPress = 0;
unsigned long lastResetPress = 0;

unsigned long eventCounter = 0;

String activeAlertType = "";
String activeSeverity = "";

bool temperatureEmergencyLatched = false;
bool buzzerIsOn = false;

// =====================================================
// WIFI, MQTT AND TIMESTAMP HELPERS
// =====================================================
String getTimestamp() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 1000)) return "";
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
  return String(buffer);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connected!");
}

bool connectMQTT() {
  if (mqttClient.connected()) return true;
  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to MQTT...");
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println(" MQTT TLS CONNECTION SUCCESSFUL!");
    return true;
  }
  Serial.print(" MQTT connection failed, state=");
  Serial.println(mqttClient.state());
  return false;
}

void maintainMQTTConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }
  if (!mqttClient.connected() && millis() - lastMQTTReconnect >= MQTT_RECONNECT_INTERVAL) {
    lastMQTTReconnect = millis();
    connectMQTT();
  }
  mqttClient.loop();
}

void publishDeviceStatus() {
  if (!mqttClient.connected()) return;
  String payload = "{";
  payload += "\"message_type\":\"status\",\"schema_version\":\"1.0\",";
  payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",\"module\":\"safety\",";
  payload += "\"building_id\":\"" + String(BUILDING_ID) + "\",\"zone_id\":\"" + String(ZONE_ID) + "\",";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",\"timestamp\":\"" + getTimestamp() + "\",";
  payload += "\"online\":true,\"emergency_active\":" + String(emergencyActive ? "true" : "false") + "}";
  if (mqttClient.publish(STATUS_TOPIC, payload.c_str())) Serial.println("Safety device online status published.");
  else Serial.println("Safety device status MQTT publish failed.");
}

// =====================================================
// AGGREGATED DASHBOARD TELEMETRY
// =====================================================
void publishTelemetry() {
  String payload;
  payload.reserve(1100);
  payload = "{";
  payload += "\"message_type\":\"telemetry\",\"schema_version\":\"1.0\",";
  payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",";
  payload += "\"module\":\"" + String(MODULE) + "\",\"building_id\":\"" + String(BUILDING_ID) + "\",";
  payload += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"timestamp\":\"" + getTimestamp() + "\",\"timestamp_ms\":" + String(millis()) + ",\"metrics\":{";
  payload += "\"temperature\":{\"value\":";
  payload += temperatureValid ? String(temperatureC, 2) : "null";
  payload += ",\"unit\":\"C\",\"status\":\"";
  payload += temperatureValid ? (warningActive ? "warning" : "normal") : "sensor_unavailable";
  payload += "\"},";
  payload += "\"humidity\":{\"value\":";
  payload += temperatureValid ? String(humidity, 2) : "null";
  payload += ",\"unit\":\"%\",\"status\":\"";
  payload += temperatureValid ? "normal" : "sensor_unavailable";
  payload += "\"},";
  payload += "\"warning_active\":{\"value\":";
  payload += warningActive ? "true" : "false";
  payload += ",\"unit\":\"boolean\",\"status\":\"normal\"},";
  payload += "\"emergency_active\":{\"value\":";
  payload += emergencyActive ? "true" : "false";
  payload += ",\"unit\":\"boolean\",\"status\":\"normal\"},";
  payload += "\"active_alert_type\":{\"value\":\"" + activeAlertType + "\",\"unit\":\"state\",\"status\":\"normal\"},";
  payload += "\"active_severity\":{\"value\":\"" + activeSeverity + "\",\"unit\":\"state\",\"status\":\"normal\"}";
  payload += "}}";
  Serial.println(payload);
  if (mqttClient.connected()) {
    if (mqttClient.publish(TELEMETRY_TOPIC, payload.c_str())) Serial.println("Safety telemetry published to MQTT.");
    else Serial.println("Safety telemetry MQTT publish failed.");
  } else {
    Serial.println("MQTT disconnected; safety telemetry not published.");
  }
}

// =====================================================
// ALERT EVENT OUTPUT
// =====================================================

void publishAlert(
  const char* alertType,
  const char* severity,
  const char* event
) {
  eventCounter++;
  String payload = "{";
  payload += "\"message_type\":\"alert\",\"schema_version\":\"1.0\",";
  payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",\"module\":\"safety\",";
  payload += "\"building_id\":\"" + String(BUILDING_ID) + "\",\"zone_id\":\"" + String(ZONE_ID) + "\",";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",\"timestamp\":\"" + getTimestamp() + "\",";
  payload += "\"timestamp_ms\":" + String(millis()) + ",";
  payload += "\"alert_type\":\"" + String(alertType) + "\",\"severity\":\"" + String(severity) + "\",";
  payload += "\"event\":\"" + String(event) + "\",\"event_id\":\"" + String(DEVICE_ID) + "-" + String(eventCounter) + "\",";
  payload += "\"temperature_c\":";
  payload += temperatureValid ? String(temperatureC, 2) : "null";
  payload += ",\"alert_status\":\"" + String(emergencyActive ? "active" : "resolved") + "\"}";
  Serial.println(payload);
  if (mqttClient.connected()) {
    if (mqttClient.publish(ALERT_TOPIC, payload.c_str())) Serial.println("Safety alert event published to MQTT.");
    else Serial.println("Safety alert MQTT publish failed.");
  } else {
    Serial.println("MQTT disconnected; safety alert was not published.");
  }
}

// =====================================================
// EMERGENCY ACTIVATION
// =====================================================

void activateEmergency(
  const char* alertType,
  const char* severity
) {
  // Avoid creating duplicate events while one is active.
  if (emergencyActive) {
    Serial.println("An emergency alert is already active.");
    return;
  }

  emergencyActive = true;

  activeAlertType = alertType;
  activeSeverity = severity;

  if (String(alertType) == "high_temperature") {
    temperatureEmergencyLatched = true;
  }

  Serial.println("EMERGENCY ACTIVATED!");

  publishAlert(
    alertType,
    severity,
    "triggered"
  );
}

// =====================================================
// RESET EMERGENCY
// =====================================================

void resetEmergency() {

  if (!emergencyActive) {
    Serial.println("No active emergency to reset.");
    return;
  }

  String previousAlertType = activeAlertType;

  emergencyActive = false;
  temperatureEmergencyLatched = false;

  activeAlertType = "";
  activeSeverity = "";


  Serial.println("Emergency alert reset.");

  publishAlert(
    previousAlertType.c_str(),
    "resolved",
    "reset"
  );
}

// =====================================================
// TEMPERATURE MONITORING
// =====================================================

void readTemperature() {

  float newTemperature = dht.readTemperature();
  float newHumidity = dht.readHumidity();

  if (isnan(newTemperature) || isnan(newHumidity)) {

    temperatureValid = false;
    warningActive = false;

    Serial.println("DHT22 SENSOR ERROR");

    return;
  }

  temperatureC = newTemperature;
  humidity = newHumidity;

  temperatureValid = true;

  warningActive =
    temperatureC >= WARNING_TEMP_C;

  // Temperature emergency is latched until reset.
  if (temperatureC >= EMERGENCY_TEMP_C &&
      !temperatureEmergencyLatched &&
      !emergencyActive) {

    activateEmergency(
      "high_temperature",
      "critical"
    );
  }

  Serial.print("Temperature: ");
  Serial.print(temperatureC);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");
}

// =====================================================
// BUTTON PROCESSING
// =====================================================

void processButtons() {

  unsigned long currentTime = millis();

  bool emergencyPressed =
    digitalRead(EMERGENCY_BUTTON) == LOW;

  bool resetPressed =
    digitalRead(RESET_BUTTON) == LOW;

  // Emergency button - detect press edge.
  if (emergencyPressed &&
      !emergencyButtonWasPressed &&
      currentTime - lastEmergencyPress >= BUTTON_DEBOUNCE) {

    lastEmergencyPress = currentTime;

    activateEmergency(
      "manual_emergency",
      "critical"
    );
  }

  emergencyButtonWasPressed = emergencyPressed;

  // Reset button - detect press edge.
  if (resetPressed &&
      !resetButtonWasPressed &&
      currentTime - lastResetPress >= BUTTON_DEBOUNCE) {

    lastResetPress = currentTime;

    resetEmergency();
  }

  resetButtonWasPressed = resetPressed;
}

// =====================================================
// LED AND BUZZER CONTROL
// =====================================================

void updateIndicators() {

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(RED_LED, LOW);

  if (emergencyActive) {

    digitalWrite(RED_LED, HIGH);

    if (!buzzerIsOn) {
      tone(BUZZER_PIN, 2000);
      buzzerIsOn = true;
    }

  } else {

    if (buzzerIsOn) {
      noTone(BUZZER_PIN);
      buzzerIsOn = false;
    }

    if (!temperatureValid) {

      digitalWrite(YELLOW_LED, HIGH);

    } else if (warningActive) {

      digitalWrite(YELLOW_LED, HIGH);

    } else {

      digitalWrite(GREEN_LED, HIGH);
    }
  }
}

// =====================================================
// SERIAL SYSTEM STATUS
// =====================================================

void printSystemStatus() {

  Serial.println();
  Serial.println("====================================");
  Serial.println(" CHRIST SMART CAMPUS - SAFETY NODE");
  Serial.println("====================================");

  Serial.print("Campus      : ");
  Serial.println(CAMPUS_ID);

  Serial.print("Building    : ");
  Serial.println(BUILDING_ID);

  Serial.print("Zone        : ");
  Serial.println(ZONE_ID);

  Serial.print("Device ID   : ");
  Serial.println(DEVICE_ID);

  Serial.println("------------------------------------");

  Serial.print("Temperature : ");

  if (temperatureValid) {
    Serial.print(temperatureC);
    Serial.println(" C");
  } else {
    Serial.println("INVALID");
  }

  Serial.print("Humidity    : ");

  if (temperatureValid) {
    Serial.print(humidity);
    Serial.println(" %");
  } else {
    Serial.println("INVALID");
  }

  Serial.print("Warning     : ");
  Serial.println(warningActive ? "ACTIVE" : "INACTIVE");

  Serial.print("Emergency   : ");
  Serial.println(emergencyActive ? "ACTIVE" : "INACTIVE");

  if (emergencyActive) {
    Serial.print("Alert Type  : ");
    Serial.println(activeAlertType);

    Serial.print("Severity    : ");
    Serial.println(activeSeverity);
  }

  Serial.println("====================================");
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  connectWiFi();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("Synchronising system time...");
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 10000)) Serial.println("Time synchronized successfully.");
  else Serial.println("NTP time not available yet; timestamps may be empty.");
  secureClient.setInsecure(); // Prototype only: use a trusted CA certificate in production.
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);
  mqttClient.setBufferSize(2048);
  connectMQTT();

  pinMode(EMERGENCY_BUTTON, INPUT_PULLUP);
  pinMode(RESET_BUTTON, INPUT_PULLUP);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  dht.begin();

  Serial.println();
  Serial.println("Starting Campus Safety Monitoring...");

  delay(2000);

  readTemperature();

  updateIndicators();

  lastSensorRead = millis();
  lastTelemetry = millis();
  lastStatusPublish = millis();

  Serial.println("Safety monitoring system ready.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  maintainMQTTConnection();
  unsigned long currentTime = millis();

  processButtons();

  if (currentTime - lastSensorRead >= SENSOR_INTERVAL) {

    lastSensorRead = currentTime;

    readTemperature();

    updateIndicators();

    printSystemStatus();
  }

  if (currentTime - lastTelemetry >= TELEMETRY_INTERVAL) {

    lastTelemetry = currentTime;

    publishTelemetry();
  }

  updateIndicators();

  if (currentTime - lastStatusPublish >= STATUS_INTERVAL) {
    lastStatusPublish = currentTime;
    publishDeviceStatus();
  }
}
