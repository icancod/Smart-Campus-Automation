
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>

// ============================================================
// CHRIST SMART CAMPUS - SMART ENERGY MODULE
// SIMULATED ENERGY MONITORING SYSTEM
// ============================================================

// ---------------- WI-FI AND MQTT CONFIGURATION ----------------

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_BROKER = "broker.emqx.io";
const int MQTT_PORT = 8883;

const char* TELEMETRY_TOPIC =
    "christ-smart-campus/v1/energy/BLOCK-A/ENERGY-A-ESP32-01/telemetry";
const char* STATUS_TOPIC =
    "christ-smart-campus/v1/energy/BLOCK-A/ENERGY-A-ESP32-01/status";

const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long STATUS_INTERVAL = 30000;

unsigned long lastMQTTAttempt = 0;
unsigned long lastStatusPublish = 0;

// ---------------- DEVICE INFORMATION ----------------

const char* DEVICE_ID = "ENERGY-A-ESP32-01";
const char* CAMPUS_ID = "CHRIST-DEMO";
const char* BUILDING_ID = "BLOCK-A";
const char* ZONE_ID = "ENERGY-LAB-A";

// ---------------- GPIO CONFIGURATION ----------------

const int POT_PIN = 34;

const int GREEN_LED = 25;
const int YELLOW_LED = 26;
const int RED_LED = 27;

// ---------------- SIMULATION PARAMETERS ----------------

// These are simulated electrical values.
// The potentiometer is NOT a real current sensor.

const float SIMULATED_VOLTAGE = 12.0;
const float MAX_CURRENT = 2.0;

const int ADC_MAX_VALUE = 4095;

// ---------------- POWER THRESHOLDS ----------------

// 0 W to below 12 W: NORMAL
// 12 W to 18 W: MODERATE
// Above 18 W: HIGH

const float MODERATE_THRESHOLD_W = 12.0;
const float HIGH_THRESHOLD_W = 18.0;

// ---------------- TIMING ----------------

const unsigned long READING_INTERVAL = 1000;
const unsigned long BLINK_INTERVAL = 300;

// ---------------- ENERGY VARIABLES ----------------

float voltage = SIMULATED_VOLTAGE;
float current = 0.0;
float power = 0.0;
float energyKWh = 0.0;

float previousPower = 0.0;

unsigned long lastReadingTime = 0;
unsigned long lastBlinkTime = 0;

bool redLedState = false;

// ---------------- LOAD STATUS ----------------

enum LoadStatus {
    NO_LOAD,
    NORMAL_LOAD,
    MODERATE_LOAD,
    HIGH_LOAD
};

LoadStatus loadStatus = NO_LOAD;

// ============================================================
// READ SIMULATED CURRENT
// ============================================================

float readSimulatedCurrent() {

    int adcValue = analogRead(POT_PIN);

    // Convert ADC reading to simulated current (0-2 A).
    float simulatedCurrent =
        (adcValue / (float)ADC_MAX_VALUE) * MAX_CURRENT;

    return simulatedCurrent;
}

// ============================================================
// CALCULATE POWER
// ============================================================

float calculatePower(float v, float i) {
    return v * i;
}

// ============================================================
// ACCUMULATE ENERGY
// ============================================================

void calculateEnergy(unsigned long elapsedMs) {

    // Trapezoidal integration:
    // average power (W) * elapsed hours = Wh
    // Wh / 1000 = kWh

    float averagePower = (previousPower + power) / 2.0;

    float elapsedHours = elapsedMs / 3600000.0;

    energyKWh += (averagePower * elapsedHours) / 1000.0;

    previousPower = power;
}

// ============================================================
// DETERMINE LOAD STATUS
// ============================================================

LoadStatus determineLoadStatus() {

    if (current < 0.05) {
        return NO_LOAD;
    }

    if (power < MODERATE_THRESHOLD_W) {
        return NORMAL_LOAD;
    }

    if (power <= HIGH_THRESHOLD_W) {
        return MODERATE_LOAD;
    }

    return HIGH_LOAD;
}

// ============================================================
// LOAD STATUS AS TEXT
// ============================================================

const char* getLoadStatusName(LoadStatus status) {

    switch (status) {

        case NO_LOAD:
            return "NO LOAD";

        case NORMAL_LOAD:
            return "NORMAL";

        case MODERATE_LOAD:
            return "MODERATE";

        case HIGH_LOAD:
            return "HIGH CONSUMPTION";

        default:
            return "UNKNOWN";
    }
}

// ============================================================
// LED CONTROL
// ============================================================

