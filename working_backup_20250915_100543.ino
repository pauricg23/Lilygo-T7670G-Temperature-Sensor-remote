#include <HardwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

HardwareSerial Modem(1);

// T-A7670G pin definitions
#define BOARD_POWERON_PIN 12
#define MODEM_PWR_PIN 4
#define MODEM_RESET_PIN 5
#define MODEM_DTR_PIN 25
#define MODEM_RING_PIN 33

// I2C pins for OLED display
#define SDA_PIN 33
#define SCL_PIN 32

// Temperature sensor pin (DS18B20 on IO14)
#define ONE_WIRE_BUS 14

// Server configuration
const char* serverUrl = "http://housetemp.pauricgrant.com/submit";
const char* apn = "simbase";

// Temperature sensor setup
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Display setup - OLED 128x64
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Sleep duration (1 hour = 3,600,000,000 microseconds)
#define SLEEP_DURATION_US 3600000000ULL

// Display scrolling variables
String displayLines[8]; // Store up to 8 lines
int currentLine = 0;

void addDisplayLine(String text) {
  // Remove emojis and clean up text
  text.replace("✅", "[OK]");
  text.replace("❌", "[ERR]");
  text.replace("⚠️", "[!]");
  
  // If we already have 8 lines, shift everything up first
  if (currentLine >= 8) {
    for (int i = 0; i < 7; i++) {
      displayLines[i] = displayLines[i + 1];
    }
    // Add new line at the end
    displayLines[7] = text;
  } else {
    // Add new line to array
    displayLines[currentLine] = text;
    currentLine++;
  }
  
  // Clear and redraw all lines
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Always show 8 lines (or currentLine if less than 8)
  int linesToShow = (currentLine > 8) ? 8 : currentLine;
  for (int i = 0; i < linesToShow; i++) {
    display.setCursor(0, i * 8); // 8 pixels per line
    display.println(displayLines[i]);
  }
  
  display.display();
  delay(300); // Shorter pause for smoother scrolling
}

void clearDisplayLines() {
  // Clear the display lines array
  for (int i = 0; i < 8; i++) {
    displayLines[i] = "";
  }
  currentLine = 0;
}

String sendATCommand(const String& command, unsigned long timeout) {
  Serial.println(">> " + command);
  Modem.println(command);
  String response = "";
  unsigned long startTime = millis();
  while (millis() - startTime < timeout) {
    if (Modem.available()) {
      char c = Modem.read();
      response += c;
      Serial.write(c);
    }
  }
  Serial.println("<< " + response);
  Serial.println("Response length: " + String(response.length()));
  
  // Check for common success indicators
  if (response.indexOf("OK") >= 0) {
    Serial.println("✅ Command successful");
  } else if (response.indexOf("ERROR") >= 0) {
    Serial.println("❌ Command failed");
  } else if (response.length() == 0) {
    Serial.println("⚠️ No response (timeout)");
  } else {
    Serial.println("⚠️ Unknown response");
  }
  
  return response;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Check wake-up reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.printf("Wakeup reason: %d\n", wakeup_reason);
  
  Serial.println("\n=== T-A7670G Production Temperature Monitor ===");
  Serial.println("Reading sensors and sending data every hour via LTE");
  
  // Initialize display FIRST
  initDisplay();
  Serial.println("Display initialized");
  
  // Show startup message on display
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("T-A7670G Monitor");
  display.setCursor(0, 15);
  display.println("Starting up...");
  display.display();
  
  // Enable board peripherals
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);
  delay(1000);

  // Initialize modem control pins
  pinMode(MODEM_PWR_PIN, OUTPUT);
  pinMode(MODEM_RESET_PIN, OUTPUT);
  pinMode(MODEM_DTR_PIN, OUTPUT);
  pinMode(MODEM_RING_PIN, INPUT);

  // Power modem with proper boot time
  digitalWrite(MODEM_PWR_PIN, HIGH);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  digitalWrite(MODEM_DTR_PIN, HIGH);
  delay(10000);  // Give modem 10s to wake up fully

  // Start UART
  Modem.begin(115200, SERIAL_8N1, 27, 26);
  delay(1000);

  // Initialize temperature sensors
  sensors.begin();
  Serial.println("Temperature sensors initialized");
  
  // Clear display for scrolling at the very beginning
  clearDisplayLines();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();
  
  // Add first line
  addDisplayLine("Starting up...");
  
  // Setup LTE with timeout
  unsigned long lteStartTime = millis();
  setupLTE();
  
  // If LTE setup takes too long, skip and go to sleep
  if (millis() - lteStartTime > 30000) { // 30 second timeout
    Serial.println("❌ LTE setup timeout - going to sleep");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("LTE timeout");
    display.setCursor(0, 15);
    display.println("Going to sleep...");
    display.display();
  } else {
    readAndSendData();
  }
  
  // Final display message
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Data sent!");
  display.setCursor(0, 15);
  display.println("Going to sleep...");
  display.display();
  
  Serial.println("Going to sleep for 1 hour...");
  
  // Shut down peripherals
  powerDownModem();
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  
  Serial.flush();
  delay(200);
  
  // Enter deep sleep
  esp_deep_sleep(SLEEP_DURATION_US);
}

