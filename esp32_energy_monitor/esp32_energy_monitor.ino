/*
 * ESP32 Prepaid Energy Meter with MQTT (HiveMQ Cloud)
 * =====================================================
 * 
 * Hardware Connections:
 * - LCD Display (16x2 I2C): SDA → D21, SCL → D22
 * - ACS712 Current Sensor: OUT → D34 (in SERIES with load)
 * - ZMPT101B Voltage Sensor: OUT → D35 (in PARALLEL with mains)
 * - Push Button: D4
 * - Relay: D2 (NC relay - controls load)
 * 
 * Features:
 * - Real-time voltage and current monitoring
 * - Power and energy calculation with moving average filter
 * - MQTT communication with HiveMQ Cloud
 * - Auto-calibration at startup (measures phantom current)
 * - Dynamic calibration via MQTT commands
 * - All calibration values stored in EEPROM
 * - LCD display with button to switch views
 * - Auto relay control on fault detection
 * 
 * CALIBRATION (AUTOMATIC):
 * ========================
 * On first boot, the system auto-calibrates by measuring phantom current.
 * Ensure NO LOAD is connected during first boot!
 * 
 * MQTT Calibration Commands (send to energy/control topic):
 * - {"calibrate": true}           - Run auto-calibration
 * - {"voltageFactor": 500}        - Set voltage calibration factor
 * - {"currentSensitivity": 0.066} - Set ACS712 sensitivity (V/A)
 * - {"currentOffset": 0.2}        - Manually set phantom current offset
 * 
 * ACS712 Sensitivity Values:
 * - 5A model:  0.185 V/A (best for loads < 1000W)
 * - 20A model: 0.100 V/A
 * - 30A model: 0.066 V/A (high noise, best for loads > 60W)
 */

 #include <WiFi.h>
 #include <WiFiClientSecure.h>
 #include <PubSubClient.h>
 #include <Wire.h>
 #include <LiquidCrystal_I2C.h>
 #include <ArduinoJson.h>
 #include <EEPROM.h>
 
 // ============== PIN DEFINITIONS ==============
 #define VOLTAGE_PIN 35      // ZMPT101B connected to GPIO35
 #define CURRENT_PIN 34      // ACS712 connected to GPIO34
 #define RELAY_PIN 2         // Relay connected to GPIO2
 #define BUTTON_PIN 4        // Push button connected to GPIO4
 #define LCD_SDA 21          // I2C SDA for LCD
 #define LCD_SCL 22          // I2C SCL for LCD
 
 // ============== WIFI CREDENTIALS ==============
 const char* ssid = "ITBI";
 const char* password = "WF$it880";
 
 // ============== HIVEMQ CLOUD SETTINGS ==============
 // You'll get these from HiveMQ Cloud console
 const char* mqtt_server = "508ee080475c4e5f9cf0e5c9c3aa6968.s1.eu.hivemq.cloud";  // e.g., "abc123def456.s1.eu.hivemq.cloud"
 const int mqtt_port = 8883;  // TLS port for HiveMQ Cloud
 const char* mqtt_user = "MuhammadJunayed";
 const char* mqtt_password = "junu1st@Anan";
 
 // ============== MQTT TOPICS ==============
 const char* topic_data = "energy/data";           // Publish all sensor data
 const char* topic_status = "energy/status";       // Publish system status
 const char* topic_control = "energy/control";     // Subscribe for commands
 const char* topic_relay = "energy/relay";         // Relay state updates
 
 // ============== LCD SETUP ==============
 // Try 0x27 or 0x3F depending on your LCD module
 LiquidCrystal_I2C lcd(0x27, 16, 2);
 
// ============== SENSOR CALIBRATION (DYNAMIC - INDUSTRY PRACTICE) ==============
// All calibration values stored in EEPROM and adjustable via MQTT
// Default values below - will be loaded from EEPROM if available

float voltageFactor = 500.0;        // ZMPT101B calibration factor
float currentSensitivity = 0.066;   // ACS712 30A module sensitivity (V/A). Update via MQTT if using 5A/20A variant.
float currentOffset = 0.20;         // Phantom current offset (auto-calibrated at startup)

// Moving Average Filter (for stable readings - industry practice)
#define FILTER_SAMPLES 10
float voltageBuffer[FILTER_SAMPLES] = {0};
float currentBuffer[FILTER_SAMPLES] = {0};
int filterIndex = 0;
bool filterReady = false;

