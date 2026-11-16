#include <Arduino.h>
#include <DHTesp.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>

// ============================================================
// CHRIST SMART CAMPUS - INTELLIGENT CLASSROOM
// ============================================================

// ---------------- PIN CONFIGURATION ----------------
#define LDR_PIN          34
#define PIR_PIN          27
#define DHT_PIN          15
#define LIGHT_LED        25
#define FAN_SERVO_PIN    26
#define TRIG_PIN         5
#define ECHO_PIN         18
#define DOOR_SERVO_PIN   14

// ---------------- OBJECTS ----------------
DHTesp dhtSensor;
Servo fanServo;
Servo doorServo;

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

// ---------------- WIFI CONFIGURATION ----------------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// ---------------- MQTT CONFIGURATION ----------------
const char* MQTT_BROKER = "broker.emqx.io";
const int MQTT_PORT = 8883;

const char* DEVICE_ID = "CLASS-A-ESP32-01";
const char* CAMPUS_ID = "CHRIST-DEMO";

const char* TELEMETRY_TOPIC =
    "christ-smart-campus/v1/classroom/BLOCK-A/CLASS-A-ESP32-01/telemetry";

const char* STATUS_TOPIC =
    "christ-smart-campus/v1/classroom/BLOCK-A/CLASS-A-ESP32-01/status";

// ---------------- AUTOMATION CONFIGURATION ----------------
const int DARK_THRESHOLD = 2000;
const float FAN_ON_TEMP = 30.0;
const float FAN_OFF_TEMP = 27.0;
const float DOOR_DISTANCE_CM = 50.0;

const int DOOR_OPEN_ANGLE = 90;
const int DOOR_CLOSED_ANGLE = 0;

const unsigned long OCCUPANCY_TIMEOUT = 30000;
const unsigned long DOOR_CLOSE_DELAY = 3000;
const unsigned long DHT_INTERVAL = 2000;
const unsigned long SENSOR_INTERVAL = 250;

const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long TELEMETRY_INTERVAL = 10000;
const unsigned long STATUS_INTERVAL = 30000;

// ---------------- STATE VARIABLES ----------------
bool classroomOccupied = false;
bool fanOn = false;
bool doorOpen = false;
bool dhtAvailable = false;

unsigned long lastMotionTime = 0;
unsigned long lastDoorDetection = 0;
unsigned long lastDHTRead = 0;
unsigned long lastSensorRead = 0;
unsigned long lastMQTTAttempt = 0;
unsigned long lastTelemetryPublish = 0;
unsigned long lastStatusPublish = 0;

int lightValue = 0;
int motionDetected = 0;

float temperature = NAN;
float humidity = NAN;
float distance = -1.0;

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
// STRUCTURED CLASSROOM TELEMETRY
// One JSON object contains all current module readings/states.
// It is always printed to Serial and published to MQTT when connected.
// ============================================================
void appendFloatMetric(
    String &payload,
    const char* metric,
    float value,
    const char* unit,
    const char* status,
    bool &firstMetric
) {
    if (!firstMetric) payload += ",";
    firstMetric = false;

    payload += "\"";
    payload += metric;
    payload += "\":{\"value\":";

    if (isnan(value) || isinf(value)) {
        payload += "null";
    } else {
        payload += String(value, 2);
    }

    payload += ",\"unit\":\"";
    payload += unit;
    payload += "\",\"status\":\"";
    payload += status;
    payload += "\"}";
}

void appendBooleanMetric(
    String &payload,
    const char* metric,
    bool value,
    const char* status,
    bool &firstMetric
) {
    if (!firstMetric) payload += ",";
    firstMetric = false;

    payload += "\"";
    payload += metric;
    payload += "\":{\"value\":";
    payload += value ? "true" : "false";
    payload += ",\"unit\":\"boolean\",\"status\":\"";
    payload += status;
    payload += "\"}";
}

String buildClassroomTelemetry() {
    String payload;
    payload.reserve(1000);

    String timestamp = getTimestamp();

    payload = "{";
    payload += "\"message_type\":\"telemetry\",";
    payload += "\"schema_version\":\"1.0\",";
    payload += "\"campus_id\":\"";
    payload += CAMPUS_ID;
    payload += "\",\"module\":\"classroom\",";
    payload += "\"building_id\":\"BLOCK-A\",";
    payload += "\"zone_id\":\"ROOM-204\",";
    payload += "\"device_id\":\"";
    payload += DEVICE_ID;
    payload += "\",";

    payload += "\"timestamp\":";
    if (timestamp.length() > 0) {
        payload += "\"" + timestamp + "\"";
    } else {
        payload += "null";
    }
    payload += ",\"timestamp_ms\":";
    payload += String(millis());
    payload += ",\"metrics\":{";

    bool firstMetric = true;

    appendBooleanMetric(payload, "occupancy_detected", classroomOccupied,
                        "normal", firstMetric);

    appendFloatMetric(payload, "light_level", (float)lightValue,
                      "raw", "normal", firstMetric);

    appendFloatMetric(payload, "temperature", temperature,
                      "C", dhtAvailable ? "normal" : "sensor_unavailable", firstMetric);

    appendFloatMetric(payload, "humidity", humidity,
                      "%", dhtAvailable ? "normal" : "sensor_unavailable", firstMetric);

    appendBooleanMetric(payload, "lighting_state",
                        digitalRead(LIGHT_LED) == HIGH, "normal", firstMetric);

    appendBooleanMetric(payload, "fan_state", fanOn,
                        dhtAvailable ? "normal" : "sensor_unavailable", firstMetric);

    appendBooleanMetric(payload, "door_open", doorOpen,
                        "normal", firstMetric);

    appendFloatMetric(payload, "distance", distance,
                      "cm", distance >= 0 ? "normal" : "sensor_unavailable", firstMetric);

    payload += "}}";
    return payload;
}