void updateLEDs(unsigned long currentTime) {

    switch (loadStatus) {

        case NO_LOAD:

            digitalWrite(GREEN_LED, LOW);
            digitalWrite(YELLOW_LED, LOW);
            digitalWrite(RED_LED, LOW);

            redLedState = false;
            break;

        case NORMAL_LOAD:

            digitalWrite(GREEN_LED, HIGH);
            digitalWrite(YELLOW_LED, LOW);
            digitalWrite(RED_LED, LOW);

            redLedState = false;
            break;

        case MODERATE_LOAD:

            digitalWrite(GREEN_LED, LOW);
            digitalWrite(YELLOW_LED, HIGH);
            digitalWrite(RED_LED, LOW);

            redLedState = false;
            break;

        case HIGH_LOAD:

            digitalWrite(GREEN_LED, LOW);
            digitalWrite(YELLOW_LED, LOW);

            // Blink the red LED during high consumption.
            if (currentTime - lastBlinkTime >= BLINK_INTERVAL) {

                lastBlinkTime = currentTime;

                redLedState = !redLedState;

                digitalWrite(
                    RED_LED,
                    redLedState ? HIGH : LOW
                );
            }

            break;
    }
}

// ============================================================
// DISPLAY ENERGY STATUS
// ============================================================

void displayEnergyStatus() {

    Serial.println();
    Serial.println("========================================");
    Serial.println("       CHRIST SMART CAMPUS");
    Serial.println("          SMART ENERGY");
    Serial.println("========================================");

    Serial.print("Device ID: ");
    Serial.println(DEVICE_ID);

    Serial.println("----------------------------------------");

    Serial.print("Voltage: ");
    Serial.print(voltage, 2);
    Serial.println(" V (simulated)");

    Serial.print("Current: ");
    Serial.print(current, 3);
    Serial.println(" A (simulated)");

    Serial.print("Power: ");
    Serial.print(power, 2);
    Serial.println(" W (simulated)");

    Serial.print("Energy Consumed: ");
    Serial.print(energyKWh, 6);
    Serial.println(" kWh (simulated)");

    Serial.println("----------------------------------------");

    Serial.print("Load Status: ");
    Serial.println(getLoadStatusName(loadStatus));

    Serial.print("LED Indicator: ");

    switch (loadStatus) {

        case NO_LOAD:
            Serial.println("ALL OFF");
            break;

        case NORMAL_LOAD:
            Serial.println("GREEN ON");
            break;

        case MODERATE_LOAD:
            Serial.println("YELLOW ON");
            break;

        case HIGH_LOAD:
            Serial.println("RED BLINKING");
            break;
    }

    if (loadStatus == HIGH_LOAD) {
        Serial.println("ALERT: HIGH POWER CONSUMPTION!");
    }

    Serial.println("========================================");
}


const char* getLoadStatusCode(LoadStatus status) {
    switch (status) {
        case NO_LOAD: return "no_load";
        case NORMAL_LOAD: return "normal";
        case MODERATE_LOAD: return "moderate";
        case HIGH_LOAD: return "high_consumption";
        default: return "unknown";
    }
}

// ============================================================
// DASHBOARD TELEMETRY - ONE AGGREGATED JSON RECORD
// All electrical readings in this module are simulated.
// ============================================================
String getTimestamp() {
    time_t now = time(nullptr);
    if (now < 1700000000) return "";

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    char timestamp[25];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(timestamp);
}

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

    if (now >= 1700000000) Serial.println("Time synchronized successfully.");
    else Serial.println("WARNING: Time synchronization failed.");
}