void initDisplay() {
  // Initialize I2C with correct pins
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("❌ Display initialization failed on address 0x3C");
    Serial.println("Trying alternative I2C address 0x3D...");
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("❌ Display not found on I2C bus");
      return;
    }
  }
  
  Serial.println("✅ Display initialized successfully");
  
  // Clear display
  display.clearDisplay();
  display.display();
}

void setupLTE() {
  Serial.println("\n=== Setting up LTE Connection ===");
  
  // Add first line
  addDisplayLine("Testing modem...");
  
  // Basic AT commands with error handling
  if (!sendATCommand("AT", 2000).indexOf("OK") > 0) {
    Serial.println("❌ Modem not responding to AT command");
    addDisplayLine("❌ Modem error");
    return;
  }
  
  addDisplayLine("✅ Modem OK");
  addDisplayLine("Checking SIM...");
  
  if (!sendATCommand("AT+CPIN?", 3000).indexOf("READY") > 0) {
    Serial.println("❌ SIM card not ready");
    addDisplayLine("❌ SIM not ready");
    return;
  }
  
  addDisplayLine("✅ SIM ready");
  addDisplayLine("Configuring LTE...");
  
  // Configure for LTE with shorter timeouts
  sendATCommand("AT+CNMP=38", 3000); // LTE only
  sendATCommand("AT+CMNB=1", 3000);  // LTE mode
  delay(1000);
  
  addDisplayLine("✅ LTE configured");
  addDisplayLine("Checking signal...");
  
  // Check signal
  String signalResponse = sendATCommand("AT+CSQ", 3000);
  Serial.println("Signal response: " + signalResponse);
  
  String regResponse = sendATCommand("AT+CREG?", 3000);
  Serial.println("Registration response: " + regResponse);
  
  addDisplayLine("✅ Signal OK");
  addDisplayLine("Setting up APN...");
  
  // Setup APN
  sendATCommand("AT+CGDCONT=1,\"IP\",\"" + String(apn) + "\"", 3000);
  
  addDisplayLine("✅ APN set");
  addDisplayLine("Activating data...");
  
  // Activate PDP context with timeout
  String pdpResponse = sendATCommand("AT+CGACT=1,1", 5000);
  if (pdpResponse.indexOf("OK") > 0) {
    Serial.println("✅ LTE setup complete");
    addDisplayLine("✅ LTE connected!");
  } else {
    Serial.println("❌ PDP context activation failed");
    addDisplayLine("❌ LTE failed");
  }
}