// Calibration state
bool isCalibrated = false;
float rawCurrentRMS = 0.0;  // For debugging
 
// ============== THRESHOLD VALUES ==============
float overVoltageThreshold = 260.0;   // Over-voltage protection (V)
float underVoltageThreshold = 110.0;  // Under-voltage protection (V) - Adjusted for Bangladesh voltage fluctuations
float overCurrentThreshold = 1.0;     // Over-current protection (A) - 1A for 40W bulb @ 220V
float costPerKWh = 8.0;               // Cost per kWh in Taka
 
 // ============== MONITORING VARIABLES ==============
 float voltage = 0.0;
 float current = 0.0;
 float power = 0.0;
 float energy = 0.0;
 float balance = 500.0;
 unsigned long lastEnergyUpdateTime = 0;
 bool relayState = true;
 bool faultDetected = false;
 bool manualOverride = false;
 String faultReason = "";
 
 // ============== DISPLAY STATES ==============
 enum DisplayState { 
   DISPLAY_VOLTAGE_CURRENT, 
   DISPLAY_POWER_ENERGY, 
   DISPLAY_STATUS,
   DISPLAY_BALANCE
 };
 DisplayState currentDisplay = DISPLAY_VOLTAGE_CURRENT;
 
 // ============== BUTTON HANDLING ==============
 bool lastButtonState = HIGH;
 unsigned long lastDebounceTime = 0;
 const unsigned long debounceDelay = 50;
 
 // ============== WIFI AND MQTT ==============
 WiFiClientSecure espClient;
 PubSubClient mqttClient(espClient);
 
 // ============== TIMING VARIABLES ==============
 unsigned long lastMqttPublish = 0;
 const long mqttPublishInterval = 2000;  // Publish every 2 seconds (more frequent for testing)
 unsigned long lastSensorRead = 0;
 const long sensorReadInterval = 500;    // Read sensors every 500ms
 unsigned long lastDisplayUpdate = 0;
 const long displayUpdateInterval = 1000; // Update display every 1 second
 