void publishAllTelemetry() {
    String payload = buildClassroomTelemetry();

    // Always print one complete JSON record for serial capture/debugging.
    Serial.println(payload);

    // Publish the exact same record when MQTT is connected.
    if (mqttClient.connected()) {
        bool published = mqttClient.publish(TELEMETRY_TOPIC, payload.c_str());

        if (published) {
            Serial.println("Classroom telemetry published to MQTT.");
        } else {
            Serial.println("Classroom MQTT telemetry publish failed.");
        }
    } else {
        Serial.println("MQTT unavailable; telemetry remains available on Serial.");
    }
}

void publishDeviceStatus() {
    if (!mqttClient.connected()) {
        return;
    }

    String timestamp = getTimestamp();

    String payload = "{";
    payload += "\"message_type\":\"device_status\",";
    payload += "\"schema_version\":\"1.0\",";
    payload += "\"campus_id\":\"";
    payload += CAMPUS_ID;
    payload += "\",\"module\":\"classroom\",";
    payload += "\"building_id\":\"BLOCK-A\",";
    payload += "\"zone_id\":\"ROOM-204\",";
    payload += "\"device_id\":\"";
    payload += DEVICE_ID;
    payload += "\",\"timestamp\":";

    if (timestamp.length() > 0) {
        payload += "\"" + timestamp + "\"";
    } else {
        payload += "null";
    }

    payload += ",\"timestamp_ms\":";
    payload += String(millis());
    payload += ",\"status\":\"online\"}";

    if (mqttClient.publish(STATUS_TOPIC, payload.c_str())) {
        Serial.println("Device online status published.");
    } else {
        Serial.println("Device status publish failed.");
    }
}

// ============================================================
// ULTRASONIC SENSOR
// ============================================================
float readDistance() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);

    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);

    digitalWrite(TRIG_PIN, LOW);

    unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000);

    if (duration == 0) {
        return -1.0;
    }

    return duration * 0.0343 / 2.0;
}

// ============================================================
// OCCUPANCY DETECTION
// ============================================================
void updateOccupancy() {
    motionDetected = digitalRead(PIR_PIN);

    if (motionDetected == HIGH) {
        lastMotionTime = millis();

        if (!classroomOccupied) {
            classroomOccupied = true;
            Serial.println("CLASSROOM: OCCUPIED");
        }
    }

    if (
        classroomOccupied &&
        millis() - lastMotionTime >= OCCUPANCY_TIMEOUT
    ) {
        classroomOccupied = false;
        Serial.println("CLASSROOM: UNOCCUPIED");
    }
}

// ============================================================
// LIGHTING CONTROL
// ============================================================
void controlLighting() {
    lightValue = analogRead(LDR_PIN);

    bool isDark = lightValue < DARK_THRESHOLD;
    bool lightRequired = classroomOccupied && isDark;

    digitalWrite(LIGHT_LED, lightRequired ? HIGH : LOW);
}

// ============================================================
// ENVIRONMENT SENSOR
// ============================================================
void readEnvironment() {
    if (millis() - lastDHTRead < DHT_INTERVAL) {
        return;
    }

    lastDHTRead = millis();

    TempAndHumidity data = dhtSensor.getTempAndHumidity();

    if (isnan(data.temperature) || isnan(data.humidity)) {
        dhtAvailable = false;
        Serial.println("ERROR: DHT22 reading failed");

        // Fail-safe: turn the fan indicator off if temperature
        // information is unavailable.
        fanOn = false;
        fanServo.write(0);
        return;
    }

    dhtAvailable = true;
    temperature = data.temperature;
    humidity = data.humidity;
}

// ============================================================
// FAN CONTROL
// ============================================================
void controlFan() {
    if (!dhtAvailable || !classroomOccupied) {
        fanOn = false;
    } else if (temperature > FAN_ON_TEMP) {
        fanOn = true;
    } else if (temperature < FAN_OFF_TEMP) {
        fanOn = false;
    }

    // Between 27 C and 30 C, retain the previous fan state.
    fanServo.write(fanOn ? 180 : 0);
}

// ============================================================
// DOOR CONTROL
// ============================================================
void controlDoor(float measuredDistance) {
    if (measuredDistance > 0 && measuredDistance <= DOOR_DISTANCE_CM) {
        lastDoorDetection = millis();

        if (!doorOpen) {
            doorServo.write(DOOR_OPEN_ANGLE);
            doorOpen = true;
            Serial.println("DOOR: OPEN");
        }
    }

    if (doorOpen && millis() - lastDoorDetection >= DOOR_CLOSE_DELAY) {
        doorServo.write(DOOR_CLOSED_ANGLE);
        doorOpen = false;
        Serial.println("DOOR: CLOSED");
    }
}

