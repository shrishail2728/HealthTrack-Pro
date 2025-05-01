#include <WiFi.h>
#include "ThingSpeak.h"
#include <DHT.h>
#include "MAX30105.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "heartRate.h"      // Maxim's heart rate algorithm
#include "spo2_algorithm.h" // Maxim's SpO2 algorithm
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Explicitly define I2C_BUFFER_LENGTH to avoid redefinition warning
#define I2C_BUFFER_LENGTH 128

// ---- DHT22 Config ----
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ---- MAX30102 Config ----
MAX30105 particleSensor;

// ---- LCD Config ----
LiquidCrystal_I2C lcd(0x27, 16, 2); // Change to 0x3F if needed

// ---- AD8232 ECG Config ----
#define ECG_PIN 34 // ADC1_CH6 (GPIO34)
#define LO_PLUS_PIN 32
#define LO_MINUS_PIN 33

// ---- Shared Data Structure ----
struct SensorData
{
  float bpm; // Now stores Maxim BPM
  int32_t spo2;
  int32_t ecgValue;
  float ecgBpm;
  float temperature;
  float humidity;
  bool spo2_valid;
  bool hr_valid;
};

// ---- Global Variables ----
SensorData sensorData = {0, -999, 0, 0, 0, 0, false, false};
SemaphoreHandle_t dataMutex;

// ---- MAX30102 Buffers ----
#define BUFFER_SIZE 100 // Matches Maxim algorithm requirements
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int bufferIndex = 0;
SemaphoreHandle_t bufferMutex;

// ---- AD8232 Heartbeat Variables ----
unsigned long lastEcgBeat = 0;
const int ecgThreshold = 512; // Adjust this threshold as needed

// Wi-Fi configuration
const char *ssid = "mohin";           // Replace with your Wi-Fi SSID
const char *password = "jioairfiber"; // Replace with your Wi-Fi password

// ThingSpeak channel configuration
unsigned long channelID = 2938936;       // Replace with your ThingSpeak channel ID
const char *apiKey = "VLQSMNBV0G4AD7JW"; // Replace with your ThingSpeak write API key

WiFiClient client; // Create a WiFi client to send data to ThingSpeak

// Function prototypes for FreeRTOS tasks
void max30102Task(void *pvParameters);
void dht22Task(void *pvParameters);
void ecgTask(void *pvParameters);
void lcdTask(void *pvParameters);
void thingSpeakTask(void *pvParameters);

// Function to initialize sensors
void initializeSensors() {
  // --- MAX30102 Setup ---
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sensor Error");
    Serial.println("MAX30102 not found. Check wiring.");
    while (1);
  }
  particleSensor.setup(0x3F, 4, 2, 100, 411, 16384);
  particleSensor.setPulseAmplitudeRed(0x3F);
  particleSensor.setPulseAmplitudeIR(0x3F);

  // --- DHT22 Setup ---
  dht.begin();

  // --- AD8232 Setup ---
  pinMode(ECG_PIN, INPUT);
  pinMode(LO_PLUS_PIN, INPUT);
  pinMode(LO_MINUS_PIN, INPUT);
}

