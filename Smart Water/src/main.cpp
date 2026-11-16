
#include <Arduino.h>

// ============================================================
// CHRIST SMART CAMPUS - SMART WATER MANAGEMENT
// ============================================================

// ---------------- DEVICE CONFIGURATION ----------------

const char* DEVICE_ID = "WATER-A-ESP32-01";
const char* CAMPUS_ID = "CHRIST-DEMO";
const char* BUILDING_ID = "BLOCK-A";
const char* ZONE_ID = "WATER-TANK-A";

// ---------------- GPIO CONFIGURATION ----------------

// Ultrasonic sensor
#define TRIG_PIN 5
#define ECHO_PIN 18

// Simulation buttons
#define FLOW_PIN 32
#define LEAK_PIN 33

// Pump indicator
#define PUMP_LED 19

// Status LEDs
#define GREEN_LED 25
#define YELLOW_LED 26
#define RED_LED 27

// ---------------- TANK CONFIGURATION ----------------

// Calibrated tank distances
const float EMPTY_DISTANCE_CM = 100.0;
const float FULL_DISTANCE_CM = 10.0;

// Small tolerance for measurement variation
const float CALIBRATION_TOLERANCE_CM = 5.0;

// Automatic pump thresholds
const float LOW_LEVEL_THRESHOLD = 20.0;
const float FULL_LEVEL_THRESHOLD = 90.0;

// ---------------- FLOW CONFIGURATION ----------------

// Approximate YF-S201 calibration.
// Must be calibrated against the actual sensor.
const float PULSES_PER_LITRE = 450.0;

// ---------------- TIMING CONFIGURATION ----------------

const unsigned long LEVEL_INTERVAL = 1000;
const unsigned long FLOW_INTERVAL = 1000;
const unsigned long DISPLAY_INTERVAL = 2000;

const unsigned long ECHO_TIMEOUT = 30000;

// ---------------- WATER LEVEL VARIABLES ----------------

float distance = -1.0;
float tankLevel = 0.0;

bool sensorValid = false;
bool tankCalibrationValid = false;

// ---------------- WATER FLOW VARIABLES ----------------

volatile unsigned long flowPulseCount = 0;

unsigned long previousFlowPulseCount = 0;
unsigned long lastFlowRead = 0;

float flowRate = 0.0;
float totalWaterVolume = 0.0;

// ---------------- SYSTEM STATES ----------------

bool pumpRunning = false;
bool leakDetected = false;

// ---------------- TIMERS ----------------

unsigned long lastLevelRead = 0;
unsigned long lastDisplay = 0;

// ============================================================
// FLOW SENSOR INTERRUPT
// ============================================================

void IRAM_ATTR countFlowPulse() {

    static unsigned long lastPulseTime = 0;

    unsigned long currentTime = micros();

    // Ignore electrical bounce
    if (currentTime - lastPulseTime >= 2000) {

        flowPulseCount++;
        lastPulseTime = currentTime;
    }
}

// ============================================================
// ULTRASONIC DISTANCE MEASUREMENT
// ============================================================

float readDistance() {

    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);

    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);

    digitalWrite(TRIG_PIN, LOW);

    unsigned long duration = pulseIn(
        ECHO_PIN,
        HIGH,
        ECHO_TIMEOUT
    );

    if (duration == 0) {
        return -1.0;
    }

    float measuredDistance =
        duration * 0.0343 / 2.0;

    // Validate the HC-SR04 measurement range
    if (measuredDistance < 2.0 ||
        measuredDistance > 400.0) {

        return -1.0;
    }

    return measuredDistance;
}

// ============================================================
// WATER LEVEL CALCULATION
// ============================================================

void updateWaterLevel() {

    distance = readDistance();

    sensorValid = false;
    tankCalibrationValid = false;

    if (distance < 0) {

        Serial.println(
            "ERROR: Ultrasonic sensor reading invalid!"
        );

        return;
    }

    sensorValid = true;

    // Check whether the measurement fits the tank calibration
    if (distance < FULL_DISTANCE_CM - CALIBRATION_TOLERANCE_CM ||
        distance > EMPTY_DISTANCE_CM + CALIBRATION_TOLERANCE_CM) {

        Serial.println(
            "WARNING: Distance outside calibrated tank range!"
        );

        return;
    }

    tankCalibrationValid = true;

    // Calculate percentage
    float percentage =
        ((EMPTY_DISTANCE_CM - distance) /
        (EMPTY_DISTANCE_CM - FULL_DISTANCE_CM)) * 100.0;

    tankLevel = constrain(percentage, 0.0, 100.0);
}

