
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

// ---------------- WIFI CONFIGURATION ----------------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// ---------------- MQTT CONFIGURATION ----------------
const char* MQTT_BROKER = "broker.emqx.io";
const int MQTT_PORT = 8883;

const char* TELEMETRY_TOPIC =
    "christ-smart-campus/v1/parking/BLOCK-A/PARK-A-ESP32-01/telemetry";

const char* STATUS_TOPIC =
    "christ-smart-campus/v1/parking/BLOCK-A/PARK-A-ESP32-01/status";

const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long TELEMETRY_INTERVAL = 3000;
const unsigned long STATUS_INTERVAL = 30000;

unsigned long lastMQTTAttempt = 0;
unsigned long lastTelemetryPublish = 0;
unsigned long lastStatusPublish = 0;

// ============================================================
// CHRIST SMART CAMPUS - SMART PARKING MODULE
// ============================================================

// ---------------- PARKING CONFIGURATION ----------------

#define TOTAL_SLOTS 4

const char* DEVICE_ID = "PARK-A-ESP32-01";
const char* CAMPUS_ID = "CHRIST-DEMO";

const char* BUILDING_ID = "BLOCK-A";
const char* ZONE_ID = "PARKING-AREA-A";

// ---------------- GPIO PIN CONFIGURATION ----------------

// Ultrasonic TRIG pins
const int trigPins[TOTAL_SLOTS] = {
    5, 18, 19, 21
};

// Ultrasonic ECHO pins
const int echoPins[TOTAL_SLOTS] = {
    34, 35, 32, 33
};

// Green LEDs indicate FREE slots
const int greenPins[TOTAL_SLOTS] = {
    25, 27, 12, 4
};

// Red LEDs indicate OCCUPIED slots
const int redPins[TOTAL_SLOTS] = {
    26, 14, 13, 16
};

// ---------------- DISTANCE CONFIGURATION ----------------

// A vehicle within this distance is considered present.
// Calibrate this threshold using your simulated parking layout.
const float OCCUPIED_THRESHOLD_CM = 50.0;

// Maximum time to wait for an ultrasonic echo
const unsigned long ECHO_TIMEOUT = 30000;

// ---------------- TIMING CONFIGURATION ----------------

const unsigned long SENSOR_INTERVAL = 500;
const unsigned long DISPLAY_INTERVAL = 3000;

// Require repeated readings before accepting a state change.
const int REQUIRED_CONFIRMATIONS = 3;

// ---------------- SLOT STATES ----------------

enum SlotState {
    UNKNOWN,
    FREE,
    OCCUPIED,
    SENSOR_ERROR
};

struct ParkingSlot {
    int slotNumber;

    int trigPin;
    int echoPin;

    int greenLed;
    int redLed;

    float distance;

    SlotState state;
    SlotState candidateState;

    int confirmationCount;
    int invalidReadingCount;
};

ParkingSlot slots[TOTAL_SLOTS] = {
    {1, 5, 34, 25, 26, -1, UNKNOWN, UNKNOWN, 0, 0},
    {2, 18, 35, 27, 14, -1, UNKNOWN, UNKNOWN, 0, 0},
    {3, 19, 32, 12, 13, -1, UNKNOWN, UNKNOWN, 0, 0},
    {4, 21, 33, 4, 16, -1, UNKNOWN, UNKNOWN, 0, 0}
};

// ---------------- GLOBAL COUNTERS ----------------

int occupiedSlots = 0;
int availableSlots = 0;
int unknownSlots = TOTAL_SLOTS;

unsigned long lastSensorRead = 0;
unsigned long lastDisplay = 0;

// ============================================================
// WIFI CONNECTION
// ============================================================
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

// ============================================================
// TIME SYNCHRONIZATION
// ============================================================
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

    if (now >= 1700000000) {
        Serial.println("Time synchronized successfully.");
    } else {
        Serial.println("WARNING: Time synchronization failed.");
    }
}

String getTimestamp() {
    time_t now = time(nullptr);

    if (now < 1700000000) {
        return "";
    }

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    char timestamp[25];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

    return String(timestamp);
}