// ============== EEPROM ADDRESSES ==============
#define EEPROM_SIZE 128
#define ADDR_ENERGY 0
#define ADDR_BALANCE 4
#define ADDR_OVER_VOLTAGE 8
#define ADDR_OVER_CURRENT 12
#define ADDR_COST_KWH 16
#define ADDR_INITIALIZED 20
// Calibration values in EEPROM
#define ADDR_VOLTAGE_FACTOR 24
#define ADDR_CURRENT_SENSITIVITY 28
#define ADDR_CURRENT_OFFSET 32
#define ADDR_CALIBRATION_DONE 36
 
 // ============== ROOT CA CERTIFICATE FOR HIVEMQ CLOUD ==============
 // This is the ISRG Root X1 certificate used by HiveMQ Cloud
 static const char* root_ca PROGMEM = R"EOF(
 -----BEGIN CERTIFICATE-----
 MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
 TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
 cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
 WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
 ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
 MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
 h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
 0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
 A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
 T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
 B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
 B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
 KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
 OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
 jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
 qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
 rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
 HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
 hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
 ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
 3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
 NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
 ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
 TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
 jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
 oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
 4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
 mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
 emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
 -----END CERTIFICATE-----
 )EOF";
 
 // ============== SETUP ==============
 void setup() {
   Serial.begin(115200);
   Serial.println("\n\n========================================");
   Serial.println("ESP32 Prepaid Energy Meter");
   Serial.println("Starting up...");
   Serial.println("========================================\n");
   
   // Initialize EEPROM
   EEPROM.begin(EEPROM_SIZE);
   
 // Setup pins
 pinMode(RELAY_PIN, OUTPUT);
 pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Configure ADC for sensor range (ACS712 & ZMPT output swing ≈ 0–3.3V).
  // Default ESP32 attenuation (0 dB) clips everything above ~1.1V, which makes
  // the current reading stick at ~0 A. Set 11 dB so full 3.3V is measurable.
  analogReadResolution(12);
  analogSetPinAttenuation(CURRENT_PIN, ADC_11db);
  analogSetPinAttenuation(VOLTAGE_PIN, ADC_11db);
  
  // IMPORTANT: Relay is Normally Closed (NC)
  // HIGH = relay de-energized = NC contacts closed = load OFF
  // LOW = relay energized = NC contacts open = load ON
  digitalWrite(RELAY_PIN, HIGH);  // Start with load OFF (relay de-energized, NC contacts closed)
  relayState = false;  // Start with relayState FALSE to match dashboard "Power OFF" state
   
   // Initialize LCD
   Wire.begin(LCD_SDA, LCD_SCL);
   lcd.init();
   lcd.backlight();
   lcd.clear();
   lcd.setCursor(0, 0);
   lcd.print("Energy Monitor");
   lcd.setCursor(0, 1);
   lcd.print("Initializing...");
   
   // Load saved settings
   loadSettings();
   
   // Load sensor calibration (or run auto-calibration if first boot)
   loadCalibration();
   
   // Connect to WiFi
   setupWiFi();
   
  // Setup MQTT with TLS
  espClient.setCACert(root_ca);
  espClient.setInsecure();  // Skip certificate verification (for testing only)
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
   
   // Connect to MQTT
   connectMQTT();
   
   // Show ready message
   lcd.clear();
   lcd.setCursor(0, 0);
   lcd.print("System Ready!");
   lcd.setCursor(0, 1);
   lcd.print(WiFi.localIP());
   delay(2000);
   
   lastEnergyUpdateTime = millis();
 }
 
 // ============== MAIN LOOP ==============
 void loop() {
   unsigned long currentMillis = millis();
   
   // Maintain WiFi connection
   if (WiFi.status() != WL_CONNECTED) {
     setupWiFi();
   }
   
   // Maintain MQTT connection
   if (!mqttClient.connected()) {
     connectMQTT();
   }
   mqttClient.loop();
   
   // Read sensors at intervals
   if (currentMillis - lastSensorRead >= sensorReadInterval) {
     lastSensorRead = currentMillis;
     readSensors();
     checkThresholds();
   }
   
   // Update display at intervals
   if (currentMillis - lastDisplayUpdate >= displayUpdateInterval) {
     lastDisplayUpdate = currentMillis;
     updateDisplay();
   }
   
   // Publish to MQTT at intervals
   if (currentMillis - lastMqttPublish >= mqttPublishInterval) {
     lastMqttPublish = currentMillis;
     publishData();
     
     // Deduct balance based on energy consumed
     deductBalance();
   }
   
   // Handle button press
   handleButton();
   
   // Save settings periodically (every 5 minutes)
   static unsigned long lastSave = 0;
   if (currentMillis - lastSave >= 300000) {
     lastSave = currentMillis;
     saveSettings();
   }
 }
 
 // ============== WIFI SETUP ==============
 void setupWiFi() {
   Serial.print("Connecting to WiFi: ");
   Serial.println(ssid);
   
   lcd.clear();
   lcd.setCursor(0, 0);
   lcd.print("Connecting WiFi");
   lcd.setCursor(0, 1);
   
   WiFi.mode(WIFI_STA);
   WiFi.begin(ssid, password);
   
   int attempts = 0;
   while (WiFi.status() != WL_CONNECTED && attempts < 30) {
     delay(500);
     Serial.print(".");
     lcd.print(".");
     attempts++;
   }
   
   if (WiFi.status() == WL_CONNECTED) {
     Serial.println("\nWiFi Connected!");
     Serial.print("IP Address: ");
     Serial.println(WiFi.localIP());
     
     lcd.clear();
     lcd.setCursor(0, 0);
     lcd.print("WiFi Connected!");
     lcd.setCursor(0, 1);
     lcd.print(WiFi.localIP());
     delay(1500);
   } else {
     Serial.println("\nWiFi Failed!");
     lcd.clear();
     lcd.setCursor(0, 0);
     lcd.print("WiFi Failed!");
     lcd.setCursor(0, 1);
     lcd.print("Check Settings");
     delay(2000);
   }
 }
 
 // ============== MQTT CONNECTION ==============
 void connectMQTT() {
   int attempts = 0;
   
   while (!mqttClient.connected() && attempts < 3) {
     Serial.print("Connecting to MQTT...");
     
     lcd.clear();
     lcd.setCursor(0, 0);
     lcd.print("MQTT Connecting");
     
     String clientId = "ESP32_EnergyMeter_";
     clientId += String(random(0xffff), HEX);
     
     if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
       Serial.println("Connected!");
       
       lcd.setCursor(0, 1);
       lcd.print("Connected!");
       
       // Subscribe to control topic
       mqttClient.subscribe(topic_control);
       Serial.println("Subscribed to: " + String(topic_control));
       
       // Publish online status
       StaticJsonDocument<128> statusDoc;
       statusDoc["status"] = "online";
       statusDoc["ip"] = WiFi.localIP().toString();
       statusDoc["rssi"] = WiFi.RSSI();
       
       String statusStr;
       serializeJson(statusDoc, statusStr);
       mqttClient.publish(topic_status, statusStr.c_str(), true);
       
       delay(1000);
     } else {
       Serial.print("Failed, rc=");
       Serial.print(mqttClient.state());
       Serial.println(" Retrying...");
       
       lcd.setCursor(0, 1);
       lcd.print("Failed! Retry...");
       
       delay(3000);
     }
     attempts++;
   }
 }
 
 // ============== MQTT CALLBACK ==============
 void mqttCallback(char* topic, byte* payload, unsigned int length) {
   Serial.print("Message on [");
   Serial.print(topic);
   Serial.print("]: ");
   
   String message;
   for (int i = 0; i < length; i++) {
     message += (char)payload[i];
   }
   Serial.println(message);
   
   // Parse JSON command
   StaticJsonDocument<256> doc;
   DeserializationError error = deserializeJson(doc, message);
   
   if (error) {
     Serial.println("JSON parse error!");
     return;
   }
   
   // Handle relay control
   if (doc.containsKey("relay")) {
     bool newState = doc["relay"].as<bool>();
     setRelay(newState, true);  // Manual control
   }
   
   // Handle threshold updates
   if (doc.containsKey("overVoltage")) {
     overVoltageThreshold = doc["overVoltage"].as<float>();
     Serial.println("Updated overVoltage: " + String(overVoltageThreshold));
   }
   
   if (doc.containsKey("overCurrent")) {
     overCurrentThreshold = doc["overCurrent"].as<float>();
     Serial.println("Updated overCurrent: " + String(overCurrentThreshold));
   }
   
   if (doc.containsKey("underVoltage")) {
     underVoltageThreshold = doc["underVoltage"].as<float>();
     Serial.println("Updated underVoltage: " + String(underVoltageThreshold));
   }
   
   if (doc.containsKey("costPerKWh")) {
     costPerKWh = doc["costPerKWh"].as<float>();
     Serial.println("Updated costPerKWh: " + String(costPerKWh));
   }
   
   // Handle balance recharge
   if (doc.containsKey("recharge")) {
     float amount = doc["recharge"].as<float>();
     balance += amount;
     Serial.println("Balance recharged: +" + String(amount) + " = " + String(balance));
   }
   
   // Handle energy reset
   if (doc.containsKey("resetEnergy") && doc["resetEnergy"].as<bool>()) {
     energy = 0.0;
     Serial.println("Energy reset to 0");
   }
   
   // Handle settings save
   if (doc.containsKey("save") && doc["save"].as<bool>()) {
     saveSettings();
   }
   
   // Handle factory reset
   if (doc.containsKey("factoryReset") && doc["factoryReset"].as<bool>()) {
     factoryReset();
   }
   
   // ============== CALIBRATION COMMANDS ==============
   // Run auto-calibration
   if (doc.containsKey("calibrate") && doc["calibrate"].as<bool>()) {
     calibrateSensors();
   }
   
   // Update voltage calibration factor
   if (doc.containsKey("voltageFactor")) {
     voltageFactor = doc["voltageFactor"].as<float>();
     EEPROM.writeFloat(ADDR_VOLTAGE_FACTOR, voltageFactor);
     EEPROM.commit();
     Serial.println("Updated voltageFactor: " + String(voltageFactor));
   }
   
   // Update current sensitivity
   if (doc.containsKey("currentSensitivity")) {
     currentSensitivity = doc["currentSensitivity"].as<float>();
     EEPROM.writeFloat(ADDR_CURRENT_SENSITIVITY, currentSensitivity);
     EEPROM.commit();
     Serial.println("Updated currentSensitivity: " + String(currentSensitivity, 4));
   }
   
   // Manually set current offset
   if (doc.containsKey("currentOffset")) {
     currentOffset = doc["currentOffset"].as<float>();
     EEPROM.writeFloat(ADDR_CURRENT_OFFSET, currentOffset);
     EEPROM.writeByte(ADDR_CALIBRATION_DONE, 0xBB);
     EEPROM.commit();
     Serial.println("Updated currentOffset: " + String(currentOffset, 3));
   }
}
 