void readAndSendData() {
  Serial.println("\n=== Reading Sensors and Sending Data ===");
  
  addDisplayLine("Reading sensors...");
  
  // Read temperature sensors
  sensors.requestTemperatures();
  delay(1000);
  
  float t1 = sensors.getTempCByIndex(0);
  float t2 = sensors.getTempCByIndex(1);
  float t3 = sensors.getTempCByIndex(2);
  
  Serial.println("T1: " + String(t1, 2) + "°C");
  Serial.println("T2: " + String(t2, 2) + "°C");
  Serial.println("T3: " + String(t3, 2) + "°C");
  
  // Get current time
  String timestamp = getCurrentTime();
  
  // Create JSON payload
  String payload = "{\"time\":\"" + timestamp.substring(11, 16) + "\",\"ts\":\"" + timestamp + "\",\"t1\":" + String(t1, 2) + ",\"t2\":" + String(t2, 2) + ",\"t3\":" + String(t3, 2) + "}";
  
  Serial.println("Payload: " + payload);
  
  addDisplayLine("Sending data...");
  
  // Send HTTP POST
  sendHTTPPost(payload);
}

void sendHTTPPost(String payload) {
  Serial.println("\n=== Sending HTTP POST ===");
  
  // Start HTTP session
  sendATCommand("AT+HTTPINIT", 5000);
  sendATCommand("AT+HTTPPARA=\"URL\",\"" + String(serverUrl) + "\"", 5000);
  sendATCommand("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 5000);
  
  // Set data length
  sendATCommand("AT+HTTPDATA=" + String(payload.length()) + ",10000", 5000);
  
  // Send payload
  Modem.print(payload);
  delay(2000);
  
  // Execute HTTP action
  String response = sendATCommand("AT+HTTPACTION=1", 15000);
  
  // Check response
  if (response.indexOf("200") > 0) {
    Serial.println("✅ Data sent successfully!");
    addDisplayLine("✅ Data sent!");
  } else {
    Serial.println("❌ HTTP POST failed");
    addDisplayLine("❌ Send failed");
  }
  
  // Terminate HTTP session
  sendATCommand("AT+HTTPTERM", 5000);
}

String getCurrentTime() {
  // Try to get time from network
  String response = sendATCommand("AT+CCLK?", 5000);
  
  if (response.indexOf("+CCLK:") > 0) {
    // Extract time from response
    int start = response.indexOf("\"") + 1;
    int end = response.indexOf("\"", start);
    if (start > 0 && end > start) {
      String timeStr = response.substring(start, end);
      // Convert from YY/MM/DD,HH:MM:SS to YYYY-MM-DDTHH:MM:SS
      String year = "20" + timeStr.substring(0, 2);
      String month = timeStr.substring(3, 5);
      String day = timeStr.substring(6, 8);
      // Convert UTC to local time (Ireland is UTC+1)
      String timePart = timeStr.substring(9);
      int hour = timePart.substring(0, 2).toInt();
      int minute = timePart.substring(3, 5).toInt();
      int second = timePart.substring(6, 8).toInt();
      
      // Add 1 hour for Ireland timezone
      hour = (hour + 1) % 24;
      
      // Format back to HH:MM:SS
      String localTime = String(hour < 10 ? "0" : "") + String(hour) + ":" + 
                        String(minute < 10 ? "0" : "") + String(minute) + ":" + 
                        String(second < 10 ? "0" : "") + String(second);
      
      return year + "-" + month + "-" + day + "T" + localTime;
    }
  }
  
  // Fallback timestamp
  return "2025-09-04T12:00";
}

void powerDownModem() {
  Serial.println("Powering down modem...");
  
  // Try to gracefully shut down modem
  String response = sendATCommand("AT+CPWROFF", 2000);
  if (response.indexOf("OK") == -1) {
    Serial.println("⚠️ Graceful shutdown failed, forcing power off");
  }
  
  delay(1000);
  
  // Force power off regardless of AT command response
  digitalWrite(MODEM_PWR_PIN, LOW);
  digitalWrite(MODEM_RESET_PIN, LOW);
  digitalWrite(MODEM_DTR_PIN, LOW);
  
  Serial.println("✅ Modem powered down");
}

void loop() {
  // This should never be reached due to deep sleep
  Serial.println("Unexpected wake from deep sleep - going back to sleep");
  esp_deep_sleep(SLEEP_DURATION_US);
}