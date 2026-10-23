#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WDT.h>
#include <WiFiS3.h>
#include <ArduinoMqttClient.h>
#include "arduino_secrets.h"

// ---------- Pins ----------
const int SENSOR_POWER = 7;
const int SENSOR_PIN   = A1;
const int RELAY_PIN    = 8;

const int RELAY_ON  = HIGH;  // your relay is active HIGH
const int RELAY_OFF = LOW;

// ---------- Calibration (fill in your numbers) ----------
const int RAW_DRY = 0;
const int RAW_WET = 800;
const int DRY_THRESHOLD = 30;

// ---------- Timing ----------
const unsigned long READ_INTERVAL = 5000;
const unsigned long PUMP_TIME     = 2000;
const unsigned long SOAK_TIME     = 15000;
const unsigned long HOUR          = 3600000UL;
const int MAX_PUMPS_PER_HOUR      = 5;
const unsigned long WDT_TIMEOUT   = 4000;
const unsigned long NET_RETRY     = 10000;   // retry network every 10 s

// ---------- MQTT ----------
// Public broker: anyone who knows your topic can read/send. Use a random topic name.
const char BROKER[]       = "broker.hivemq.com";
const int  BROKER_PORT    = 1883;
const char TOPIC_STATUS[] = "azur-plant-x7k2/status";   // board -> website
const char TOPIC_CMD[]    = "azur-plant-x7k2/cmd";      // website -> board
const char TOPIC_ONLINE[] = "azur-plant-x7k2/online";   // "1" / "0"

WiFiClient wifiClient;
MqttClient mqtt(wifiClient);

LiquidCrystal_I2C lcd(0x27, 16, 2);

enum State { IDLE, PUMPING, SOAKING, LOCKED };
State state = IDLE;
bool autoWater = false;

unsigned long lastRead = 0, pumpStart = 0, soakStart = 0, hourStart = 0, lastNetTry = 0;
int pumpsThisHour = 0;
int raw = 0, moisture = 0;

// ---------- Sensor / pump ----------
int readRaw() {
  digitalWrite(SENSOR_POWER, HIGH);
  delay(10);
  int v = analogRead(SENSOR_PIN);
  digitalWrite(SENSOR_POWER, LOW);
  return v;
}

int toPercent(int r) {
  return constrain(map(r, RAW_DRY, RAW_WET, 0, 100), 0, 100);
}

void pump(bool on) {
  digitalWrite(RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
}

const char* stateName() {
  switch (state) {
    case IDLE:    return autoWater ? "OK (auto)" : "OK (manual)";
    case PUMPING: return "Watering...";
    case SOAKING: return "Soaking...";
    case LOCKED:  return "Check water!";
  }
  return "";
}

const char* stateCode() {
  switch (state) {
    case IDLE:    return "idle";
    case PUMPING: return "pumping";
    case SOAKING: return "soaking";
    case LOCKED:  return "locked";
  }
  return "";
}

// ---------- Output ----------
void publishStatus() {
  if (!mqtt.connected()) return;
  char json[128];
  snprintf(json, sizeof(json),
    "{\"moisture\":%d,\"raw\":%d,\"state\":\"%s\",\"auto\":%s,\"pumps\":%d}",
    moisture, raw, stateCode(), autoWater ? "true" : "false", pumpsThisHour);
  mqtt.beginMessage(TOPIC_STATUS, true);   // retained: website gets last value on connect
  mqtt.print(json);
  mqtt.endMessage();
}

void refresh() {
  char line[17];
  snprintf(line, sizeof(line), "Moist:%3d%% %4d", moisture, raw);
  lcd.setCursor(0, 0);
  lcd.print(line);
  snprintf(line, sizeof(line), "%-15s%c", stateName(), mqtt.connected() ? '*' : ' ');
  lcd.setCursor(0, 1);
  lcd.print(line);

  Serial.print(raw); Serial.print("  ");
  Serial.print(moisture); Serial.print("%  ");
  Serial.println(stateName());

  publishStatus();
}

void resyncLcd() {
  delay(50);
  lcd.init();
  lcd.backlight();
  refresh();
}

// ---------- Commands (Serial + MQTT) ----------
bool startPump() {
  if (state != IDLE) return false;
  if (pumpsThisHour >= MAX_PUMPS_PER_HOUR) return false;
  pump(true);
  pumpStart = millis();
  pumpsThisHour++;
  state = PUMPING;
  refresh();
  return true;
}

void handleCommand(const char* cmd) {
  if (strcmp(cmd, "pump") == 0) {
    Serial.println(startPump() ? "Pump started" : "Pump busy/locked");
  } else if (strcmp(cmd, "auto:on") == 0) {
    autoWater = true;
    refresh();
  } else if (strcmp(cmd, "auto:off") == 0) {
    autoWater = false;
    refresh();
  } else if (strcmp(cmd, "status") == 0) {
    refresh();
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'p') handleCommand("pump");
  if (c == 'a') handleCommand(autoWater ? "auto:off" : "auto:on");
}

void onMqttMessage(int size) {
  char buf[32];
  int n = 0;
  while (mqtt.available() && n < (int)sizeof(buf) - 1) {
    buf[n++] = (char)mqtt.read();
  }
  buf[n] = '\0';
  while (mqtt.available()) mqtt.read();   // drop anything too long
  Serial.print("MQTT cmd: "); Serial.println(buf);
  handleCommand(buf);
}

// ---------- Network ----------
bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("WiFi connecting... ");
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.println(ok ? "OK" : "failed");
  return ok;
}