// ============== READ SENSORS (INDUSTRY-GRADE WITH FILTERING) ==============
void readSensors() {
  const int numSamples = 500;  // Samples for RMS calculation
  
  float voltageSumSquared = 0;
  float currentSumSquared = 0;
  
  // Measure DC offset dynamically
  float voltageOffsetSum = 0;
  float currentOffsetSum = 0;
  
  for (int i = 0; i < 100; i++) {
    voltageOffsetSum += (analogRead(VOLTAGE_PIN) / 4095.0) * 3.3;
    currentOffsetSum += (analogRead(CURRENT_PIN) / 4095.0) * 3.3;
    delayMicroseconds(100);
  }
  
  float voltageOffset = voltageOffsetSum / 100.0;
  float currentADCOffset = currentOffsetSum / 100.0;
  
  // Sample AC waveform for RMS calculation
  for (int i = 0; i < numSamples; i++) {
    float vReading = (analogRead(VOLTAGE_PIN) / 4095.0) * 3.3;
    float iReading = (analogRead(CURRENT_PIN) / 4095.0) * 3.3;
    
    float vInst = vReading - voltageOffset;
    float iInst = iReading - currentADCOffset;
    
    voltageSumSquared += vInst * vInst;
    currentSumSquared += iInst * iInst;
    
    delayMicroseconds(100);
  }
  
  // Calculate RMS values
  float voltageRMS = sqrt(voltageSumSquared / numSamples);
  rawCurrentRMS = sqrt(currentSumSquared / numSamples);
  
  // Apply calibration factors (stored in EEPROM, adjustable via MQTT)
  float rawVoltage = voltageRMS * voltageFactor;
  float rawCurrent = rawCurrentRMS / currentSensitivity;
  
  // Subtract phantom current offset (calibrated at startup)
  rawCurrent = rawCurrent - currentOffset;
  if (rawCurrent < 0) rawCurrent = 0;
  
  // ============== MOVING AVERAGE FILTER ==============
  // Provides stable readings by averaging last N samples
  voltageBuffer[filterIndex] = rawVoltage;
  currentBuffer[filterIndex] = rawCurrent;
  filterIndex = (filterIndex + 1) % FILTER_SAMPLES;
  
  if (filterIndex == 0) filterReady = true;
  
  // Calculate filtered values
  float voltageSum = 0;
  float currentSum = 0;
  int samples = filterReady ? FILTER_SAMPLES : (filterIndex + 1);
  
  for (int i = 0; i < samples; i++) {
    voltageSum += voltageBuffer[i];
    currentSum += currentBuffer[i];
  }
  
  voltage = voltageSum / samples;
  current = currentSum / samples;
  
  // ============== NOISE FLOOR FILTERING ==============
  if (voltage < 50) voltage = 0;  // No mains power
  
  // Minimum detectable current (based on sensor noise)
  // For 30A module: ~0.05A after offset subtraction
  if (current < 0.03) current = 0;
  
  // When relay is OFF, force current to zero
  if (!relayState) {
    if (rawCurrent > 0.5) {
      Serial.println("WARNING: Significant current while relay OFF - possible theft!");
    }
    current = 0;
  }
  
  // No voltage = no current
  if (voltage == 0) current = 0;
  
  // Calculate power (Watts)
  power = voltage * current;
  
  // Calculate energy (kWh)
  unsigned long now = millis();
  if (lastEnergyUpdateTime > 0 && relayState && power > 0) {
    float hoursPassed = (now - lastEnergyUpdateTime) / 3600000.0;
    energy += (power * hoursPassed) / 1000.0;
  }
  lastEnergyUpdateTime = now;
  
  // Debug output
  Serial.printf("V: %.1fV | I: %.3fA (raw: %.3f, offset: %.3f) | P: %.1fW\n", 
                voltage, current, rawCurrent + currentOffset, currentOffset, power);
}

