#include <HardwareSerial.h>
#include <driver/rtc_io.h>
#include <Preferences.h>

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

HardwareSerial Modem(1);
Preferences prefs;

// Power state structure for modem-based battery monitoring
struct PowerState { 
  bool usb, charging, full; 
  int percent; 
  float vbat; 
};

// FreeRTOS task globals for background LTE setup
volatile bool lteReady = false;
TaskHandle_t lteTaskHandle = NULL;
SemaphoreHandle_t atMux;
volatile bool modemReadyForCBC = false;

// T-A7670G pin definitions
#define BOARD_POWERON_PIN 12
#define MODEM_PWR_PIN 4
#define MODEM_RESET_PIN 5
#define MODEM_DTR_PIN 25
#define MODEM_RING_PIN 34  // Changed from 33 to avoid conflict with I2C SDA

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

// RTC memory for first run detection and sleep intervals
RTC_DATA_ATTR bool isFirstRun = true;
RTC_DATA_ATTR bool probeModeCompleted = false; // Track if probe mode has ever been completed
RTC_DATA_ATTR uint32_t sleep_secs = 3600; // Default 1 hour
RTC_DATA_ATTR bool psmTried = false;
RTC_DATA_ATTR uint32_t boot_count = 0;
RTC_DATA_ATTR int lastPct = -1;

// --- 1) Voltage -> % mapping (piecewise, Li-ion OCV) ---
int lipoPercentFromVoltage(float v) {
  struct P { float v; int p; } t[] = {
    {3.30f,  0}, {3.50f,  5}, {3.60f, 20}, {3.70f, 45},
    {3.80f, 60}, {3.90f, 75}, {4.00f, 85}, {4.10f, 92},
    {4.20f,100}
  };
  if (v <= t[0].v) return 0;
  if (v >= t[8].v) return 100;
  for (int i=0;i<8;i++) if (v >= t[i].v && v <= t[i+1].v) {
    float u = (v - t[i].v) / (t[i+1].v - t[i].v);
    return (int)(t[i].p + u * (t[i+1].p - t[i].p) + 0.5f);
  }
  return 0;
}

// --- 2) Helpers: median-of-5 and gentle hysteresis ---
template<typename T>
T median5(T a, T b, T c, T d, T e) {
  T v[5] = {a,b,c,d,e};
  for (int i=0;i<5;i++) for (int j=i+1;j<5;j++) if (v[j]<v[i]) { T t=v[i]; v[i]=v[j]; v[j]=t; }
  return v[2];
}

int smoothPercent(int now) {
  if (lastPct < 0) { lastPct = now; return now; }
  if (now > lastPct + 4) now = lastPct + 4;   // max 4% step per wake
  if (now < lastPct - 4) now = lastPct - 4;
  lastPct = now;
  return now;
}

// NVS-based probe mode tracking
bool probeAlreadyDone() {
  prefs.begin("tmprobe", false);
  bool done = prefs.getBool("done", false);
  prefs.end();
  return done;
}

void markProbeDone() {
  prefs.begin("tmprobe", false);
  prefs.putBool("done", true);
  prefs.end();
}

// Battery monitoring - now handled entirely by modem via AT+CBC

// Helper to wait for modem to be ready for CBC commands
bool waitForCBCReady(uint32_t ms=2000) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    String s = sendATCommand("AT", 300);
    if (s.indexOf("OK") >= 0) {
      String p = sendATCommand("AT+CPIN?", 800);
      if (p.indexOf("READY") >= 0) return true;
    }
    delay(100);
  }
  return false;
}

// Background LTE connection task
void lteConnectTask(void*){
  // All AT commands happen here until we're done
  lteReady = setupLTE();   // your existing, blocking bring-up
  vTaskDelete(NULL);
}