// ============================================================
// CLASSROOM STATUS DISPLAY
// ============================================================
void printClassroomStatus() {
    Serial.println("\n--- CLASSROOM STATUS ---");

    Serial.print("Occupancy: ");
    Serial.println(classroomOccupied ? "OCCUPIED" : "UNOCCUPIED");

    Serial.print("LDR: ");
    Serial.println(lightValue);

    Serial.print("Temperature: ");
    if (dhtAvailable) {
        Serial.print(temperature);
        Serial.println(" C");
    } else {
        Serial.println("UNAVAILABLE");
    }

    Serial.print("Humidity: ");
    if (dhtAvailable) {
        Serial.print(humidity);
        Serial.println(" %");
    } else {
        Serial.println("UNAVAILABLE");
    }

    Serial.print("Lighting: ");
    Serial.println(digitalRead(LIGHT_LED) ? "ON" : "OFF");

    Serial.print("Fan: ");
    Serial.println(fanOn ? "ON" : "OFF");

    Serial.print("Door: ");
    Serial.println(doorOpen ? "OPEN" : "CLOSED");

    Serial.print("Distance: ");
    if (distance >= 0) {
        Serial.print(distance);
        Serial.println(" cm");
    } else {
        Serial.println("NO ECHO");
    }
}


void testEMQXTCP() {
    Serial.println("\n--- EMQX TCP DIAGNOSTIC ---");

    IPAddress brokerIP;

    if (!WiFi.hostByName("broker.emqx.io", brokerIP)) {
        Serial.println("DNS FAILED");
        return;
    }

    Serial.print("Resolved IP: ");
    Serial.println(brokerIP);

    WiFiClient testClient;

    Serial.println("Testing TCP port 1883...");

    if (testClient.connect(brokerIP, 1883)) {
        Serial.println("TCP 1883 SUCCESS");
        testClient.stop();
    } else {
        Serial.println("TCP 1883 FAILED");
    }

    Serial.println("Testing TCP port 8883...");

    if (testClient.connect(brokerIP, 8883)) {
        Serial.println("TCP 8883 SUCCESS");
        testClient.stop();
    } else {
        Serial.println("TCP 8883 FAILED");
    }

    Serial.println("----------------------------");
}


// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);

    pinMode(LDR_PIN, INPUT);
    pinMode(PIR_PIN, INPUT);
    pinMode(LIGHT_LED, OUTPUT);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    digitalWrite(LIGHT_LED, LOW);
    digitalWrite(TRIG_PIN, LOW);

    dhtSensor.setup(DHT_PIN, DHTesp::DHT22);

    fanServo.setPeriodHertz(50);
    fanServo.attach(FAN_SERVO_PIN, 500, 2400);

    doorServo.setPeriodHertz(50);
    doorServo.attach(DOOR_SERVO_PIN, 500, 2400);

    fanServo.write(0);
    doorServo.write(DOOR_CLOSED_ANGLE);

    Serial.println();
    Serial.println("================================");
    Serial.println(" CHRIST SMART CAMPUS");
    Serial.println(" INTELLIGENT CLASSROOM");
    Serial.println("================================");
    Serial.println("System initialized.");

    connectWiFi();

    Serial.println("\n--- MQTT DNS TEST ---");

IPAddress brokerIP;

if (WiFi.hostByName("broker.emqx.io", brokerIP)) {
    Serial.print("EMQX IP resolved: ");
    Serial.println(brokerIP);
} else {
    Serial.println("EMQX DNS resolution FAILED");
}

if (WiFi.hostByName("google.com", brokerIP)) {
    Serial.print("Google IP resolved: ");
    Serial.println(brokerIP);
} else {
    Serial.println("Google DNS resolution FAILED");
}

    syncTime();

    // Temporary prototype TLS setup. Certificate validation is
    // disabled here; configure a trusted CA certificate before
    // production deployment.
    secureClient.setInsecure();

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(5);
    mqttClient.setBufferSize(1024);

    // Make the first MQTT attempt. Classroom automation continues
    // even if the broker is unavailable; reconnection is retried.
    lastMQTTAttempt = millis();
    connectMQTT();
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
    maintainMQTTConnection();

    updateOccupancy();
    controlLighting();
    readEnvironment();
    controlFan();

    if (millis() - lastSensorRead >= SENSOR_INTERVAL) {
        lastSensorRead = millis();

        distance = readDistance();
        controlDoor(distance);

        printClassroomStatus();
    }

    if (millis() - lastTelemetryPublish >= TELEMETRY_INTERVAL) {
        lastTelemetryPublish = millis();
        publishAllTelemetry();
    }

    if (
        mqttClient.connected() &&
        millis() - lastStatusPublish >= STATUS_INTERVAL
    ) {
        lastStatusPublish = millis();
        publishDeviceStatus();
    }
}