// ============== AUTO-CALIBRATION FUNCTION ==============
void calibrateSensors() {
  Serial.println("\n========== AUTO-CALIBRATION ==========");
  Serial.println("Measuring phantom current offset...");
  Serial.println("Ensure NO LOAD is connected or relay is OFF");
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Calibrating...");
  lcd.setCursor(0, 1);
  lcd.print("Please wait");
  
  // Take multiple readings to find phantom current
  float totalPhantom = 0;
  const int calibrationSamples = 20;
  
  for (int s = 0; s < calibrationSamples; s++) {
    float currentSumSquared = 0;
    float currentOffsetSum = 0;
    
    // Measure DC offset
    for (int i = 0; i < 100; i++) {
      currentOffsetSum += (analogRead(CURRENT_PIN) / 4095.0) * 3.3;
      delayMicroseconds(100);
    }
    float adcOffset = currentOffsetSum / 100.0;
    
    // Measure RMS
    for (int i = 0; i < 500; i++) {
      float iReading = (analogRead(CURRENT_PIN) / 4095.0) * 3.3;
      float iInst = iReading - adcOffset;
      currentSumSquared += iInst * iInst;
      delayMicroseconds(100);
    }
    
    float rms = sqrt(currentSumSquared / 500.0);
    float phantom = rms / currentSensitivity;
    totalPhantom += phantom;
    
    Serial.printf("  Sample %d: %.3fA\n", s + 1, phantom);
    delay(100);
  }
  
  // Set offset as average phantom current
  currentOffset = totalPhantom / calibrationSamples;
  
  Serial.printf("\n>> Phantom current offset: %.3fA\n", currentOffset);
  Serial.println(">> Calibration complete!\n");
  
  // Save to EEPROM
  EEPROM.writeFloat(ADDR_CURRENT_OFFSET, currentOffset);
  EEPROM.writeByte(ADDR_CALIBRATION_DONE, 0xBB);
  EEPROM.commit();
  
  isCalibrated = true;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Calibrated!");
  lcd.setCursor(0, 1);
  lcd.print("Offset: ");
  lcd.print(currentOffset, 2);
  lcd.print("A");
  delay(2000);
}