// ============================================================
// MQTT CONNECTION AND RECONNECTION
// ============================================================
bool connectMQTT() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi is disconnected. MQTT connection skipped.");
        return false;
    }

    if (mqttClient.connected()) {
        return true;
    }

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
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (mqttClient.connected()) {
        mqttClient.loop();
        return;
    }

    if (millis() - lastMQTTAttempt >= MQTT_RECONNECT_INTERVAL) {
        lastMQTTAttempt = millis();
        connectMQTT();
    }
}

// ============================================================
// DISTANCE MEASUREMENT
// ============================================================

float readDistance(int trigPin, int echoPin) {

    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);

    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);

    digitalWrite(trigPin, LOW);

    unsigned long duration = pulseIn(
        echoPin,
        HIGH,
        ECHO_TIMEOUT
    );

    // No echo received
    if (duration == 0) {
        return -1.0;
    }

    float distance = (duration * 0.0343) / 2.0;

    // Reject physically invalid readings
    if (distance < 2.0 || distance > 400.0) {
        return -1.0;
    }

    return distance;
}

// ============================================================
// SLOT STATE CONVERSION
// ============================================================

const char* getStateName(SlotState state) {

    switch (state) {

        case FREE:
            return "FREE";

        case OCCUPIED:
            return "OCCUPIED";

        case SENSOR_ERROR:
            return "SENSOR ERROR";

        default:
            return "UNKNOWN";
    }
}

// ============================================================
// LED CONTROL
// ============================================================

void updateSlotLEDs(ParkingSlot &slot) {

    switch (slot.state) {

        case FREE:

            digitalWrite(slot.greenLed, HIGH);
            digitalWrite(slot.redLed, LOW);

            break;

        case OCCUPIED:

            digitalWrite(slot.greenLed, LOW);
            digitalWrite(slot.redLed, HIGH);

            break;

        case SENSOR_ERROR:
        case UNKNOWN:

            // Both LEDs OFF when the reading is unreliable
            digitalWrite(slot.greenLed, LOW);
            digitalWrite(slot.redLed, LOW);

            break;
    }
}

// ============================================================
// SLOT STATE VALIDATION
// ============================================================

void updateSlotState(ParkingSlot &slot, SlotState newState) {

    if (newState == SENSOR_ERROR) {

        slot.confirmationCount = 0;
        slot.candidateState = UNKNOWN;

        if (slot.invalidReadingCount < REQUIRED_CONFIRMATIONS) {
            slot.invalidReadingCount++;
        }

        if (slot.invalidReadingCount >= REQUIRED_CONFIRMATIONS) {

            if (slot.state != SENSOR_ERROR) {

                slot.state = SENSOR_ERROR;

                Serial.print("Slot ");
                Serial.print(slot.slotNumber);
                Serial.println(": SENSOR ERROR");
            }
        }

        updateSlotLEDs(slot);

        return;
    }

    slot.invalidReadingCount = 0;

    if (newState == slot.candidateState) {

        slot.confirmationCount++;

    } else {

        slot.candidateState = newState;
        slot.confirmationCount = 1;
    }

    if (slot.confirmationCount >= REQUIRED_CONFIRMATIONS) {

        if (slot.state != newState) {

            slot.state = newState;

            Serial.print("Slot ");
            Serial.print(slot.slotNumber);
            Serial.print(" changed to ");
            Serial.println(getStateName(slot.state));
        }
    }

    updateSlotLEDs(slot);
}

// ============================================================
// READ ONE PARKING SLOT
// ============================================================

void readParkingSlot(ParkingSlot &slot) {

    slot.distance = readDistance(
        slot.trigPin,
        slot.echoPin
    );

    if (slot.distance < 0) {

        updateSlotState(slot, SENSOR_ERROR);

        return;
    }

    SlotState detectedState;

    if (slot.distance <= OCCUPIED_THRESHOLD_CM) {

        detectedState = OCCUPIED;

    } else {

        detectedState = FREE;
    }

    updateSlotState(slot, detectedState);
}

// ============================================================
// READ ALL PARKING SLOTS
// ============================================================

void scanParkingArea() {

    for (int i = 0; i < TOTAL_SLOTS; i++) {

        readParkingSlot(slots[i]);

        // Allow ultrasonic echoes to settle before
        // triggering the next sensor.
        delay(60);
    }
}

// ============================================================
// CALCULATE PARKING STATISTICS
// ============================================================

