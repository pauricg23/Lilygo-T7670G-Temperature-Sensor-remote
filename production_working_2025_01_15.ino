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

// ===================== Config =====================
const char* serverUrl = "http://housetemp.pauricgrant.com/submit";
const char* apn       = "simbase";

// Sleep & timing
#define NORMAL_SLEEP_SECS   3600    // 1 hour
#define FAIL_BACKOFF_SECS    600    // 10 minutes on failure
#define PROBE_DURATION_MS  15000    // one-time probe window

// RAT selection (typical: LTE-only, Cat-M only)
#define CNMP_VALUE 38  // 38=LTE only
#define CMNB_VALUE 2   // 2=Cat-M only (0=auto, 1=NB-IoT)

// Modem power policy: use PSM (no CFUN=0). Flip to 1 to hard power-off each cycle.
#define USE_MODEM_PSM 1

// ===================== Board pins =====================
#define BOARD_POWERON_PIN 12
#define MODEM_PWR_PIN      4
#define MODEM_RESET_PIN    5
#define MODEM_DTR_PIN     25
#define MODEM_RING_PIN    34

#define SDA_PIN           33
#define SCL_PIN           32

#define ONE_WIRE_BUS      14

// ===================== Display =====================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ===================== DS18B20 =====================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ===================== FreeRTOS =====================
volatile bool lteReady = false;
TaskHandle_t lteTaskHandle = NULL;
SemaphoreHandle_t atMux;

// ===================== RTC persist =====================
RTC_DATA_ATTR bool isFirstRun         = true;
RTC_DATA_ATTR bool probeModeCompleted = false;
RTC_DATA_ATTR bool psmTried           = false;
RTC_DATA_ATTR uint32_t boot_count     = 0;
RTC_DATA_ATTR int lastPct             = -1;

// ---------- Li-ion OCV mapping ----------
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

template<typename T>
T median5(T a, T b, T c, T d, T e) {
  T v[5] = {a,b,c,d,e};
  for (int i=0;i<5;i++) for (int j=i+1;j<5;j++) if (v[j]<v[i]) { T t=v[i]; v[i]=v[j]; v[j]=t; }
  return v[2];
}

int smoothPercent(int now) {
  if (lastPct < 0) { lastPct = now; return now; }
  if (now > lastPct + 4) now = lastPct + 4;
  if (now < lastPct - 4) now = lastPct - 4;
  lastPct = now;
  return now;
}

// ---------- Probe tracking in NVS ----------
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

// ---------- AT helpers (mutex protected) ----------
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

// ---------- Modem bring-up ----------
bool setupLTE() {
  Serial.println("Setting up LTE connection...");

  // power rails / lines
  digitalWrite(MODEM_PWR_PIN, HIGH);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  digitalWrite(MODEM_DTR_PIN, HIGH);
  delay(10000); // boot time

  Modem.begin(115200, SERIAL_8N1, 27, 26); // (RX,TX on ESP32 side)
  delay(2000);

  sendATCommand("AT", 1500);
  sendATCommand("AT+CFUN=1", 3000);        // ensure full functionality

  // RAT selection + APN
  sendATCommand(String("AT+CNMP=") + CNMP_VALUE, 3000); // LTE only
  sendATCommand(String("AT+CMNB=") + CMNB_VALUE, 3000); // Cat-M only
  sendATCommand(String("AT+CGDCONT=1,\"IP\",\"") + apn + "\"", 3000);

  // sanity checks
  String r = sendATCommand("AT+CPIN?", 3000);
  if (r.indexOf("READY") < 0) { Serial.println("❌ SIM not ready"); return false; }

  r = sendATCommand("AT+CSQ", 3000);
  if (r.indexOf("+CSQ:") < 0) { Serial.println("❌ No signal"); return false; }

  sendATCommand("AT+CEREG=2", 1000);
  String rCereg = sendATCommand("AT+CEREG?", 2000);
  bool epsReg = false;
  int statPos = rCereg.indexOf("+CEREG:");
  if (statPos >= 0) {
    int c1 = rCereg.indexOf(',', statPos);
    int c2 = rCereg.indexOf(',', c1+1);
    int stat = rCereg.substring(c1+1, c2).toInt();
    epsReg = (stat == 1 || stat == 5);
  }
  if (!epsReg) Serial.println("⚠️ EPS reg not yet 1/5, continuing...");

  String att = sendATCommand("AT+CGATT=1", 4000);
  // PDP up
  r = sendATCommand("AT+CGACT=1,1", 6000);
  if (r.indexOf("OK") < 0) { Serial.println("❌ PDP activation failed"); return false; }

  // Got IP?
  String ip = sendATCommand("AT+CGPADDR=1", 2000);
  if (ip.indexOf('.') < 0) { Serial.println("❌ No IP on CID 1"); return false; }

  Serial.println("✅ LTE connection established");
  return true;
}

// ---------- Background task to do LTE bring-up during probe ----------
void lteConnectTask(void*) {
  lteReady = setupLTE();
  vTaskDelete(NULL);
}

