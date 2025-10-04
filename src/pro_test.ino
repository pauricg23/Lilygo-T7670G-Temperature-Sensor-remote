#include <HardwareSerial.h>
#include <driver/rtc_io.h>
#include "esp_task_wdt.h"

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ========= Build-time logging toggle =========
#define LOGGING 0
#if LOGGING
  #define DBG(...)  do { Serial.printf(__VA_ARGS__); } while(0)
#else
  #define DBG(...)  do {} while(0)
#endif

// ========= Board / modem pins (LilyGO T-A7670G defaults) =========
#define BOARD_POWERON_PIN 12
#define MODEM_PWR_PIN      4
#define MODEM_RESET_PIN    5
#define MODEM_DTR_PIN     25
#define MODEM_RING_PIN    34
#define UART_RX           27   // ESP32 RX from modem TX
#define UART_TX           26   // ESP32 TX to modem RX

// ========= I2C / display / sensors =========
#define SDA_PIN           33
#define SCL_PIN           32
#define ONE_WIRE_BUS      14

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C

// Guard OLED so we never call it unless it’s really there
#define USE_OLED 1
static bool OLED_READY = false;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
inline void oledOn()  { if (USE_OLED && OLED_READY) display.ssd1306_command(SSD1306_DISPLAYON); }
inline void oledOff() { if (USE_OLED && OLED_READY) display.ssd1306_command(SSD1306_DISPLAYOFF); }

// ========= DS18B20 =========
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ========= LTE modem UART =========
HardwareSerial Modem(1);

// ========= Config =========
const char* serverUrl = "http://housetemp.pauricgrant.com/submit";
const char* apn       = "simbase";

#define BATTERY_ADC_PIN 35
#define BATTERY_DIVIDER_RATIO 2.0  // 10k:10k

// Sleep & timing
#define NORMAL_SLEEP_SECS   3600   // 1 hour (change to 10 for testing)
#define FAIL_BACKOFF_SECS    600   // 10 min
#define PROBE_DURATION_MS  180000  // 3 minutes

// RAT
#define CNMP_VALUE 38  // LTE only
#define CMNB_VALUE 2   // Cat-M only (not used: known ERROR on some firmwares)

// ========= RTC persist (survives deep sleep) =========
RTC_DATA_ATTR bool     probeModeCompleted = false; // only run probe on POWERON once
RTC_DATA_ATTR uint32_t boot_count         = 0;
RTC_DATA_ATTR uint32_t last_boot_count    = 0;

// ========= Helpers =========
const char* wakeName(esp_sleep_wakeup_cause_t w){
  switch (w){
    case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER";
    case ESP_SLEEP_WAKEUP_EXT0:      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP:       return "ULP";
    default:                         return "UNDEFINED";
  }
}
const char* resetName(esp_reset_reason_t r){
  switch (r){
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "I-WDT";
    case ESP_RST_TASK_WDT:  return "T-WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "OTHER";
  }
}

float readBatteryVoltage() {
  int adcValue = analogRead(BATTERY_ADC_PIN);
  float adcVoltage = (adcValue / 4095.0f) * 3.3f;
  return adcVoltage * BATTERY_DIVIDER_RATIO;
}

// ========= AT helpers =========
String sendAT(const String& cmd, uint32_t timeout){
  DBG(">> %s\n", cmd.c_str());
  Modem.println(cmd);
  String r; uint32_t t = millis();
  while(millis() - t < timeout){
    while (Modem.available()) r += char(Modem.read());
    delay(3);
  }
  DBG("<< %s\n", r.c_str());
  return r;
}
String readUntil(const String& token, uint32_t timeout_ms){
  String buf; uint32_t start = millis();
  while (millis() - start < timeout_ms){
    while (Modem.available()) buf += char(Modem.read());
    if (token.length() && buf.indexOf(token) >= 0) break;
    delay(5);
  }
  if (LOGGING && buf.length()) Serial.println("URC<<< " + buf);
  return buf;
}