bool connectMqtt() {
  char id[32];
  snprintf(id, sizeof(id), "azur-plant-%lu", millis());
  mqtt.setId(id);
  mqtt.setConnectionTimeout(2000);
  mqtt.setKeepAliveInterval(15000);

  mqtt.beginWill(TOPIC_ONLINE, true, 1);   // broker sends "0" if we disappear
  mqtt.print("0");
  mqtt.endWill();

  Serial.print("MQTT connecting... ");
  if (!mqtt.connect(BROKER, BROKER_PORT)) {
    Serial.println("failed");
    return false;
  }
  Serial.println("OK");

  mqtt.onMessage(onMqttMessage);
  mqtt.subscribe(TOPIC_CMD);

  mqtt.beginMessage(TOPIC_ONLINE, true);
  mqtt.print("1");
  mqtt.endMessage();

  refresh();
  return true;
}

void maintainNetwork(unsigned long now) {
  if (mqtt.connected()) {
    mqtt.poll();
    return;
  }
  if (state == PUMPING) return;              // never block while the pump is on
  if (now - lastNetTry < NET_RETRY) return;
  lastNetTry = now;
  if (connectWifi()) connectMqtt();
}

// ---------- Main ----------
void setup() {
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(SENSOR_POWER, OUTPUT);
  digitalWrite(SENSOR_POWER, LOW);

  Serial.begin(115200);
  lcd.init();
  lcd.backlight();

  raw = readRaw();
  moisture = toPercent(raw);
  refresh();

  // connect before the watchdog starts, WiFi can take several seconds
  if (connectWifi()) connectMqtt();

  Serial.println("Commands: p = pump once, a = toggle auto");
  WDT.begin(WDT_TIMEOUT);
}

void loop() {
  WDT.refresh();
  unsigned long now = millis();

  handleSerial();
  maintainNetwork(now);

  if (now - hourStart >= HOUR) {
    hourStart = now;
    pumpsThisHour = 0;
    if (state == LOCKED) {
      state = IDLE;
      refresh();
    }
  }

  if (now - lastRead >= READ_INTERVAL) {
    lastRead = now;
    raw = readRaw();
    moisture = toPercent(raw);
    refresh();
  }

  switch (state) {
    case IDLE:
      if (autoWater && moisture < DRY_THRESHOLD) {
        if (!startPump()) {
          state = LOCKED;
          refresh();
        }
      }
      break;

    case PUMPING:
      if (now - pumpStart >= PUMP_TIME) {
        pump(false);
        soakStart = now;
        state = SOAKING;
        resyncLcd();
      }
      break;

    case SOAKING:
      if (now - soakStart >= SOAK_TIME) {
        state = IDLE;
        lastRead = 0;
      }
      break;

    case LOCKED:
      break;
  }
}