// --- 3) Read modem power state using CBC (ignore CBC %) ---
PowerState readPowerState() {
  Serial.println("*** readPowerState() called - NEW VOLTAGE-BASED METHOD ***");
  PowerState ps{};
  
  // First check if modem is responding
  String test = sendATCommand("AT", 500);
  if (test.indexOf("OK") == -1) {
    Serial.println("Modem not ready for CBC, returning empty PowerState");
    return ps; // Return empty state if modem not ready
  }
  
  // take 5 quick samples for a stable voltage
  int mv[5] = {0,0,0,0,0};
  int st[5] = {0,0,0,0,0};
  for (int i=0;i<5;i++) {
    String r = sendATCommand("AT+CBC", 600);  // modem must be on
    int k = r.indexOf("+CBC:");
    if (k >= 0) {
      int c1 = r.indexOf(',', k);
      int c2 = r.indexOf(',', c1+1);
      if (c1>0 && c2>c1) {
        st[i] = r.substring(k+6, c1).toInt();     // 0=disch, 1=charging, 2=full
        mv[i] = r.substring(c2+1).toInt();        // millivolts
      }
    }
    delay(100);
  }
  int mvmid = median5(mv[0],mv[1],mv[2],mv[3],mv[4]);
  int stmid = median5(st[0],st[1],st[2],st[3],st[4]);

  // Only process if we got valid data
  if (mvmid > 0) {
    ps.vbat = mvmid / 1000.0f;

    // compensate a bit: charging raises terminal voltage above OCV
    bool charging = (stmid == 1);
    if (charging) ps.vbat -= 0.12f;   // subtract ~120 mV when on cable

    // derive percent from (approx) OCV, clamp
    int pct = lipoPercentFromVoltage(ps.vbat);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;

    ps.percent  = smoothPercent(pct);
    ps.charging = charging;
    ps.full     = (stmid == 2) || (!charging && ps.vbat >= 4.18f);
    ps.usb      = ps.charging || ps.full;

    // Debug so you can see raw vs derived:
    Serial.printf("CBC median: %dmV, state=%d -> vbat_adj=%.2fV, pct=%d%%\n",
                  mvmid, stmid, ps.vbat, ps.percent);
  } else {
    Serial.println("No valid CBC data received");
  }
  
  return ps;
}

// --- 4) Use this in your display (and anywhere else) ---
void displayBatteryStatus() {
  Serial.println("displayBatteryStatus() called");

  if (!modemReadyForCBC) {
    // Give it a quick chance to be ready; otherwise show placeholder
    if (!waitForCBCReady(1500)) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print("Battery: --%");
      display.display();
      Serial.println("Modem not ready, showing --%");
      return;
    }
    modemReadyForCBC = true;
  }

  PowerState ps = readPowerState();
  Serial.printf("PowerState: vbat=%.2f, percent=%d, usb=%s, charging=%s\n", 
                ps.vbat, ps.percent, ps.usb ? "true" : "false", ps.charging ? "true" : "false");
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  if (ps.vbat > 0) {
    // Modem is ready, use robust voltage-based percentage - TWO LINE LAYOUT
    display.setCursor(0, 0);
    display.print(ps.usb ? (ps.charging ? "Charging " : "Full ") : "Battery ");
    display.print(ps.percent);
    display.print("%");
    
    display.setCursor(0, 12);              // next line
    display.print(ps.vbat, 2);
    display.print("V");
    
    Serial.printf("Displaying: %s %d%% %.2fV (two-line layout)\n", 
                  ps.usb ? (ps.charging ? "Charging" : "Full") : "Battery", 
                  ps.percent, ps.vbat);
  } else {
    // Modem not ready - show placeholder
    display.setCursor(0, 0);
    display.print("Battery: --%");
    Serial.println("Displaying: Battery: --% (modem not ready)");
  }
  
  display.display();
}

void displayProbeMode() {
  display.clearDisplay();
  
  // Full screen for sensor data (no header)
  display.fillRect(0, 0, 128, 64, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  
  // Battery placeholder (don't query modem here)
  display.setCursor(2, 2);
  display.print("Bat: --%");
  
  // Ultra-high-sensitivity temperature readings (12-bit resolution)
  sensors.setResolution(12); // Maximum sensitivity: 0.0625°C resolution
  sensors.requestTemperatures();
  delay(750); // Wait for 12-bit conversion (750ms max)
  
  float t1 = sensors.getTempCByIndex(0);
  float t2 = sensors.getTempCByIndex(1);
  float t3 = sensors.getTempCByIndex(2);
  
  display.setCursor(2, 14);
  display.print("T1: ");
  display.print(t1, 4); // 4 decimal places for ultra-high precision
  display.print("C");
  
  display.setCursor(2, 26);
  display.print("T2: ");
  display.print(t2, 4);
  display.print("C");
  
  display.setCursor(2, 38);
  display.print("T3: ");
  display.print(t3, 4);
  display.print("C");
  
  display.display();
}

void displayGoingToSleep() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 25);
  display.print("Going to sleep...");
  display.display();
  delay(1000); // Brief display before turning off
}