// ---------- Battery via modem ----------
struct PowerState { bool usb, charging, full; int percent; float vbat; };

PowerState readPowerState() {
  Serial.println("*** readPowerState() ***");
  PowerState ps{};
  String test = sendATCommand("AT", 600);
  if (test.indexOf("OK") < 0) {
    Serial.println("Modem not ready, empty PowerState");
    return ps;
  }

  int mv[5] = {0}, st[5] = {0};
  for (int i=0;i<5;i++) {
    String r = sendATCommand("AT+CBC", 800);
    int k = r.indexOf("+CBC:");
    if (k >= 0) {
      int c1 = r.indexOf(',', k);
      int c2 = r.indexOf(',', c1+1);
      if (c1>0 && c2>c1) {
        st[i] = r.substring(k+6, c1).toInt();  // 0=disch, 1=charging, 2=full
        mv[i] = r.substring(c2+1).toInt();     // mV
      }
    }
    delay(80);
  }

  int mvmid = median5(mv[0],mv[1],mv[2],mv[3],mv[4]);
  int stmid = median5(st[0],st[1],st[2],st[3],st[4]);

  if (mvmid <= 0) {
    Serial.println("No valid CBC data");
    return ps;
  }

  ps.vbat = mvmid / 1000.0f;
  bool charging = (stmid == 1);
  if (charging) ps.vbat -= 0.12f;          // remove charge bump to approximate OCV

  int pct = lipoPercentFromVoltage(ps.vbat);
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  ps.percent  = smoothPercent(pct);
  ps.charging = charging;
  ps.full     = (stmid == 2) || (!charging && ps.vbat >= 4.18f);
  ps.usb      = ps.charging || ps.full;

  Serial.printf("CBC mid: %dmV state=%d -> %.2fV ~ %d%%\n", mvmid, stmid, ps.vbat, ps.percent);
  return ps;
}

void displayBatteryStatus() {
  PowerState ps = readPowerState();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  if (ps.vbat > 0) {
    display.print(ps.usb ? (ps.charging ? "Charging " : "Full ") : "Battery ");
    display.print(ps.percent); display.print("%");
    display.setCursor(0, 12);
    display.print(ps.vbat, 2); display.print("V");
  } else {
    display.print("Battery: --%");
  }
  display.display();
}

// ---------- Probe UI (once, overlaps LTE task) ----------
void runProbeOnceWithLTE() {
  Serial.println("Probe mode (overlapping LTE bring-up)");
  if (!lteTaskHandle) {
    xTaskCreatePinnedToCore(lteConnectTask, "lteTask", 8192, NULL, 1, &lteTaskHandle, 0);
  }

  unsigned long until = millis() + PROBE_DURATION_MS;
  bool batteryShown = false;
  PowerState ps{};

  while (millis() < until) {
    // Read temps at max resolution
    sensors.setResolution(12);
    sensors.requestTemperatures();
    delay(750);
    float t1 = sensors.getTempCByIndex(0);
    float t2 = sensors.getTempCByIndex(1);
    float t3 = sensors.getTempCByIndex(2);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    // Top line: LTE status OR battery once LTE is ready
    display.setCursor(0, 0);
    if (!lteReady) {
      display.print("LTE: connecting...");
    } else {
      if (!batteryShown) { ps = readPowerState(); batteryShown = true; }
      display.print("Bat: "); display.print(ps.percent);
      display.print("% "); display.print(ps.vbat, 2); display.print("V");
    }

    // Big temps
    display.setTextSize(2);
    display.setCursor(0, 18); display.print("T1: "); display.print(t1,2); display.print("C");
    display.setCursor(0, 38); display.print("T2: "); display.print(t2,2); display.print("C");
    display.setCursor(0, 58); display.print("T3: "); display.print(t3,2); display.print("C");
    display.display();

    delay(300);
  }

  markProbeDone();
}