// ============================================================
// WATER FLOW CALCULATION
// ============================================================

void updateWaterFlow(unsigned long currentTime) {

    unsigned long elapsedTime =
        currentTime - lastFlowRead;

    if (elapsedTime == 0) {
        return;
    }

    unsigned long currentPulseCount;

    noInterrupts();
    currentPulseCount = flowPulseCount;
    interrupts();

    unsigned long newPulses =
        currentPulseCount - previousFlowPulseCount;

    previousFlowPulseCount = currentPulseCount;

    lastFlowRead = currentTime;

    // Convert pulses into litres
    float litres =
        newPulses / PULSES_PER_LITRE;

    // Calculate flow rate using actual elapsed milliseconds
    flowRate =
        (litres * 60000.0) / elapsedTime;

    // Accumulate total consumption
    totalWaterVolume += litres;
}

// ============================================================
// LEAK DETECTION
// ============================================================

void checkLeak() {

    // Button pressed = LOW
    leakDetected = (digitalRead(LEAK_PIN) == LOW);
}

// ============================================================
// AUTOMATIC PUMP CONTROL
// ============================================================

void controlPump() {

    // Stop pump if leakage is detected
    if (leakDetected) {

        pumpRunning = false;
        return;
    }

    // Stop pump if sensor measurement is unreliable
    if (!sensorValid || !tankCalibrationValid) {

        pumpRunning = false;
        return;
    }

    // Start pump when tank is low
    if (tankLevel <= LOW_LEVEL_THRESHOLD) {

        pumpRunning = true;
    }

    // Stop pump when tank is sufficiently full
    else if (tankLevel >= FULL_LEVEL_THRESHOLD) {

        pumpRunning = false;
    }

    // Between thresholds, retain previous state
}

// ============================================================
// LED STATUS INDICATION
// ============================================================

void updateLEDs() {

    digitalWrite(
        PUMP_LED,
        pumpRunning ? HIGH : LOW
    );

    // Critical: leak or invalid tank measurement
    if (leakDetected ||
        !sensorValid ||
        !tankCalibrationValid) {

        digitalWrite(GREEN_LED, LOW);
        digitalWrite(YELLOW_LED, LOW);
        digitalWrite(RED_LED, HIGH);

        return;
    }

    // Low water warning
    if (tankLevel <= LOW_LEVEL_THRESHOLD) {

        digitalWrite(GREEN_LED, LOW);
        digitalWrite(YELLOW_LED, HIGH);
        digitalWrite(RED_LED, LOW);

        return;
    }

    // Normal operation
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, LOW);
}

// ============================================================
// SERIAL MONITOR DISPLAY
// ============================================================

void displayStatus() {

    Serial.println();
    Serial.println("======================================");
    Serial.println(" CHRIST SMART CAMPUS - SMART WATER");
    Serial.println("======================================");

    Serial.print("Device ID: ");
    Serial.println(DEVICE_ID);

    // Distance
    Serial.print("Tank Distance: ");

    if (sensorValid) {

        Serial.print(distance, 2);
        Serial.println(" cm");

    } else {

        Serial.println("INVALID");
    }

    // Tank level
    Serial.print("Tank Level: ");

    if (tankCalibrationValid) {

        Serial.print(tankLevel, 2);
        Serial.println(" %");

    } else if (sensorValid) {

        Serial.println("OUTSIDE CALIBRATED RANGE");

    } else {

        Serial.println("UNAVAILABLE");
    }

    // Sensor status
    Serial.print("Ultrasonic Sensor: ");

    Serial.println(
        sensorValid ? "ONLINE" : "ERROR"
    );

    Serial.print("Tank Calibration: ");

    Serial.println(
        tankCalibrationValid ? "VALID" : "INVALID"
    );

    // Flow
    Serial.print("Water Flow Rate: ");
    Serial.print(flowRate, 2);
    Serial.println(" L/min");

    Serial.print("Total Water Volume: ");
    Serial.print(totalWaterVolume, 3);
    Serial.println(" L");

    // Leak
    Serial.print("Leak Status: ");

    Serial.println(
        leakDetected ? "LEAK DETECTED" : "NORMAL"
    );

    // Pump
    Serial.print("Pump Status: ");

    Serial.println(
        pumpRunning ? "RUNNING" : "OFF"
    );

    Serial.println("======================================");
}