// ============== LOAD CALIBRATION FROM EEPROM ==============
void loadCalibration() {
  if (EEPROM.readByte(ADDR_CALIBRATION_DONE) == 0xBB) {
    currentOffset = EEPROM.readFloat(ADDR_CURRENT_OFFSET);
    
    // Validate
    if (isnan(currentOffset) || currentOffset < 0 || currentOffset > 1.0) {
      currentOffset = 0.2;  // Default for 30A module
    }
    
    // Load voltage factor if saved
    float savedVoltageFactor = EEPROM.readFloat(ADDR_VOLTAGE_FACTOR);
    if (!isnan(savedVoltageFactor) && savedVoltageFactor > 100 && savedVoltageFactor < 1000) {
      voltageFactor = savedVoltageFactor;
    }
    
  // Load current sensitivity if saved
  float savedSensitivity = EEPROM.readFloat(ADDR_CURRENT_SENSITIVITY);
  if (!isnan(savedSensitivity) && savedSensitivity > 0.04 && savedSensitivity < 0.25) {
    currentSensitivity = savedSensitivity; // honour stored value for the module in use
  }
    
    isCalibrated = true;
    Serial.println("Calibration loaded from EEPROM:");
    Serial.printf("  Voltage Factor: %.1f\n", voltageFactor);
    Serial.printf("  Current Sensitivity: %.4f V/A\n", currentSensitivity);
    Serial.printf("  Current Offset: %.3fA\n", currentOffset);
  } else {
    Serial.println("No calibration found, running auto-calibration...");
    calibrateSensors();
  }
}
 
 // ============== CHECK THRESHOLDS ==============
 void checkThresholds() {
   bool previousFault = faultDetected;
   faultDetected = false;
   faultReason = "";
   
   // Check over-voltage
   if (voltage > overVoltageThreshold) {
     faultDetected = true;
     faultReason = "Over Voltage!";
     Serial.println("FAULT: Over Voltage - " + String(voltage) + "V");
   }
   
   // Check under-voltage (but only if there's some voltage)
   if (voltage > 50 && voltage < underVoltageThreshold) {
     faultDetected = true;
     faultReason = "Under Voltage!";
     Serial.println("FAULT: Under Voltage - " + String(voltage) + "V");
   }
   
   // Check over-current
   if (current > overCurrentThreshold) {
     faultDetected = true;
     faultReason = "Over Current!";
     Serial.println("FAULT: Over Current - " + String(current) + "A");
   }
   
   // Check low balance
   if (balance <= 0) {
     faultDetected = true;
     faultReason = "Low Balance!";
     Serial.println("FAULT: Low Balance - " + String(balance) + " Taka");
   }
   
   // Auto-control relay (if not manually overridden)
   if (!manualOverride) {
     if (faultDetected && relayState) {
       setRelay(false, false);
       Serial.println("Auto: Relay turned OFF due to fault");
     } else if (!faultDetected && !relayState && balance > 0) {
       setRelay(true, false);
       Serial.println("Auto: Relay turned ON - fault cleared");
     }
   }
   
   // Publish fault status if changed
   if (faultDetected != previousFault) {
     publishStatus();
   }
 }
 