// Function to connect to Wi-Fi with timeout
bool connectToWiFi() {
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { // 20 attempts, 1 second each
    delay(1000);
    Serial.println("Connecting to WiFi...");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to WiFi");
    return true;
  } else {
    Serial.println("Failed to connect to WiFi");
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Create mutexes
  dataMutex = xSemaphoreCreateMutex();
  bufferMutex = xSemaphoreCreateMutex();

  // --- LCD Setup ---
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  initializeSensors();
  lcd.clear();

  // --- Wi-Fi Setup ---
  if (!connectToWiFi()) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Error");
    while (1);
  }

  // --- ThingSpeak Setup ---
  ThingSpeak.begin(client);

  // --- Create FreeRTOS Tasks ---
  xTaskCreatePinnedToCore(max30102Task, "MAX30102 Task", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(dht22Task, "DHT22 Task", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(ecgTask, "ECG Task", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(lcdTask, "LCD Task", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(thingSpeakTask, "ThingSpeak Task", 4096, NULL, 1, NULL, 0);
}

void max30102Task(void *pvParameters)
{
  while (1)
  {
    // --- MAX30102 Sensor Readings ---
    uint32_t irValue = particleSensor.getIR();
    uint32_t redValue = particleSensor.getRed();

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      if (irValue < 5000)
      {
        Serial.println("No finger detected (IR < 5000)");
        sensorData.bpm = 0;
        sensorData.spo2 = -999;
        sensorData.spo2_valid = false;
        sensorData.hr_valid = false;
        xSemaphoreGive(dataMutex);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      xSemaphoreGive(dataMutex);
    }

    // Save to buffer
    if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      irBuffer[bufferIndex] = irValue;
      redBuffer[bufferIndex] = redValue;
      bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
      xSemaphoreGive(bufferMutex);
    }

    // --- Maxim's Algorithm for BPM and SpO2 ---
    if (bufferIndex == 0)
    { // Buffer is full, process BPM and SpO2
      int32_t spo2 = -999;
      int8_t spo2_valid = 0;
      int32_t heart_rate = -999;
      int8_t hr_valid = 0;

      if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        maxim_heart_rate_and_oxygen_saturation(irBuffer, BUFFER_SIZE, redBuffer, &spo2, &spo2_valid, &heart_rate, &hr_valid);
        xSemaphoreGive(bufferMutex);
      }

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        sensorData.spo2 = spo2;
        sensorData.spo2_valid = spo2_valid;
        sensorData.hr_valid = hr_valid;
        if (hr_valid)
        {
          sensorData.bpm = heart_rate; // Store Maxim BPM
          Serial.println("=== Heart Rate ===");
          Serial.print("Maxim BPM: ");
          Serial.println(heart_rate);
        }
        else
        {
          sensorData.bpm = 0; // Clear BPM if invalid
          Serial.println("Invalid Maxim BPM");
        }
        Serial.println("=== SpO2 ===");
        if (spo2_valid)
        {
          Serial.print("SpO2 (%): ");
          Serial.println(spo2);
        }
        else
        {
          Serial.println("Invalid SpO2 from Maxim algorithm");
        }
        xSemaphoreGive(dataMutex);
      }
      bufferIndex = 0; // Reset buffer index
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // 100Hz sampling (10ms delay)
  }
}

void dht22Task(void *pvParameters)
{
  while (1)
  {
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      sensorData.temperature = temperature;
      sensorData.humidity = humidity;
      xSemaphoreGive(dataMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(2000)); // Read every 2 seconds
  }
}

void ecgTask(void *pvParameters)
{
  while (1)
  {
    int ecgValue = analogRead(ECG_PIN);
    unsigned long now = millis();

    if (ecgValue > ecgThreshold && (now - lastEcgBeat > 300))
    {
      float newEcgBpm = 60000.0 / (now - lastEcgBeat);
      lastEcgBeat = now;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        sensorData.ecgValue = ecgValue;
        sensorData.ecgBpm = newEcgBpm;
        // Comment out BPM print
        // Serial.print("Heartbeat! BPM (ECG): ");
        // Serial.println(newEcgBpm);
        xSemaphoreGive(dataMutex);
      }
    }

    // Always print raw ECG value for plotting
    Serial.println(ecgValue); // <--- THIS LINE IS CRUCIAL

    vTaskDelay(pdMS_TO_TICKS(10)); // 100Hz sampling
  }
}

void lcdTask(void *pvParameters) {
  static int displayState = 0;
  SensorData lastDisplayedData = {0, -999, 0, 0, 0, 0, false, false};
  while (1) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (memcmp(&sensorData, &lastDisplayedData, sizeof(SensorData)) != 0) { // Only update if data has changed
        lcd.clear();
        lcd.setCursor(0, 0);
        switch (displayState) {
          case 0:
            if (sensorData.hr_valid && sensorData.bpm > 0) {
              lcd.print("BPM: ");
              lcd.print((int)sensorData.bpm);
            } else {
              lcd.print("BPM: N/A");
            }
            break;
          case 1:
            if (sensorData.spo2_valid && sensorData.spo2 >= 0 && sensorData.spo2 <= 100) {
              lcd.print("SpO2: ");
              lcd.print(sensorData.spo2);
              lcd.print("%");
            } else {
              lcd.print("SpO2: N/A");
            }
            break;
          case 2:
            if (!isnan(sensorData.temperature)) {
              lcd.print("Temp: ");
              lcd.print(sensorData.temperature, 1);
              lcd.print((char)223);
              lcd.print("C");
            } else {
              lcd.print("Temp: N/A");
            }
            break;
          case 3:
            if (!isnan(sensorData.humidity)) {
              lcd.print("Hum: ");
              lcd.print(sensorData.humidity, 0);
              lcd.print("%");
            } else {
              lcd.print("Hum: N/A");
            }
            break;
          case 4:
            lcd.print("ECG: ");
            lcd.print(sensorData.ecgValue);
            break;
        }
        lastDisplayedData = sensorData; // Update last displayed data
      }
      xSemaphoreGive(dataMutex);
      displayState = (displayState + 1) % 5;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void thingSpeakTask(void *pvParameters)
{
  while (1)
  {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      // Debug all parameters before sending
      Serial.println("=== ThingSpeak Update ===");
      Serial.print("Maxim BPM before send: ");
      Serial.println(sensorData.bpm);
      Serial.print("SpO2 before send: ");
      Serial.print(sensorData.spo2);
      Serial.print(", Valid: ");
      Serial.println(sensorData.spo2_valid);
      Serial.print("ECG Value: ");
      Serial.println(sensorData.ecgValue);
      Serial.print("Temperature: ");
      Serial.println(sensorData.temperature);
      Serial.print("Humidity: ");
      Serial.println(sensorData.humidity);

      ThingSpeak.setField(1, sensorData.bpm); // Maxim BPM
      if (sensorData.spo2_valid && sensorData.spo2 >= 0 && sensorData.spo2 <= 100)
      {
        ThingSpeak.setField(2, sensorData.spo2);
        Serial.print("Sending SpO2 to ThingSpeak: ");
        Serial.println(sensorData.spo2);
      }
      else
      {
        Serial.println("Skipping SpO2 send: Invalid or out-of-range value");
      }
      ThingSpeak.setField(3, sensorData.ecgValue);
      ThingSpeak.setField(4, sensorData.temperature);
      ThingSpeak.setField(5, sensorData.humidity);

      int responseCode = ThingSpeak.writeFields(channelID, apiKey);
      if (responseCode == 200)
      {
        Serial.println("Data sent to ThingSpeak successfully!");
      }
      else
      {
        Serial.print("Error sending data to ThingSpeak: ");
        Serial.println(responseCode);
      }
      xSemaphoreGive(dataMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(20000)); // Update every 20 seconds
  }
}

void loop()
{
  // Empty loop; all work is done in FreeRTOS tasks
  vTaskDelay(portMAX_DELAY); // Suspend loop task
}