void enterDeepSleepSeconds(uint32_t secs) {
  // Turn off OLED, stop I2C
  display.clearDisplay(); 
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  Wire.end();
  pinMode(SDA_PIN, INPUT);     // not PULLDOWN (saves leakage with external pullups)
  pinMode(SCL_PIN, INPUT);
  pinMode(ONE_WIRE_BUS, INPUT);
  rtc_gpio_isolate((gpio_num_t)MODEM_RING_PIN);  // isolate RTC-only pin

  // DO NOT drive/hold strap pins during deep sleep:
  // - Leave BOARD_POWERON_PIN (GPIO12) HIGH or INPUT, and do NOT gpio_hold_en it.
  // - Do NOT hold GPIO4/5 either (they're also strap pins).
  digitalWrite(BOARD_POWERON_PIN, HIGH);   // keep rails up (safe for wake)
  // gpio_hold_en((gpio_num_t)BOARD_POWERON_PIN);   // <-- REMOVED (GPIO12 is strap pin)
  // gpio_hold_en((gpio_num_t)MODEM_PWR_PIN);       // <-- REMOVED (GPIO4 is strap pin)
  // gpio_hold_en((gpio_num_t)MODEM_RESET_PIN);     // <-- REMOVED (GPIO5 is strap pin)
  // You can keep DTR held HIGH if you want (GPIO25 is RTC-capable and not a strap)
  // gpio_hold_en((gpio_num_t)MODEM_DTR_PIN);      // optional; not necessary

  // Lowest-power ESP domains (timer still works without RTC_PERIPH)
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);

  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  esp_deep_sleep_start();
}

bool enablePSM() {
  // Ask for EPS reg with PSM fields, then enable PSM
  sendATCommand("AT+CEREG=4", 800);
  String r = sendATCommand("AT+CPSMS=1", 1200);    // network-defined timers
  // If unsupported you'll get ERROR; still okay to continue
  return (r.indexOf("OK") >= 0);
}

void modemToLowPower() {
  // Let modem idle into PSM (DO NOT use CFUN=0 - it causes detach!)
  digitalWrite(MODEM_DTR_PIN, HIGH);  // allow sleep on many SIMComs
  // PSM will handle the power saving without detaching
}

void modemHardOff() {
  // Fallback if you really want it dead between wakes
  sendATCommand("AT+CPOWD=1", 2000);  // graceful power down (some firmwares)
  sendATCommand("AT+CPOF", 1000);     // alt command on others (ignore ERRORs)
  delay(500);
  // Keep control lines defined during ESP deep sleep
  digitalWrite(MODEM_PWR_PIN, LOW);
  digitalWrite(MODEM_RESET_PIN, LOW);
  digitalWrite(MODEM_DTR_PIN, HIGH);
}

void powerDownModem() {
  Serial.println("Powering down modem...");
  
  // Try PSM once after first successful attach (it persists)
  if (!psmTried) { 
    enablePSM(); 
    psmTried = true; 
  }
  modemToLowPower();   // CFUN=0 + DTR=HIGH
}

String sendATCommand(const String& cmd, uint32_t timeout){
  xSemaphoreTake(atMux, portMAX_DELAY);
  Serial.println(">> " + cmd);
  Modem.println(cmd);
  String r; 
  uint32_t t = millis();
  while(millis() - t < timeout){ 
    while(Modem.available()) r += (char)Modem.read(); 
    delay(3); 
  }
  Serial.println("<< " + r);
  xSemaphoreGive(atMux);
  return r;
}