// ========= LTE bring-up (foreground) =========
bool setupLTE() {
  DBG("Setting up LTE...\n");

  // Ensure modem rail is ON
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);
  delay(1000);

  // Basic control pins (idle high for these boards)
  pinMode(MODEM_PWR_PIN,   OUTPUT);
  pinMode(MODEM_RESET_PIN, OUTPUT);
  pinMode(MODEM_DTR_PIN,   OUTPUT);
  digitalWrite(MODEM_PWR_PIN,   HIGH);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  digitalWrite(MODEM_DTR_PIN,   HIGH);
  delay(10000); // modem boot time

  // UART to modem
  Modem.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  delay(1000);

  sendAT("AT", 1500);
  sendAT("AT+CFUN=1", 3000);
  sendAT(String("AT+CNMP=")+CNMP_VALUE, 3000);
  sendAT(String("AT+CGDCONT=1,\"IP\",\"") + apn + "\"", 3000);

  String r = sendAT("AT+CPIN?", 3000);
  if (r.indexOf("READY") < 0) { DBG("SIM not ready\n"); return false; }

  r = sendAT("AT+CSQ", 3000);
  if (r.indexOf("+CSQ:") < 0) { DBG("No signal\n"); return false; }

  sendAT("AT+CEREG=2", 1000);
  String reg = sendAT("AT+CEREG?", 2000); // don’t block; network can be slow
  (void)reg;

  sendAT("AT+CGATT=1", 4000);
  r = sendAT("AT+CGACT=1,1", 6000);
  if (r.indexOf("OK") < 0) { DBG("PDP act fail\n"); return false; }

  r = sendAT("AT+CGPADDR=1", 2000);
  if (r.indexOf('.') < 0) { DBG("No IP on CID 1\n"); return false; }

  // Kill LEDs
  sendAT("AT+CNETLIGHT=0", 1000);
  sendAT("AT+CLED=0", 1000);
  sendAT("AT+CLED=1,0", 1000);
  sendAT("AT+CLED=2,0", 1000);
  sendAT("AT+CLED=3,0", 1000);
  sendAT("AT&W", 1000);

  DBG("LTE up\n");
  return true;
}

// ========= HTTP via modem =========
bool httpPostJson(const String& url, const String& json){
  String r = sendAT("AT+CGACT?", 2000);
  if (r.indexOf("1,1") < 0) return false;

  sendAT("AT+HTTPTERM", 300);
  sendAT("AT+HTTPINIT", 2000);
  sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 3000);
  sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 500);

  r = sendAT("AT+HTTPDATA=" + String(json.length()) + ",8000", 3000);
  if (r.indexOf("DOWNLOAD") < 0) { sendAT("AT+HTTPTERM", 300); return false; }
  Modem.write((const uint8_t*)json.c_str(), json.length());
  readUntil("OK", 3000);

  String rAct = sendAT("AT+HTTPACTION=1", 150);
  String urc = rAct;
  if (urc.indexOf("+HTTPACTION:") < 0) urc = readUntil("+HTTPACTION:", 120000);

  int httpCode = -1, bodyLen = -1;
  if (urc.indexOf("+HTTPACTION:") >= 0) {
    int p = urc.lastIndexOf("+HTTPACTION:");
    int c1 = urc.indexOf(',', p);
    int c2 = urc.indexOf(',', c1+1);
    if (p>=0 && c1>p && c2>c1) {
      httpCode = urc.substring(c1+1, c2).toInt();
      bodyLen  = urc.substring(c2+1).toInt();
    }
  }
  sendAT("AT+HTTPTERM", 800);
  return (httpCode == 200 || httpCode == 201 || httpCode == 204);
}

// ========= Small UI helpers =========
void displayGoingToSleep(){
  if (USE_OLED && OLED_READY) {
    oledOn();
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(8, 25);
    display.print("Going to sleep...");
    display.display();
    delay(400);
    oledOff();
  } else {
    DBG("Going to sleep...\n");
  }
}

// ========= Deep sleep (hold modem rail HIGH) =========
void enterDeepSleepSeconds(uint32_t seconds){
  DBG("Deep sleep for %u s\n", seconds);

  // Timer wake
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);

  // Quiet peripherals
  if (USE_OLED && OLED_READY) { display.clearDisplay(); display.display(); display.ssd1306_command(SSD1306_DISPLAYOFF); }
  Wire.end();
  pinMode(SDA_PIN, INPUT);
  pinMode(SCL_PIN, INPUT);
  pinMode(ONE_WIRE_BUS, INPUT);
  rtc_gpio_isolate((gpio_num_t)MODEM_RING_PIN);

  // Release UART to modem
  Modem.end();
  pinMode(UART_TX, INPUT);
  pinMode(UART_RX, INPUT);

  // Avoid backfeed on control pins
  pinMode(MODEM_PWR_PIN,   INPUT);
  pinMode(MODEM_RESET_PIN, INPUT);
  pinMode(MODEM_DTR_PIN,   INPUT);
  pinMode(MODEM_RING_PIN,  INPUT);

  // Keep modem rail ON across deep sleep (prevents brownouts)
  rtc_gpio_hold_dis((gpio_num_t)BOARD_POWERON_PIN);
  rtc_gpio_deinit((gpio_num_t)BOARD_POWERON_PIN);
  rtc_gpio_set_direction((gpio_num_t)BOARD_POWERON_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)BOARD_POWERON_PIN, 1);  // hold HIGH
  rtc_gpio_hold_en((gpio_num_t)BOARD_POWERON_PIN);

  // Keep RTC periph on so the hold actually holds
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // Kill task WDT noise
  esp_task_wdt_deinit();

  // Zzz
  esp_deep_sleep_start();
}