void calculateParkingStatistics() {

    occupiedSlots = 0;
    availableSlots = 0;
    unknownSlots = 0;

    for (int i = 0; i < TOTAL_SLOTS; i++) {

        switch (slots[i].state) {

            case OCCUPIED:
                occupiedSlots++;
                break;

            case FREE:
                availableSlots++;
                break;

            case SENSOR_ERROR:
            case UNKNOWN:
                unknownSlots++;
                break;
        }
    }
}

// ============================================================
// DISPLAY PARKING STATUS
// ============================================================

void displayParkingStatus() {

    calculateParkingStatistics();

    Serial.println();
    Serial.println("========================================");
    Serial.println("       CHRIST SMART CAMPUS");
    Serial.println("          SMART PARKING");
    Serial.println("========================================");

    Serial.print("Device ID: ");
    Serial.println(DEVICE_ID);

    Serial.println();

    for (int i = 0; i < TOTAL_SLOTS; i++) {

        Serial.print("Parking Slot ");
        Serial.print(slots[i].slotNumber);

        Serial.print(" | Distance: ");

        if (slots[i].distance >= 0) {

            Serial.print(slots[i].distance, 2);
            Serial.print(" cm");

        } else {

            Serial.print("Unavailable");
        }

        Serial.print(" | Status: ");
        Serial.println(getStateName(slots[i].state));
    }

    Serial.println("----------------------------------------");

    Serial.print("Total Slots: ");
    Serial.println(TOTAL_SLOTS);

    Serial.print("Occupied Slots: ");
    Serial.println(occupiedSlots);

    Serial.print("Available Slots: ");
    Serial.println(availableSlots);

    Serial.print("Unknown / Error Slots: ");
    Serial.println(unknownSlots);

    Serial.println("----------------------------------------");

    Serial.println("Parking occupancy: ");

    if (TOTAL_SLOTS > 0) {

        float occupancyPercentage =
            (occupiedSlots * 100.0) / TOTAL_SLOTS;

        Serial.print(occupancyPercentage, 1);
        Serial.println("%");
    }

    Serial.println("========================================");
}


// ============================================================
// DASHBOARD TELEMETRY - ONE AGGREGATED JSON RECORD
// ============================================================
const char* getStateCode(SlotState state) {
    switch (state) {
        case FREE: return "free";
        case OCCUPIED: return "occupied";
        case SENSOR_ERROR: return "sensor_error";
        default: return "unknown";
    }
}

void publishParkingTelemetry() {
    calculateParkingStatistics();
    float occupancyPercentage = (occupiedSlots * 100.0f) / TOTAL_SLOTS;

    String payload;
    payload.reserve(1400);
    payload = "{";
    payload += "\"message_type\":\"telemetry\",\"schema_version\":\"1.0\",";
    payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",";
    payload += "\"module\":\"parking\",\"building_id\":\"" + String(BUILDING_ID) + "\",";
    payload += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
    payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    String timestamp = getTimestamp();
    payload += "\"timestamp\":";
    if (timestamp.length() > 0) payload += "\"" + timestamp + "\"";
    else payload += "null";
    payload += ",\"timestamp_ms\":" + String(millis()) + ",\"metrics\":{";

    for (int i = 0; i < TOTAL_SLOTS; i++) {
        if (i > 0) payload += ",";
        String slotNumber = String(slots[i].slotNumber);
        String slotState = getStateCode(slots[i].state);
        payload += "\"slot_" + slotNumber + "_status\":{\"value\":\"" + slotState + "\",\"unit\":\"state\",\"status\":\"";
        payload += (slots[i].state == SENSOR_ERROR || slots[i].state == UNKNOWN) ? "sensor_unavailable" : "normal";
        payload += "\"},";
        payload += "\"slot_" + slotNumber + "_distance\":{\"value\":";
        if (slots[i].distance >= 0) payload += String(slots[i].distance, 2);
        else payload += "null";
        payload += ",\"unit\":\"cm\",\"status\":\"";
        payload += slots[i].distance >= 0 ? "normal" : "sensor_unavailable";
        payload += "\"}";
    }
    payload += ",\"total_slots\":{\"value\":" + String(TOTAL_SLOTS) + ",\"unit\":\"slots\",\"status\":\"normal\"}";
    payload += ",\"occupied_slots\":{\"value\":" + String(occupiedSlots) + ",\"unit\":\"slots\",\"status\":\"normal\"}";
    payload += ",\"available_slots\":{\"value\":" + String(availableSlots) + ",\"unit\":\"slots\",\"status\":\"normal\"}";
    payload += ",\"unknown_slots\":{\"value\":" + String(unknownSlots) + ",\"unit\":\"slots\",\"status\":\"";
    payload += unknownSlots > 0 ? "sensor_unavailable" : "normal";
    payload += "\"}";
    payload += ",\"occupancy_percentage\":{\"value\":" + String(occupancyPercentage, 1) + ",\"unit\":\"%\",\"status\":\"normal\"}";
    payload += "}}";

    Serial.println(payload);

    if (mqttClient.connected()) {
        if (mqttClient.publish(TELEMETRY_TOPIC, payload.c_str())) {
            Serial.println("Parking telemetry published to MQTT.");
        } else {
            Serial.println("Parking MQTT telemetry publish failed.");
        }
    } else {
        Serial.println("MQTT unavailable; telemetry remains available on Serial.");
    }
}