bool connectMQTT() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi is disconnected. MQTT connection skipped.");
        return false;
    }

    if (mqttClient.connected()) return true;

    Serial.println();
    Serial.println("--- MQTT TLS CONNECTION ---");
    Serial.println("Connecting to broker.emqx.io:8883...");

    String clientId = String(DEVICE_ID) + "-" +
                      String((uint32_t)ESP.getEfuseMac(), HEX);

    if (mqttClient.connect(clientId.c_str())) {
        Serial.println("MQTT TLS CONNECTION SUCCESSFUL!");
        Serial.print("Client ID: ");
        Serial.println(clientId);
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

void publishDeviceStatus() {
    if (!mqttClient.connected()) return;

    String timestamp = getTimestamp();
    String payload = "{";
    payload += "\"message_type\":\"device_status\",";
    payload += "\"schema_version\":\"1.0\",";
    payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",";
    payload += "\"module\":\"energy\",";
    payload += "\"building_id\":\"" + String(BUILDING_ID) + "\",";
    payload += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
    payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    payload += "\"timestamp\":";
    payload += timestamp.length() ? "\"" + timestamp + "\"" : "null";
    payload += ",\"timestamp_ms\":" + String(millis()) + ",";
    payload += "\"status\":\"online\"}";

    if (mqttClient.publish(STATUS_TOPIC, payload.c_str())) {
        Serial.println("Energy device online status published.");
    } else {
        Serial.println("Energy device status publish failed.");
    }
}

void publishEnergyTelemetry() {
    String payload;
    payload.reserve(1000);
    payload = "{";
    payload += "\"message_type\":\"telemetry\",\"schema_version\":\"1.0\",";
    payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",";
    payload += "\"module\":\"energy\",\"building_id\":\"" + String(BUILDING_ID) + "\",";
    payload += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
    payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    String timestamp = getTimestamp();
    payload += "\"timestamp\":";
    payload += timestamp.length() ? "\"" + timestamp + "\"" : "null";
    payload += ",\"timestamp_ms\":" + String(millis()) + ",";
    payload += "\"measurement_mode\":\"simulated\",\"metrics\":{";
    payload += "\"voltage\":{\"value\":" + String(voltage, 2) + ",\"unit\":\"V\",\"status\":\"normal\"},";
    payload += "\"current\":{\"value\":" + String(current, 3) + ",\"unit\":\"A\",\"status\":\"normal\"},";
    payload += "\"power\":{\"value\":" + String(power, 2) + ",\"unit\":\"W\",\"status\":\"normal\"},";
    payload += "\"energy_consumed\":{\"value\":" + String(energyKWh, 6) + ",\"unit\":\"kWh\",\"status\":\"normal\"},";
    payload += "\"load_status\":{\"value\":\"" + String(getLoadStatusCode(loadStatus)) + "\",\"unit\":\"state\",\"status\":\"normal\"}";
    payload += "}}";
    Serial.println(payload);

    if (mqttClient.connected()) {
        if (mqttClient.publish(TELEMETRY_TOPIC, payload.c_str())) {
            Serial.println("Energy telemetry published to MQTT.");
        } else {
            Serial.println("Energy telemetry MQTT publish failed.");
        }
    } else {
        Serial.println("MQTT is disconnected. Energy telemetry was not sent.");
    }
}

// ============================================================
// INITIALIZE HARDWARE
// ============================================================

void initializeHardware() {

    pinMode(POT_PIN, INPUT);

    pinMode(GREEN_LED, OUTPUT);
    pinMode(YELLOW_LED, OUTPUT);
    pinMode(RED_LED, OUTPUT);

    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, LOW);

    analogReadResolution(12);
    analogSetPinAttenuation(POT_PIN, ADC_11db);
}

// ============================================================
// SETUP
// ============================================================

void setup() {

    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println("       CHRIST SMART CAMPUS");
    Serial.println("          SMART ENERGY");
    Serial.println("========================================");

    Serial.println("Initializing energy monitoring system...");

    initializeHardware();

    connectWiFi();
    syncTime();

    // Prototype setting: certificate validation is disabled.
    // Replace with the broker CA certificate for production deployments.
    secureClient.setInsecure();
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(5);
    mqttClient.setBufferSize(1024);

    connectMQTT();

    lastReadingTime = millis();
    lastBlinkTime = millis();

    Serial.println("Hardware initialized successfully.");
    Serial.println("Measurement mode: SIMULATED");
    Serial.println("Voltage: 12 V (fixed simulation)");
    Serial.println("Current range: 0 to 2 A (simulated)");
    Serial.println("Adjust the potentiometer to change load.");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

    maintainMQTTConnection();

    unsigned long currentTime = millis();

    // Update simulated measurements once per second.
    if (currentTime - lastReadingTime >= READING_INTERVAL) {

        unsigned long elapsedMs =
            currentTime - lastReadingTime;

        lastReadingTime = currentTime;

        current = readSimulatedCurrent();

        power = calculatePower(voltage, current);

        calculateEnergy(elapsedMs);

        loadStatus = determineLoadStatus();

        displayEnergyStatus();
        publishEnergyTelemetry();
    }

    // Called continuously so red LED blinks smoothly.
    updateLEDs(millis());

    if (mqttClient.connected() &&
        currentTime - lastStatusPublish >= STATUS_INTERVAL) {
        lastStatusPublish = currentTime;
        publishDeviceStatus();
    }
}