// ========= Probe (UI only; no LTE) =========
void runProbeOnceUIOnly(){
  unsigned long until = millis() + PROBE_DURATION_MS;
  if (USE_OLED && OLED_READY) oledOn();

  while (millis() < until) {
    sensors.setResolution(12);
    sensors.requestTemperatures();
    delay(750);
    float t1 = sensors.getTempCByIndex(0);
    float t2 = sensors.getTempCByIndex(1);
    float t3 = sensors.getTempCByIndex(2);

    if (USE_OLED && OLED_READY) {
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);

      unsigned long remain = (until - millis())/1000;
      display.setCursor(2, 0);
      display.print("Probe ");
      display.print(remain/60); display.print(":");
      if ((remain%60) < 10) display.print("0");
      display.print(remain%60);

      display.setCursor(2, 20); display.print("T1: "); display.print(t1,2); display.print("C");
      display.setCursor(2, 32); display.print("T2: "); display.print(t2,2); display.print("C");
      display.setCursor(2, 44); display.print("T3: "); display.print(t3,2); display.print("C");
      display.display();
    } else {
      DBG("Probe t=%.2f/%.2f/%.2f\n", t1,t2,t3);
    }
    delay(300);
  }

  if (USE_OLED && OLED_READY) oledOff();
}

// ========= setup / loop =========
void setup(){
#if LOGGING
  Serial.begin(115200);
  delay(150);
#endif

  // Re-assert modem rail on boot and release any holds
  rtc_gpio_hold_dis((gpio_num_t)BOARD_POWERON_PIN);
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);
  delay(500);

  // Read reasons
  esp_reset_reason_t reset = esp_reset_reason();
  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  const bool fromDeepSleep = (reset == ESP_RST_DEEPSLEEP);
  const bool powerOnBoot   = (reset == ESP_RST_POWERON);

  // Probe only once on true POWERON
  bool shouldRunProbe = (powerOnBoot && !probeModeCompleted);
  if (powerOnBoot) {
    probeModeCompleted = false; // allow exactly one probe now
    last_boot_count = boot_count;
    boot_count++;
  }

  // Init I2C / OLED safely
#if USE_OLED
  Wire.end();
  delay(50);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(50);
  OLED_READY = display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  if (!OLED_READY) {
    OLED_READY = display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  }
  if (OLED_READY) {
    oledOn();
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0,0);
    display.print("RST: ");  display.println(resetName(reset));
    display.print("WAKE: "); display.println(wakeName(wake));
    display.display();
    delay(700);
    oledOff();
  }
#endif

  // Sensors
  pinMode(BATTERY_ADC_PIN, INPUT);
  sensors.begin();

  // ----- PROBE (UI only) -----
  if (shouldRunProbe) {
    runProbeOnceUIOnly();
    probeModeCompleted = true;  // don't run again until next POWERON
  }

  // ----- LTE connect (foreground) -----
  bool lteConnected = setupLTE();
  if (!lteConnected) {
    displayGoingToSleep();
    enterDeepSleepSeconds(FAIL_BACKOFF_SECS);
    return;
  }

  // ----- Build payload -----
  sensors.setResolution(9);
  sensors.requestTemperatures();
  delay(100);
  float t1 = sensors.getTempCByIndex(0);
  float t2 = sensors.getTempCByIndex(1);
  float t3 = sensors.getTempCByIndex(2);
  float vb = readBatteryVoltage();

  if (vb < 3.4f) {
    String alert = String("{\"alert\":\"LOW_BATTERY\",\"battery\":") + String(vb,2) + "}";
    httpPostJson("http://housetemp.pauricgrant.com/alert", alert);
  } else if (vb < 3.6f) {
    String warn = String("{\"alert\":\"BATTERY_WARNING\",\"battery\":") + String(vb,2) + "}";
    httpPostJson("http://housetemp.pauricgrant.com/alert", warn);
  }

  String payload = String("{")
    + "\"t1\":" + String(t1,2) + ","
    + "\"t2\":" + String(t2,2) + ","
    + "\"t3\":" + String(t3,2) + ","
    + "\"battery\":" + String(vb,2) + ","
    + "\"battery_status\":\"" + (vb<3.4f ? "CRITICAL" : vb<3.6f ? "LOW" : "OK") + "\","
    // === moved to top-level ===
    + "\"wake_cause\":" + String((int)wake) + ","
    + "\"wake_cause_name\":\"" + String(wakeName(wake)) + "\","
    + "\"reset_reason\":" + String((int)reset) + ","
    + "\"reset_reason_name\":\"" + String(resetName(reset)) + "\","
    + "\"boot_count\":" + String(boot_count) + ","
    + "\"last_boot_count\":" + String(last_boot_count) + ","
    + "\"probe_mode_completed\":" + String(probeModeCompleted ? "true" : "false") + ","
    + "\"should_run_probe\":" + String(shouldRunProbe ? "true" : "false")
  + "}";

  // Debug: print payload to see what we're sending
  Serial.println("Payload: " + payload);
  
  bool ok = httpPostJson(serverUrl, payload);
  if (!ok) {
    displayGoingToSleep();
    enterDeepSleepSeconds(FAIL_BACKOFF_SECS);
    return;
  }

  // ----- Sleep normally -----
  displayGoingToSleep();
  enterDeepSleepSeconds(NORMAL_SLEEP_SECS);
}

void loop(){ /* never reached */ }