String readModemUntil(const String& token, uint32_t timeout_ms) {
  String buf;
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    while (Modem.available()) buf += char(Modem.read());
    if (token.length() && buf.indexOf(token) >= 0) break;
    delay(5);
  }
  if (buf.length()) Serial.println("URC<<< " + buf);
  return buf;
}

bool setupLTE() {
  Serial.println("Setting up LTE connection...");

  // Power on modem
  digitalWrite(MODEM_PWR_PIN, HIGH);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  digitalWrite(MODEM_DTR_PIN, HIGH);
  delay(10000); // Give modem time to boot

  // Initialize serial communication
  Modem.begin(115200, SERIAL_8N1, 27, 26);
  delay(2000);
  
  // Ensure full functionality before attach
  sendATCommand("AT+CFUN=1", 3000);
  String cfunResponse = sendATCommand("AT+CFUN?", 1000);
  Serial.println("CFUN status: " + cfunResponse);
  
  // Test modem communication
  String response = sendATCommand("AT", 2000);
  if (response.indexOf("OK") == -1) {
    Serial.println("❌ Modem not responding");
    return false;
  }
  
  // Check SIM status
  response = sendATCommand("AT+CPIN?", 5000);
  if (response.indexOf("READY") == -1) {
    Serial.println("❌ SIM not ready");
    return false;
  }
  
  // Check signal strength
  response = sendATCommand("AT+CSQ", 5000);
  if (response.indexOf("+CSQ:") == -1) {
    Serial.println("❌ No signal");
    return false;
  }
  
  // Configure network settings - Cat-M only (preferred for simbase)
  sendATCommand("AT+CNMP=38", 3000); // LTE only
  sendATCommand("AT+CMNB=2", 3000);  // Cat-M only (no NB-IoT)
  
  // Set APN to simbase
  sendATCommand("AT+CGDCONT=1,\"IP\",\"simbase\"", 3000);
  String apnResponse = sendATCommand("AT+CGDCONT?", 2000);
  Serial.println("APN config: " + apnResponse);
  
  // Check network registration - prefer EPS (LTE) registration
  sendATCommand("AT+CEREG=2", 1000); // verbose URCs helpful for debug
  String r = sendATCommand("AT+CEREG?", 1500);
  // More precise EPS registration check
  bool epsReg = false;
  int statPos = r.indexOf("+CEREG:");
  if (statPos >= 0) {
    int firstComma = r.indexOf(',', statPos);
    int secondComma = r.indexOf(',', firstComma+1);
    int stat = r.substring(firstComma+1, secondComma).toInt();
    epsReg = (stat == 1 || stat == 5);
  }
  Serial.println("EPS Registration: " + r);

  // Fallback to CS (2G/3G) reg, and accept LTE "CS not available"
  String r2 = sendATCommand("AT+CREG?", 1500);
  bool csReg = (r2.indexOf("+CREG: 0,1") >= 0 || r2.indexOf("+CREG: 0,5") >= 0 || r2.indexOf("+CREG: 0,7") >= 0);
  Serial.println("CS Registration: " + r2);

  // As a final sanity check, confirm PS attach
  String att = sendATCommand("AT+CGATT?", 1500);
  bool psAttached = (att.indexOf("+CGATT: 1") >= 0);
  Serial.println("PS Attach: " + att);

  if (!(epsReg || csReg || psAttached)) {
    Serial.println("❌ Not registered/attached (CEREG/CREG/CGATT)");
    return false;
  }
  
  Serial.println("✅ Network registration OK");
  
  // Set network mode to LTE only (as in working backup)
  sendATCommand("AT+CNMP=38", 5000);
  sendATCommand("AT+CMNB=1", 5000);
  
  // Configure PDP context
  sendATCommand("AT+CGDCONT=1,\"IP\",\"" + String(apn) + "\"", 5000);
  
  // Ensure PS attach and activate PDP context
  sendATCommand("AT+CGATT=1", 3000);
  response = sendATCommand("AT+CGACT=1,1", 6000);
  if (response.indexOf("OK") == -1) {
    Serial.println("❌ PDP activation failed");
    return false;
  }
  
  // Confirm IP address
  String ip = sendATCommand("AT+CGPADDR=1", 1500);
  if (ip.indexOf('.') < 0) { 
    Serial.println("❌ No IP on CID 1");
    return false; 
  }
  Serial.println("✅ IP Address confirmed: " + ip);
  
  Serial.println("✅ LTE connection established");
  modemReadyForCBC = true;  // Modem is now ready for battery commands
  return true;
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

bool httpPostJson(const String& url, const String& json) {
  Serial.println("\n=== Sending HTTP POST ===");
  unsigned long httpStart = millis();
  
  // Ensure PDP is up and CID 1 has IP
  String r = sendATCommand("AT+CGACT?", 2000);
  if (r.indexOf("1,1") < 0) {
    Serial.println("❌ PDP context not active");
    return false;
  }
  
  r = sendATCommand("AT+CGPADDR=1", 2000);
  Serial.println("IP Address: " + r);

  sendATCommand("AT+HTTPTERM", 500);           // clean slate (ignore errors)
  sendATCommand("AT+HTTPINIT", 2000);
  // no CID/REDIR/HTTPSSL here for plain http:// (removes ERROR responses)
  sendATCommand("AT+HTTPPARA=\"URL\",\"" + url + "\"", 3000);
  sendATCommand("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 500);

  // Some firmwares need explicit DNS servers if hostname fails:
  // sendATCommand("AT+CDNSCFG=\"8.8.8.8\",\"1.1.1.1\"", 1000);

  // Prepare upload — WAIT for DOWNLOAD
  r = sendATCommand("AT+HTTPDATA=" + String(json.length()) + ",8000", 3000);
  if (r.indexOf("DOWNLOAD") < 0) {
    Serial.println("❌ No DOWNLOAD prompt, aborting");
    sendATCommand("AT+HTTPTERM", 1000);
    return false;
  }
  
  Serial.println("✅ Got DOWNLOAD prompt, sending JSON data...");
  
  // Send exact bytes; do NOT send extra CR/LF here
  Modem.write((const uint8_t*)json.c_str(), json.length());

  // Some firmwares echo an OK after the data window closes — give it a moment
  readModemUntil("OK", 3000);

  // Start POST and then wait for the URC
  Serial.println("Starting HTTP POST request...");
  sendATCommand("AT+HTTPACTION=1", 1000);
  String urc = readModemUntil("+HTTPACTION:", 30000); // Increased timeout to 30 seconds

  // Check if we got a response
  if (urc.indexOf("+HTTPACTION:") < 0) {
    Serial.println("❌ HTTP POST timeout - no response received");
    sendATCommand("AT+HTTPTERM", 1000);
    return false;
  }

  // Parse HTTP code
  int p = urc.indexOf("+HTTPACTION:");
  int comma1 = urc.indexOf(',', p);
  int comma2 = urc.indexOf(',', comma1 + 1);
  int httpCode = (comma1 > 0 && comma2 > comma1) ? urc.substring(comma1 + 1, comma2).toInt() : -1;
  Serial.printf("HTTP code: %d\n", httpCode);
  

  // Optional: read body (not required)
  // sendATCommand("AT+HTTPREAD", 5000);

  sendATCommand("AT+HTTPTERM", 1000);
  
  unsigned long httpDuration = millis() - httpStart;
  Serial.printf("HTTP POST completed in %lu ms\n", httpDuration);
  
  return (httpCode == 200 || httpCode == 201 || httpCode == 204);
}

void sendHTTPPost(String payload) {
  bool ok = httpPostJson(String(serverUrl), payload);
  if (ok) {
    Serial.println("✅ POST success");
  } else {
    Serial.println("❌ POST failed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.printf("=== LilyGO T-A7670G Temperature Monitor ===\n");
  Serial.printf("*** FIRMWARE v3.0 - COMPACT DISPLAY LAYOUT ***\n");
  Serial.printf("*** NO MORE PROBE ME HARKIN - BATTERY ON TOP LINE ***\n");
  Serial.printf("Boot #%u, wake cause=%d\n", ++boot_count, esp_sleep_get_wakeup_cause());
  
  // Initialize AT command mutex
  atMux = xSemaphoreCreateMutex();
  
  // No GPIO holds to release (we don't hold strap pins anymore)
  
  // Set up GPIO pins first
  pinMode(MODEM_PWR_PIN, OUTPUT);
  pinMode(MODEM_RESET_PIN, OUTPUT);
  pinMode(MODEM_DTR_PIN, OUTPUT);
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  pinMode(MODEM_RING_PIN, INPUT);
  
  // Power rails for peripherals BEFORE I2C/OLED init
  digitalWrite(BOARD_POWERON_PIN, HIGH);
  delay(50);
  
  // Initialize I2C with correct pins
  Serial.printf("Initializing I2C on SDA=%d, SCL=%d\n", SDA_PIN, SCL_PIN);
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100); // Give I2C time to initialize
  
  // Initialize OLED display
  Serial.println("Attempting to initialize OLED display...");
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("❌ Display initialization failed on address 0x3C");
    Serial.println("Trying alternative I2C address 0x3D...");
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("❌ Display not found on I2C bus");
      Serial.println("Check I2C connections and power");
    } else {
      Serial.println("✅ Display initialized successfully on 0x3D");
    }
  } else {
    Serial.println("✅ Display initialized successfully on 0x3C");
  }
  
  // Turn on OLED display
  display.ssd1306_command(SSD1306_DISPLAYON);
  
  // Initialize temperature sensors
  sensors.begin();
  
  // Check wake-up reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool lteConnected = false;
  
  Serial.printf("Wake reason: %d, isFirstRun: %s, probeModeCompleted: %s\n", 
                wakeup_reason, isFirstRun ? "true" : "false", probeModeCompleted ? "true" : "false");
  
  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from deep sleep");
    // Ensure isFirstRun is false for all deep sleep wake-ups
    isFirstRun = false;
    
    // Don't show battery here (modem isn't up yet); keep OLED on for now
    
    // Setup LTE for deep sleep wake
    Serial.println("Setting up LTE connection after deep sleep wake...");
    lteConnected = setupLTE();
    lteReady = lteConnected; // Set the flag for consistency
    Serial.println("Deep sleep wake LTE result: " + String(lteConnected ? "Connected" : "Failed"));
  } else {
    Serial.println("First boot or reset");

    const bool timerWake = false; // we are in the ELSE of timer wake already
    const bool allowProbe = true;   // FORCE PROBE MODE TO ALWAYS RUN ON FIRST BOOT
    
    Serial.printf("Probe check: allowProbe=%s, probeAlreadyDone()=%s\n", 
                  allowProbe ? "true" : "false", probeAlreadyDone() ? "true" : "false");

    if (allowProbe) {
      Serial.println("*** PROBE MODE FORCED ON - 3 MINUTES ***");

      // Kick LTE in background RIGHT NOW
      if (!lteTaskHandle) {
        xTaskCreatePinnedToCore(lteConnectTask, "lteTask", 8192, NULL, 1, &lteTaskHandle, 0); // core 0
      }

      unsigned long until = millis() + 180000UL; // 3 minutes probe window
      while (millis() < until) {
        // Show sensors; DO NOT query modem unless LTE is ready
        display.clearDisplay();
        
        // Blue header area (top 16 pixels)
        display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
        display.setTextSize(1);
        
        // Yellow sensor area (bottom 48 pixels)
        display.fillRect(0, 16, 128, 48, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
        
        sensors.setResolution(12);
        sensors.requestTemperatures();
        delay(750);
        float t1 = sensors.getTempCByIndex(0);
        float t2 = sensors.getTempCByIndex(1);
        float t3 = sensors.getTempCByIndex(2);
        
        // Battery on top line (blue area) - only read when modem is ready
        static bool batteryRead = false;
        static PowerState ps;
        
        // Only try to read battery if LTE is ready (modem is ready for CBC)
        if (!batteryRead && lteReady) {
          Serial.println("Reading battery state (modem ready)...");
          ps = readPowerState();  // uses AT+CBC
          Serial.printf("Battery result: vbat=%.2f, percent=%d\n", ps.vbat, ps.percent);
          batteryRead = true;
        }
        
        display.setCursor(2, 5);
        if (batteryRead && ps.vbat > 0) {
          display.print("Bat: ");
          display.print(ps.percent);
          display.print("% ");
          display.print(ps.vbat, 2);
          display.print("V");
        } else {
          display.print("Battery: --%");
        }
        
        // Temperatures in yellow area
        display.setCursor(2, 20); 
        display.print("T1: "); 
        display.print(t1,2); 
        display.print("C");
        display.setCursor(2, 32); 
        display.print("T2: "); 
        display.print(t2,2); 
        display.print("C");
        display.setCursor(2, 44); 
        display.print("T3: "); 
        display.print(t3,2); 
        display.print("C");
        display.display();

        delay(500);
      }

      markProbeDone(); // never run probe again
    } else {
      // no probe after the first-ever boot
      Serial.println("SKIPPING probe mode - already done");
      // If we skipped probe, still kick LTE now (foreground or background—your choice)
      if (!lteTaskHandle && !lteReady) {
        xTaskCreatePinnedToCore(lteConnectTask, "lteTask", 8192, NULL, 1, &lteTaskHandle, 0);
      }
    }

    // Give LTE a little time to finish if it's still connecting
    unsigned long waitStart = millis();
    while (!lteReady && millis() - waitStart < 15000UL) { delay(200); }

    if (!lteReady) {
      Serial.println("❌ LTE not ready after probe+grace, sleeping");
      displayGoingToSleep();
      powerDownModem();
      enterDeepSleepSeconds(3600);
      return;
    }
    
    lteConnected = lteReady; // Use the background task result
  }
  
  // Check if LTE is connected
  if (!lteConnected) {
    Serial.println("❌ LTE setup failed, backoff 10 minutes");
    displayGoingToSleep();
    powerDownModem();
    enterDeepSleepSeconds(600); // Sleep for 10 minutes on failure (backoff)
    return;
  }
  
  Serial.println("✅ LTE connected, proceeding with data transmission...");
  
  // Show accurate battery status now that LTE is connected (modem is ON, so AT+CBC works now)
  displayBatteryStatus();
  delay(1500);                 // was 600 — actually see the voltage line
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  
  // Read temperature sensors (battery-friendly 9-bit resolution for data transmission)
  // Note: Probe mode uses 12-bit resolution for maximum sensitivity
  sensors.setResolution(9); // 9-bit ≈ 93.75 ms, much faster than 12-bit
  sensors.requestTemperatures();
  delay(100); // Short delay for conversion
  
  float t1 = sensors.getTempCByIndex(0);
  float t2 = sensors.getTempCByIndex(1);
  float t3 = sensors.getTempCByIndex(2);
  
  Serial.println("Temperature readings:");
  Serial.println("T1: " + String(t1, 2) + "°C");
  Serial.println("T2: " + String(t2, 2) + "°C");
  Serial.println("T3: " + String(t3, 2) + "°C");
  
  // Get current time
  String timestamp = getCurrentTime();
  Serial.println("Timestamp: " + timestamp);
  
  // Create JSON payload
  String payload = "{\"time\":\"" + timestamp.substring(11, 16) + "\",\"ts\":\"" + timestamp + "\",\"t1\":" + String(t1, 2) + ",\"t2\":" + String(t2, 2) + ",\"t3\":" + String(t3, 2) + "}";
  
  Serial.println("Payload: " + payload);
  Serial.println("About to send HTTP POST...");
  
  // Send data with timeout
  unsigned long dataStart = millis();
  sendHTTPPost(payload);
  unsigned long dataDuration = millis() - dataStart;
  
  Serial.printf("HTTP POST completed in %lu ms, proceeding to sleep...\n", dataDuration);
  
  // Power down modem
  powerDownModem();
  
  // Show going to sleep message briefly
  displayGoingToSleep();
  
  // Enter optimized deep sleep
  Serial.println("Going to sleep for 1 hour...");
  Serial.println("Sleep timer set, entering deep sleep NOW...");
  delay(1000); // Give time for serial output
  enterDeepSleepSeconds(3600); // Fixed 1 hour
}

void loop() {
  // This will never be reached due to deep sleep
}