// ============== SET RELAY ==============
void setRelay(bool state, bool isManual) {
  relayState = state;
  manualOverride = isManual;
  
  // CRITICAL: Relay is Normally Closed (NC)
  // To turn load ON: set pin LOW (relay energized, NC contacts open)
  // To turn load OFF: set pin HIGH (relay de-energized, NC contacts closed)
  digitalWrite(RELAY_PIN, relayState ? LOW : HIGH);
  
  Serial.println("Relay: " + String(relayState ? "ON" : "OFF") + 
                 " (Manual: " + String(manualOverride ? "Yes" : "No") + ")");
   
   // Publish relay state
   StaticJsonDocument<128> relayDoc;
   relayDoc["state"] = relayState;
   relayDoc["manual"] = manualOverride;
   relayDoc["timestamp"] = millis();
   
   String relayStr;
   serializeJson(relayDoc, relayStr);
   mqttClient.publish(topic_relay, relayStr.c_str());
 }
 
 // ============== PUBLISH DATA ==============
 void publishData() {
   if (!mqttClient.connected()) return;
   
   StaticJsonDocument<384> doc;
   
  doc["voltage"] = round(voltage * 10) / 10.0;
  doc["current"] = round(current * 1000) / 1000.0;
  doc["power"] = round(power / 1000.0 * 1000) / 1000.0;  // Convert W to kW (e.g., 54W -> 0.054kW)
  doc["energy"] = round(energy * 1000) / 1000.0;
   doc["balance"] = round(balance * 100) / 100.0;
   doc["relayState"] = relayState;
   doc["faultDetected"] = faultDetected;
   doc["faultReason"] = faultReason;
   doc["manualOverride"] = manualOverride;
   doc["overVoltageThreshold"] = overVoltageThreshold;
   doc["underVoltageThreshold"] = underVoltageThreshold;
   doc["overCurrentThreshold"] = overCurrentThreshold;
   doc["costPerKWh"] = costPerKWh;
   doc["rssi"] = WiFi.RSSI();
   doc["uptime"] = millis() / 1000;
   
   String jsonStr;
   serializeJson(doc, jsonStr);
   
   bool published = mqttClient.publish(topic_data, jsonStr.c_str());
   
   if (published) {
     Serial.println("Published to MQTT: " + jsonStr);
   } else {
     Serial.println("MQTT publish failed!");
   }
 }
 
 // ============== PUBLISH STATUS ==============
 void publishStatus() {
   if (!mqttClient.connected()) return;
   
   StaticJsonDocument<256> doc;
   doc["status"] = faultDetected ? "fault" : "ok";
   doc["faultReason"] = faultReason;
   doc["relayState"] = relayState;
   doc["voltage"] = voltage;
   doc["current"] = current;
   doc["balance"] = balance;
   doc["timestamp"] = millis();
   
   String jsonStr;
   serializeJson(doc, jsonStr);
   mqttClient.publish(topic_status, jsonStr.c_str());
 }
 
 // ============== DEDUCT BALANCE ==============
 void deductBalance() {
   // Calculate cost for energy consumed since last update
   static float lastEnergy = 0;
   float energyUsed = energy - lastEnergy;
   
   if (energyUsed > 0 && relayState) {
     float cost = energyUsed * costPerKWh;
     balance -= cost;
     
     if (balance < 0) balance = 0;
     
     Serial.printf("Deducted: %.4f Taka (%.6f kWh @ %.2f/kWh)\n", 
                   cost, energyUsed, costPerKWh);
   }
   
   lastEnergy = energy;
 }
 
 // ============== HANDLE BUTTON ==============
 void handleButton() {
   bool reading = digitalRead(BUTTON_PIN);
   
   if (reading != lastButtonState) {
     lastDebounceTime = millis();
   }
   
   if ((millis() - lastDebounceTime) > debounceDelay) {
     static bool buttonPressed = false;
     
     if (reading == LOW && !buttonPressed) {
       buttonPressed = true;
       
       // Cycle through display states
       currentDisplay = static_cast<DisplayState>((currentDisplay + 1) % 4);
       updateDisplay();
       
       Serial.println("Button pressed - Display mode: " + String(currentDisplay));
     }
     
     if (reading == HIGH) {
       buttonPressed = false;
     }
   }
   
   lastButtonState = reading;
 }
 
 // ============== UPDATE DISPLAY ==============
 void updateDisplay() {
   lcd.clear();
   
   switch (currentDisplay) {
     case DISPLAY_VOLTAGE_CURRENT:
       lcd.setCursor(0, 0);
       lcd.print("V: ");
       lcd.print(voltage, 1);
       lcd.print(" V");
       
       lcd.setCursor(0, 1);
       lcd.print("I: ");
       lcd.print(current, 3);
       lcd.print(" A");
       break;
       
     case DISPLAY_POWER_ENERGY:
       lcd.setCursor(0, 0);
       lcd.print("P: ");
       lcd.print(power, 1);
       lcd.print(" W");
       
       lcd.setCursor(0, 1);
       lcd.print("E: ");
       lcd.print(energy, 3);
       lcd.print(" kWh");
       break;
       
     case DISPLAY_STATUS:
       lcd.setCursor(0, 0);
       lcd.print("Relay: ");
       lcd.print(relayState ? "ON " : "OFF");
       
       lcd.setCursor(0, 1);
       if (faultDetected) {
         lcd.print(faultReason);
       } else {
         lcd.print("Status: OK");
       }
       break;
       
     case DISPLAY_BALANCE:
       lcd.setCursor(0, 0);
       lcd.print("Balance:");
       
       lcd.setCursor(0, 1);
       lcd.print(balance, 2);
       lcd.print(" Taka");
       break;
   }
 }
 
 // ============== SAVE SETTINGS ==============
 void saveSettings() {
   EEPROM.writeFloat(ADDR_ENERGY, energy);
   EEPROM.writeFloat(ADDR_BALANCE, balance);
   EEPROM.writeFloat(ADDR_OVER_VOLTAGE, overVoltageThreshold);
   EEPROM.writeFloat(ADDR_OVER_CURRENT, overCurrentThreshold);
   EEPROM.writeFloat(ADDR_COST_KWH, costPerKWh);
   EEPROM.writeByte(ADDR_INITIALIZED, 0xAA);  // Magic byte to indicate valid data
   EEPROM.commit();
   
   Serial.println("Settings saved to EEPROM");
 }
 
 // ============== LOAD SETTINGS ==============
 void loadSettings() {
   // Check if EEPROM has valid data
   if (EEPROM.readByte(ADDR_INITIALIZED) == 0xAA) {
     energy = EEPROM.readFloat(ADDR_ENERGY);
     balance = EEPROM.readFloat(ADDR_BALANCE);
     overVoltageThreshold = EEPROM.readFloat(ADDR_OVER_VOLTAGE);
     overCurrentThreshold = EEPROM.readFloat(ADDR_OVER_CURRENT);
     costPerKWh = EEPROM.readFloat(ADDR_COST_KWH);
     
     // Validate loaded values
     if (isnan(energy) || energy < 0) energy = 0.0;
     if (isnan(balance) || balance < 0) balance = 500.0;
     if (isnan(overVoltageThreshold) || overVoltageThreshold < 200) overVoltageThreshold = 260.0;
     if (isnan(overCurrentThreshold) || overCurrentThreshold < 1) overCurrentThreshold = 10.0;
     if (isnan(costPerKWh) || costPerKWh < 0) costPerKWh = 8.0;
     
     Serial.println("Settings loaded from EEPROM:");
     Serial.println("  Energy: " + String(energy) + " kWh");
     Serial.println("  Balance: " + String(balance) + " Taka");
     Serial.println("  Over Voltage: " + String(overVoltageThreshold) + " V");
     Serial.println("  Over Current: " + String(overCurrentThreshold) + " A");
     Serial.println("  Cost/kWh: " + String(costPerKWh) + " Taka");
   } else {
     Serial.println("No saved settings found, using defaults");
   }
 }
 
 // ============== FACTORY RESET ==============
 void factoryReset() {
   energy = 0.0;
   balance = 500.0;
   overVoltageThreshold = 260.0;
   underVoltageThreshold = 160.0;
   overCurrentThreshold = 10.0;
   costPerKWh = 8.0;
   relayState = true;
   manualOverride = false;
   faultDetected = false;
   
   digitalWrite(RELAY_PIN, HIGH);
   
   saveSettings();
   
   Serial.println("Factory reset completed!");
   
   lcd.clear();
   lcd.setCursor(0, 0);
   lcd.print("Factory Reset");
   lcd.setCursor(0, 1);
   lcd.print("Complete!");
   delay(2000);
 }
 
