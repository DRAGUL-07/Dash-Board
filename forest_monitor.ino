/*
 * ============================================================================
 *  FOREST MONITORING SYSTEM — ESP32 FIRMWARE
 *  Detects possible illegal tree-cutting activity using vibration, IMU and
 *  PIR sensors, runs an on-device TFLite Micro model, and reports results
 *  to a backend dashboard over Wi-Fi (REST API).
 * ============================================================================
 *
 *  >>> READ THIS FIRST <<<
 *  This file compiles and runs "as-is" in DEMO/HEURISTIC mode (no ML model
 *  needed) so you can test the full sensor -> WiFi -> backend -> dashboard
 *  pipeline immediately. To switch to real TFLite Micro inference:
 *    1. Set USE_TFLITE_MODEL to 1 below.
 *    2. Drop your model.h (see esp32/model/README.md) into esp32/model/.
 *    3. Fill in MODEL_INPUT_FEATURES / MODEL_OUTPUT_CLASSES to match your
 *       actual trained model (search "REPLACE ME" in this file).
 *
 *  Every place you MUST edit is marked:  // >>> REPLACE ME
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <time.h>

// ============================================================================
// SECTION 0: MODE SWITCH
// ============================================================================
// 0 = Heuristic/threshold mode (works out of the box, no model needed)
// 1 = TFLite Micro mode (requires model.h — see esp32/model/README.md)
#define USE_TFLITE_MODEL 0

// ============================================================================
// SECTION 1: USER CONFIGURATION — >>> REPLACE ME
// ============================================================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";       // >>> REPLACE ME
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";   // >>> REPLACE ME

// Backend server (see backend/app.py). Use your PC's LAN IP, not "localhost",
// since the ESP32 is a separate device on the network.
const char* SERVER_URL = "http://192.168.1.100:5000/api/data"; // >>> REPLACE ME

const char* DEVICE_ID = "ESP32_01";                  // >>> REPLACE ME if you have multiple nodes

// GPIO PIN ASSIGNMENTS — >>> REPLACE ME to match your wiring
const int PIN_VIBRATION_SENSOR = 34;   // Analog or digital vibration sensor (e.g. SW-420 -> digital, 801S -> analog)
const int PIN_PIR_MOTION       = 27;   // PIR motion sensor digital output
// IMU (MPU6050 or similar) uses I2C — default ESP32 I2C pins:
const int PIN_I2C_SDA = 21;            // >>> REPLACE ME if different
const int PIN_I2C_SCL = 22;            // >>> REPLACE ME if different
const uint8_t IMU_I2C_ADDR = 0x68;     // >>> REPLACE ME (0x68 default MPU6050, 0x69 if AD0 pulled high)

// Whether the vibration sensor is analog (0-4095) or digital (0/1)
#define VIBRATION_SENSOR_IS_ANALOG 1   // >>> REPLACE ME: set 0 for a purely digital vibration switch

// Sampling / reporting interval
const unsigned long SENSOR_READ_INTERVAL_MS = 200;   // how often raw samples are taken
const unsigned long REPORT_INTERVAL_MS      = 2000;  // how often a result is sent to backend

// NTP for timestamps
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 0;        // >>> REPLACE ME with your timezone offset in seconds if needed
const int   DAYLIGHT_OFFSET_SEC = 0;

// ============================================================================
// SECTION 2: ML MODEL CONFIGURATION — >>> REPLACE ME
// ============================================================================
// Do NOT trust these numbers as-is — they are placeholders. Fill them in to
// match exactly what your model was trained with (see esp32/model/README.md
// and the "ML MODEL" section of the project README for the full explanation).

#define MODEL_INPUT_FEATURE_COUNT 6   // >>> REPLACE ME — e.g. [vibration, accel_x, accel_y, accel_z, gyro_mag, motion]
#define MODEL_NUM_CLASSES         3   // >>> REPLACE ME — number of output classes

// Output class labels IN THE EXACT ORDER your model was trained to output them.
// >>> REPLACE ME
const char* MODEL_CLASS_LABELS[MODEL_NUM_CLASSES] = {
  "NORMAL",
  "SUSPICIOUS_ACTIVITY",
  "POSSIBLE_TREE_CUTTING"
};

// Feature normalization — >>> REPLACE ME to match your training preprocessing
// exactly (e.g. if you did (x - mean) / std during training, put mean/std here;
// if you did min-max scaling, put min/max here and adjust normalizeFeature()).
struct FeatureNormParams {
  float mean;
  float stdDev;
};
// >>> REPLACE ME — one entry per feature, same order as buildFeatureVector()
FeatureNormParams NORM_PARAMS[MODEL_INPUT_FEATURE_COUNT] = {
  {0.0f, 1.0f}, // vibration
  {0.0f, 1.0f}, // accel_x
  {0.0f, 1.0f}, // accel_y
  {0.0f, 1.0f}, // accel_z
  {0.0f, 1.0f}, // gyro / motion-derived feature
  {0.0f, 1.0f}  // pir motion (0/1)
};

#if USE_TFLITE_MODEL
  // Requires: TensorFlowLite_ESP32 library installed via Library Manager
  // and your converted model as a C array in esp32/model/model.h
  // (see esp32/model/README.md for the xxd/tflite-micro conversion steps).
  #include "model/model.h"   // >>> REPLACE ME — must define: const unsigned char g_model[]; const int g_model_len;
  #include <TensorFlowLite_ESP32.h>
  #include "tensorflow/lite/micro/all_ops_resolver.h"
  #include "tensorflow/lite/micro/micro_error_reporter.h"
  #include "tensorflow/lite/micro/micro_interpreter.h"
  #include "tensorflow/lite/schema/schema_generated.h"

  namespace {
    tflite::ErrorReporter* error_reporter = nullptr;
    const tflite::Model* tfl_model = nullptr;
    tflite::MicroInterpreter* interpreter = nullptr;
    TfLiteTensor* model_input = nullptr;
    TfLiteTensor* model_output = nullptr;

    // >>> REPLACE ME — increase if you get "AllocateTensors failed" errors
    constexpr int kTensorArenaSize = 20 * 1024;
    uint8_t tensor_arena[kTensorArenaSize];
  }
#endif

// ============================================================================
// SECTION 3: SENSOR STATE
// ============================================================================
struct SensorReading {
  float vibration;
  float accelX, accelY, accelZ;
  float gyroX, gyroY, gyroZ;
  int   motion; // 0 or 1
};

struct DetectionResult {
  String label;
  float  confidence;
};

unsigned long lastSensorReadMs = 0;
unsigned long lastReportMs = 0;
SensorReading latestReading = {0, 0, 0, 0, 0, 0, 0, 0};

// ============================================================================
// SECTION 4: WI-FI HANDLING (with auto-reconnect)
// ============================================================================
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to '%s' ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed to connect within timeout. Will retry in loop().");
  }
}

void ensureWiFiConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected. Reconnecting...");
    connectWiFi();
  }
}

// ============================================================================
// SECTION 5: SENSOR READING + PREPROCESSING
// ============================================================================

// --- IMU: minimal MPU6050-style raw register read (no external library needed) ---
// If your IMU needs a dedicated library (e.g. Adafruit_MPU6050, MPU9250_asukiaaa),
// >>> REPLACE ME: swap this block for that library's read calls instead.
bool readIMU(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  Wire.beginTransmission(IMU_I2C_ADDR);
  Wire.write(0x3B); // ACCEL_XOUT_H register (MPU6050 convention)
  if (Wire.endTransmission(false) != 0) return false;

  Wire.requestFrom((int)IMU_I2C_ADDR, 14, true);
  if (Wire.available() < 14) return false;

  int16_t rawAx = (Wire.read() << 8) | Wire.read();
  int16_t rawAy = (Wire.read() << 8) | Wire.read();
  int16_t rawAz = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read(); // skip temperature registers
  int16_t rawGx = (Wire.read() << 8) | Wire.read();
  int16_t rawGy = (Wire.read() << 8) | Wire.read();
  int16_t rawGz = (Wire.read() << 8) | Wire.read();

  // Default MPU6050 sensitivity: +/-2g accel, +/-250deg/s gyro
  // >>> REPLACE ME if you configured a different full-scale range
  ax = rawAx / 16384.0f;
  ay = rawAy / 16384.0f;
  az = rawAz / 16384.0f;
  gx = rawGx / 131.0f;
  gy = rawGy / 131.0f;
  gz = rawGz / 131.0f;
  return true;
}

void initIMU() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  // Wake up MPU6050 (it starts in sleep mode)
  Wire.beginTransmission(IMU_I2C_ADDR);
  Wire.write(0x6B); // PWR_MGMT_1
  Wire.write(0x00);
  Wire.endTransmission(true);
  Serial.println("[IMU] Initialized.");
}

float readVibration() {
#if VIBRATION_SENSOR_IS_ANALOG
  int raw = analogRead(PIN_VIBRATION_SENSOR);   // 0-4095 on ESP32
  return raw / 4095.0f;                          // normalize to 0..1
#else
  return digitalRead(PIN_VIBRATION_SENSOR) == HIGH ? 1.0f : 0.0f;
#endif
}

int readMotion() {
  return digitalRead(PIN_PIR_MOTION) == HIGH ? 1 : 0;
}

// Simple exponential moving average filter to smooth noisy sensor data.
// >>> REPLACE ME if your training pipeline used a different filter (e.g. a
// moving-average window, low-pass Butterworth, or none at all).
float emaFilter(float previous, float newSample, float alpha = 0.3f) {
  return alpha * newSample + (1.0f - alpha) * previous;
}

SensorReading readAllSensors() {
  static SensorReading filtered = {0, 0, 0, 1.0, 0, 0, 0, 0}; // az defaults to 1g resting

  float rawVibration = readVibration();
  float ax, ay, az, gx, gy, gz;
  bool imuOk = readIMU(ax, ay, az, gx, gy, gz);
  int motion = readMotion();

  filtered.vibration = emaFilter(filtered.vibration, rawVibration);
  if (imuOk) {
    filtered.accelX = emaFilter(filtered.accelX, ax);
    filtered.accelY = emaFilter(filtered.accelY, ay);
    filtered.accelZ = emaFilter(filtered.accelZ, az);
    filtered.gyroX  = emaFilter(filtered.gyroX, gx);
    filtered.gyroY  = emaFilter(filtered.gyroY, gy);
    filtered.gyroZ  = emaFilter(filtered.gyroZ, gz);
  } else {
    Serial.println("[IMU] Read failed — using last known values.");
  }
  filtered.motion = motion; // discrete, no smoothing

  return filtered;
}

// ============================================================================
// SECTION 6: FEATURE VECTOR + NORMALIZATION (must mirror training pipeline)
// ============================================================================
// >>> REPLACE ME: the feature order/composition here MUST exactly match what
// your model was trained on. This is a reasonable default guess only.
void buildFeatureVector(const SensorReading &r, float outFeatures[MODEL_INPUT_FEATURE_COUNT]) {
  float gyroMag = sqrtf(r.gyroX * r.gyroX + r.gyroY * r.gyroY + r.gyroZ * r.gyroZ);

  outFeatures[0] = r.vibration;
  outFeatures[1] = r.accelX;
  outFeatures[2] = r.accelY;
  outFeatures[3] = r.accelZ;
  outFeatures[4] = gyroMag;
  outFeatures[5] = (float)r.motion;
}

float normalizeFeature(float value, int featureIndex) {
  const FeatureNormParams &p = NORM_PARAMS[featureIndex];
  if (p.stdDev == 0) return value;
  return (value - p.mean) / p.stdDev;
}

// ============================================================================
// SECTION 7: ML INFERENCE
// ============================================================================
#if USE_TFLITE_MODEL

void initTFLiteModel() {
  static tflite::MicroErrorReporter micro_error_reporter;
  error_reporter = &micro_error_reporter;

  tfl_model = tflite::GetModel(g_model);
  if (tfl_model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("[TFLite] Model schema version mismatch!");
    return;
  }

  static tflite::AllOpsResolver resolver; // >>> REPLACE ME: swap for a MicroMutableOpsResolver
                                           // with only the ops your model needs, to save flash/RAM
  static tflite::MicroInterpreter static_interpreter(
      tfl_model, resolver, tensor_arena, kTensorArenaSize, error_reporter);
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("[TFLite] AllocateTensors() failed — increase kTensorArenaSize.");
    return;
  }

  model_input = interpreter->input(0);
  model_output = interpreter->output(0);
  Serial.println("[TFLite] Model initialized.");
}

DetectionResult runInference(const SensorReading &r) {
  float features[MODEL_INPUT_FEATURE_COUNT];
  buildFeatureVector(r, features);

  for (int i = 0; i < MODEL_INPUT_FEATURE_COUNT; i++) {
    // >>> REPLACE ME: if your model expects int8 quantized input instead of
    // float32, use model_input->params.scale / zero_point to quantize here.
    model_input->data.f[i] = normalizeFeature(features[i], i);
  }

  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("[TFLite] Invoke failed.");
    return {"ERROR", 0.0f};
  }

  int bestIdx = 0;
  float bestScore = model_output->data.f[0];
  for (int i = 1; i < MODEL_NUM_CLASSES; i++) {
    if (model_output->data.f[i] > bestScore) {
      bestScore = model_output->data.f[i];
      bestIdx = i;
    }
  }

  return {String(MODEL_CLASS_LABELS[bestIdx]), bestScore};
}

#else // ---- HEURISTIC / DEMO MODE (no model file required) ----

// This threshold-based stand-in lets you test the whole pipeline before your
// TFLite model is ready. It is NOT a substitute for the real model.
// >>> REPLACE ME: delete this whole function once USE_TFLITE_MODEL = 1
DetectionResult runInference(const SensorReading &r) {
  float features[MODEL_INPUT_FEATURE_COUNT];
  buildFeatureVector(r, features);
  float gyroMag = features[4];

  bool highVibration = r.vibration > 0.55f;
  bool highMotionEnergy = gyroMag > 40.0f || fabs(r.accelX) > 0.5f || fabs(r.accelY) > 0.5f;

  if (highVibration && r.motion == 1 && highMotionEnergy) {
    float confidence = 0.75f + 0.20f * min(1.0f, (r.vibration - 0.55f) / 0.45f);
    return {"POSSIBLE_TREE_CUTTING", min(confidence, 0.99f)};
  } else if (highVibration || (r.motion == 1 && highMotionEnergy)) {
    return {"SUSPICIOUS_ACTIVITY", 0.55f + 0.2f * r.vibration};
  } else {
    return {"NORMAL", 0.90f};
  }
}

#endif

// ============================================================================
// SECTION 8: REPORTING TO BACKEND (REST API over Wi-Fi)
// ============================================================================
String getTimestampISO8601() {
  time_t now;
  time(&now);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

void sendResultToBackend(const SensorReading &r, const DetectionResult &result) {
  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Report] Skipped — no WiFi.");
    return;
  }

  HTTPClient http;
  http.begin(SERVER_URL);            // >>> REPLACE ME if backend needs headers/auth
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"timestamp\":\"" + getTimestampISO8601() + "\",";
  payload += "\"vibration\":" + String(r.vibration, 4) + ",";
  payload += "\"accel_x\":" + String(r.accelX, 4) + ",";
  payload += "\"accel_y\":" + String(r.accelY, 4) + ",";
  payload += "\"accel_z\":" + String(r.accelZ, 4) + ",";
  payload += "\"motion\":" + String(r.motion) + ",";
  payload += "\"prediction\":\"" + result.label + "\",";
  payload += "\"confidence\":" + String(result.confidence, 4);
  payload += "}";

  Serial.println("[Report] POST -> " + String(SERVER_URL));
  Serial.println("[Report] Payload: " + payload);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.printf("[Report] HTTP %d: %s\n", httpCode, http.getString().c_str());
  } else {
    Serial.printf("[Report] POST failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// ============================================================================
// SECTION 9: SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Forest Monitoring System — ESP32 booting ===");

  pinMode(PIN_PIR_MOTION, INPUT);
#if !VIBRATION_SENSOR_IS_ANALOG
  pinMode(PIN_VIBRATION_SENSOR, INPUT);
#endif

  initIMU();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.println("[Time] NTP sync requested.");
  }

#if USE_TFLITE_MODEL
  initTFLiteModel();
#else
  Serial.println("[ML] Running in HEURISTIC/DEMO mode (USE_TFLITE_MODEL = 0).");
#endif

  Serial.println("=== Setup complete ===\n");
}

void loop() {
  unsigned long now = millis();

  if (now - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadMs = now;
    latestReading = readAllSensors();
  }

  if (now - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = now;

    DetectionResult result = runInference(latestReading);

    Serial.printf(
      "[Sensors] vib=%.3f accel=(%.2f,%.2f,%.2f) motion=%d  =>  [ML] %s (%.1f%%)\n",
      latestReading.vibration, latestReading.accelX, latestReading.accelY,
      latestReading.accelZ, latestReading.motion,
      result.label.c_str(), result.confidence * 100.0f
    );

    sendResultToBackend(latestReading, result);
  }

  ensureWiFiConnected();
}