// ---------- HTTP POST (JSON) ----------
bool httpPostJson(const String& url, const String& json) {
  Serial.println("\n=== HTTP POST ===");

  String r = sendATCommand("AT+CGACT?", 2000);
  if (r.indexOf("1,1") < 0) { Serial.println("❌ PDP not active"); return false; }

  sendATCommand("AT+HTTPTERM", 500);           // clean slate
  sendATCommand("AT+HTTPINIT", 2000);
  sendATCommand("AT+HTTPPARA=\"URL\",\"" + url + "\"", 3000);
  sendATCommand("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 500);

  r = sendATCommand("AT+HTTPDATA=" + String(json.length()) + ",8000", 3000);
  if (r.indexOf("DOWNLOAD") < 0) {
    Serial.println("❌ No DOWNLOAD");
    sendATCommand("AT+HTTPTERM", 1000);
    return false;
  }
  Modem.write((const uint8_t*)json.c_str(), json.length());
  readModemUntil("OK", 3000);

  sendATCommand("AT+HTTPACTION=1", 800);
  String urc = readModemUntil("+HTTPACTION:", 30000);
  if (urc.indexOf("+HTTPACTION:") < 0) {
    Serial.println("❌ No HTTPACTION URC");
    sendATCommand("AT+HTTPTERM", 1000);
    return false;
  }

  int p = urc.indexOf("+HTTPACTION:");
  int c1 = urc.indexOf(',', p);
  int c2 = urc.indexOf(',', c1+1);
  int httpCode = (c1>0 && c2>c1) ? urc.substring(c1+1, c2).toInt() : -1;
  Serial.printf("HTTP code: %d\n", httpCode);

  sendATCommand("AT+HTTPTERM", 1000);
  return (httpCode == 200 || httpCode == 201 || httpCode == 204);
}

// ---------- Power-down policy ----------
bool enablePSM() {
  sendATCommand("AT+CEREG=4", 800);
  String r = sendATCommand("AT+CPSMS=1", 1200); // network-defined timers
  return (r.indexOf("OK") >= 0);
}

void powerDownModem() {
#if USE_MODEM_PSM
  Serial.println("Powering down modem via PSM (stay CFUN=1)...");
  if (!psmTried) { enablePSM(); psmTried = true; }
  digitalWrite(MODEM_DTR_PIN, HIGH); // allow sleep on many SIMComs
#else
  Serial.println("Graceful modem power off...");
  sendATCommand("AT+CPOWD=1", 3000);
  delay(500);
#endif
}

// ---------- Deep sleep wrapper ----------
void displayGoingToSleep() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(14, 25);
  display.print("Going to sleep...");
  display.display();
  delay(500);
}

void enterDeepSleepSeconds(uint32_t secs) {
  // OLED & I2C off
  display.clearDisplay(); display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  Wire.end();
  pinMode(SDA_PIN, INPUT);
  pinMode(SCL_PIN, INPUT);
  pinMode(ONE_WIRE_BUS, INPUT);
  rtc_gpio_isolate((gpio_num_t)MODEM_RING_PIN);

  // strap pins: don't hold
  digitalWrite(BOARD_POWERON_PIN, HIGH);

  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  esp_deep_sleep_start();
}

// ===================== setup/loop =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("=== LilyGO T-A7670G Temp Monitor ===\n");
  Serial.printf("Boot #%u, wake cause=%d\n", ++boot_count, esp_sleep_get_wakeup_cause());

  atMux = xSemaphoreCreateMutex();

  pinMode(MODEM_PWR_PIN, OUTPUT);
  pinMode(MODEM_RESET_PIN, OUTPUT);
  pinMode(MODEM_DTR_PIN, OUTPUT);
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  pinMode(MODEM_RING_PIN, INPUT);

  digitalWrite(BOARD_POWERON_PIN, HIGH);
  delay(50);

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("❌ OLED not found");
    }
  }
  display.clearDisplay(); display.display();

  sensors.begin();

  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool lteConnected = false;

  if (wake == ESP_SLEEP_WAKEUP_TIMER) {
    // NORMAL CYCLE: no probe UI, just bring LTE up and send
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    Serial.println("Timer wake: bringing LTE up (foreground)...");
    lteConnected = setupLTE();
  } else {
    // FIRST BOOT / RESET
    bool allowProbe = !probeAlreadyDone();
    if (allowProbe) {
      runProbeOnceWithLTE();       // does LTE in background task
      // small grace to finish attach if still in flight
      unsigned long t0 = millis();
      while (!lteReady && millis() - t0 < 5000) delay(100);
      lteConnected = lteReady;
    } else {
      // probe already done: skip UI, just bring LTE
      display.ssd1306_command(SSD1306_DISPLAYOFF);
      lteConnected = setupLTE();
    }
  }

  if (!lteConnected) {
    Serial.println("❌ LTE not ready -> backoff sleep");
    displayGoingToSleep();
    powerDownModem();
    enterDeepSleepSeconds(FAIL_BACKOFF_SECS);
    return;
  }

  Serial.println("✅ LTE ready; show battery, then send data");
  // Show battery status (modem is ON so CBC works)
  displayBatteryStatus();
  delay(900);
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  // Read temps quickly for the POST
  sensors.setResolution(9);
  sensors.requestTemperatures();
  delay(100);
  float t1 = sensors.getTempCByIndex(0);
  float t2 = sensors.getTempCByIndex(1);
  float t3 = sensors.getTempCByIndex(2);
  Serial.printf("T1=%.2f T2=%.2f T3=%.2f\n", t1,t2,t3);

  // Build minimal JSON (server timestamps it)
  String payload = String("{\"t1\":") + String(t1,2) +
                   ",\"t2\":" + String(t2,2) +
                   ",\"t3\":" + String(t3,2) + "}";

  bool ok = httpPostJson(String(serverUrl), payload);
  if (!ok) {
    Serial.println("POST failed -> backoff sleep");
    displayGoingToSleep();
    powerDownModem();
    enterDeepSleepSeconds(FAIL_BACKOFF_SECS);
    return;
  }

  Serial.println("POST success -> normal sleep");
  displayGoingToSleep();
  powerDownModem();
  enterDeepSleepSeconds(NORMAL_SLEEP_SECS);
}

void loop() {
  // never reached
}