// ============================================================
// DASHBOARD TELEMETRY - ONE AGGREGATED JSON RECORD
// ============================================================
void publishWaterTelemetry() {
    String payload;
    payload.reserve(1200);
    payload = "{";
    payload += "\"message_type\":\"telemetry\",\"schema_version\":\"1.0\",";
    payload += "\"campus_id\":\"" + String(CAMPUS_ID) + "\",";
    payload += "\"module\":\"water\",\"building_id\":\"" + String(BUILDING_ID) + "\",";
    payload += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
    payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    payload += "\"timestamp\":null,\"timestamp_ms\":" + String(millis()) + ",\"flow_input_mode\":\"button_simulation\",\"metrics\":{";
    payload += "\"tank_distance\":{\"value\":";
    payload += sensorValid ? String(distance, 2) : "null";
    payload += ",\"unit\":\"cm\",\"status\":\"";
    payload += sensorValid ? "normal" : "sensor_unavailable";
    payload += "\"},";
    payload += "\"tank_level\":{\"value\":";
    payload += tankCalibrationValid ? String(tankLevel, 2) : "null";
    payload += ",\"unit\":\"%\",\"status\":\"";
    payload += tankCalibrationValid ? "normal" : (sensorValid ? "calibration_invalid" : "sensor_unavailable");
    payload += "\"},";
    payload += "\"flow_rate\":{\"value\":" + String(flowRate, 2) + ",\"unit\":\"L/min\",\"status\":\"simulated\"},";
    payload += "\"total_volume\":{\"value\":" + String(totalWaterVolume, 3) + ",\"unit\":\"L\",\"status\":\"normal\"},";
    payload += "\"leak_detected\":{\"value\":";
    payload += leakDetected ? "true" : "false";
    payload += ",\"unit\":\"boolean\",\"status\":\"normal\"},";
    payload += "\"pump_running\":{\"value\":";
    payload += pumpRunning ? "true" : "false";
    payload += ",\"unit\":\"boolean\",\"status\":\"normal\"},";
    payload += "\"ultrasonic_sensor_online\":{\"value\":";
    payload += sensorValid ? "true" : "false";
    payload += ",\"unit\":\"boolean\",\"status\":\"normal\"},";
    payload += "\"tank_calibration_valid\":{\"value\":";
    payload += tankCalibrationValid ? "true" : "false";
    payload += ",\"unit\":\"boolean\",\"status\":\"normal\"}";
    payload += "}}";
    Serial.println(payload);
}

// ============================================================
// SETUP
// ============================================================

void setup() {

    Serial.begin(115200);

    // Ultrasonic sensor
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    // Simulation buttons
    pinMode(FLOW_PIN, INPUT_PULLUP);
    pinMode(LEAK_PIN, INPUT_PULLUP);

    // LEDs
    pinMode(PUMP_LED, OUTPUT);
    pinMode(GREEN_LED, OUTPUT);
    pinMode(YELLOW_LED, OUTPUT);
    pinMode(RED_LED, OUTPUT);

    // Initial safe output state
    digitalWrite(PUMP_LED, LOW);
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, LOW);

    // Flow pulse interrupt
    attachInterrupt(
        digitalPinToInterrupt(FLOW_PIN),
        countFlowPulse,
        FALLING
    );

    // Initialise timers
    unsigned long currentTime = millis();

    lastLevelRead = currentTime - LEVEL_INTERVAL;
    lastFlowRead = currentTime;
    lastDisplay = currentTime;

    Serial.println();
    Serial.println("SMART WATER MANAGEMENT SYSTEM");
    Serial.println("ESP32 Initialised Successfully");

    Serial.print("Device ID: ");
    Serial.println(DEVICE_ID);

    Serial.println("Starting monitoring...");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

    unsigned long currentTime = millis();

    // 1. Update water level
    if (currentTime - lastLevelRead >= LEVEL_INTERVAL) {

        lastLevelRead = currentTime;

        updateWaterLevel();
    }

    // 2. Update water flow
    if (currentTime - lastFlowRead >= FLOW_INTERVAL) {

        updateWaterFlow(currentTime);
    }

    // 3. Check leakage
    checkLeak();

    // 4. Control pump
    controlPump();

    // 5. Update indicators
    updateLEDs();

    // 6. Display readings
    if (currentTime - lastDisplay >= DISPLAY_INTERVAL) {

        lastDisplay = currentTime;

        displayStatus();
        publishWaterTelemetry();
    }
}