// ============================================================
// DEVICE STATUS / HEARTBEAT
// ============================================================
void publishDeviceStatus() {
    if (!mqttClient.connected()) {
        return;
    }

    String timestamp = getTimestamp();

    String payload = "{";
    payload += "\"message_type\":\"device_status\",";
    payload += "\"schema_version\":\"1.0\",";
    payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",";
    payload += "\"module\":\"parking\",";
    payload += "\"building_id\":\"" + String(BUILDING_ID) + "\",";
    payload += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
    payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    payload += "\"timestamp\":";
    if (timestamp.length() > 0) payload += "\"" + timestamp + "\"";
    else payload += "null";
    payload += ",\"timestamp_ms\":" + String(millis()) + ",";
    payload += "\"status\":\"online\"}";

    if (mqttClient.publish(STATUS_TOPIC, payload.c_str())) {
        Serial.println("Parking device online status published.");
    } else {
        Serial.println("Parking device status publish failed.");
    }
}

// ============================================================
// INITIALIZE HARDWARE
// ============================================================

void initializeHardware() {

    for (int i = 0; i < TOTAL_SLOTS; i++) {

        pinMode(slots[i].trigPin, OUTPUT);
        pinMode(slots[i].echoPin, INPUT);

        pinMode(slots[i].greenLed, OUTPUT);
        pinMode(slots[i].redLed, OUTPUT);

        digitalWrite(slots[i].trigPin, LOW);

        digitalWrite(slots[i].greenLed, LOW);
        digitalWrite(slots[i].redLed, LOW);
    }
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
    Serial.println("          SMART PARKING");
    Serial.println("========================================");

    Serial.println("Initializing parking system...");

    initializeHardware();

    Serial.println("Hardware initialized successfully.");

    Serial.print("Total parking slots: ");
    Serial.println(TOTAL_SLOTS);

    Serial.println("Waiting for sensor readings...");

    connectWiFi();
    syncTime();

    // Matches the current Classroom prototype configuration.
    // Certificate validation is disabled; use a trusted CA certificate
    // before any production deployment.
    secureClient.setInsecure();

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(5);

    // The parking JSON contains several slot metrics; allocate a larger
    // packet buffer than the smaller Classroom payload.
    mqttClient.setBufferSize(2048);

    lastMQTTAttempt = millis();
    connectMQTT();
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

    maintainMQTTConnection();

    unsigned long currentMillis = millis();

    // Read all four parking sensors
    if (currentMillis - lastSensorRead >= SENSOR_INTERVAL) {

        lastSensorRead = currentMillis;

        scanParkingArea();
    }

    // Display updated parking statistics
    if (currentMillis - lastDisplay >= DISPLAY_INTERVAL) {

        lastDisplay = currentMillis;

        displayParkingStatus();
        publishParkingTelemetry();
    }

    if (
        mqttClient.connected() &&
        millis() - lastStatusPublish >= STATUS_INTERVAL
    ) {
        lastStatusPublish = millis();
        publishDeviceStatus();
    }
}
