
#include <Arduino.h>

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
    payload += "\"timestamp\":null,\"timestamp_ms\":" + String(millis()) + ",\"metrics\":{";

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
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

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
}
