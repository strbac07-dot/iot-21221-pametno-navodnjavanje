/*
 * ============================================================
 *  PAMETNO NAVODNJAVANJE S DETEKCIJOM VLAGE TLA
 *  Predmet: Dizajn i razvoj IoT projekata (IoT_21221)
 * ------------------------------------------------------------
 *  Hardver (simulirano u Wokwi):
 *    - ESP32 DevKit C v4
 *    - Senzor vlage tla  -> simuliran potenciometrom na GPIO34 (ADC1_CH6)
 *    - DHT22 (temperatura + vlaznost zraka) na GPIO15
 *    - LED "PUMPA" (plava) na GPIO26
 *    - LED "ALARM" (crvena) na GPIO27
 *
 *  Cloud:
 *    - HiveMQ Cloud  -> MQTT preko TLS (port 8883)
 *    - ThingSpeak    -> HTTP upis u kanal (field1..field4)
 *
 *  Dashboard:
 *    - IoT MQTT Panel (Android) ili HiveMQ Web Client
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ================= KONFIGURACIJA (popuni svojim podacima) ====
#include "secrets.h"

// ================= PINOVI ====================================
#define PIN_SOIL   34    // ADC1_CH6 — samo ulaz!
#define PIN_DHT    15
#define PIN_PUMP   26
#define PIN_ALARM  27

#define DHTTYPE DHT22
DHT dht(PIN_DHT, DHTTYPE);

// ================= PRAGOVI (histereza) =======================
int thDry = 35;                 // ispod -> ukljuci pumpu
int thWet = 60;                 // iznad -> iskljuci pumpu
const float TEMP_ALARM = 38.0;  // temperaturni alarm

// ================= VREMENSKI INTERVALI =======================
const unsigned long T_SAMPLE    = 2000;   // citanje senzora
const unsigned long T_MQTT      = 5000;   // slanje telemetrije
const unsigned long T_TS        = 20000;  // ThingSpeak (min 15 s)
const unsigned long PUMP_MAX_MS = 60000;  // sigurnosni limit rada pumpe

unsigned long tSample = 0, tMqtt = 0, tTs = 0, pumpStart = 0;

// ================= STANJE ====================================
bool  pumpOn = false;
bool  autoMode = true;
float temperature = NAN, humidity = NAN;
int   soilPct = 0;
int   rawAdc = 0;

WiFiClientSecure netSecure;
PubSubClient mqtt(netSecure);

// ================= SENZOR VLAGE ==============================
int readSoilPercent() {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(PIN_SOIL); delay(2); }
  rawAdc = sum / 10;
  // Kapacitivni senzor: suho = visok napon, mokro = nizak -> invertujemo.
  int pct = map(rawAdc, 0, 4095, 100, 0);
  return constrain(pct, 0, 100);
}

// ================= UPRAVLJANJE PUMPOM ========================
void publishStatus(const char* reason) {
  StaticJsonDocument<160> doc;
  doc["pump"]   = pumpOn ? "ON" : "OFF";
  doc["reason"] = reason;
  doc["mode"]   = autoMode ? "AUTO" : "MANUAL";
  char buf[160];
  serializeJson(doc, buf);
  if (mqtt.connected()) mqtt.publish(TOPIC_STATUS, buf, true);  // retained
}

void setPump(bool on, const char* reason) {
  if (on == pumpOn) return;
  pumpOn = on;
  digitalWrite(PIN_PUMP, on ? HIGH : LOW);
  if (on) pumpStart = millis();
  Serial.printf("[PUMPA] %s (%s)\n", on ? "UKLJUCENA" : "ISKLJUCENA", reason);
  publishStatus(reason);
}

// ================= WIFI ======================================
void connectWiFi() {
  Serial.print("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print(".");
  }
  Serial.printf(" OK, IP: %s\n", WiFi.localIP().toString().c_str());
}

// ================= MQTT ======================================
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.printf("[MQTT RX] %s = %s\n", topic, msg.c_str());

  if (String(topic) == TOPIC_CMD) {
    if (msg == "PUMP_ON")       { autoMode = false; setPump(true,  "rucna komanda"); }
    else if (msg == "PUMP_OFF") { autoMode = false; setPump(false, "rucna komanda"); }
    else if (msg == "AUTO")     { autoMode = true;  Serial.println("[MODE] AUTO"); }
  } else if (String(topic) == TOPIC_CONFIG) {
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, msg)) {
      if (doc.containsKey("dry")) thDry = doc["dry"];
      if (doc.containsKey("wet")) thWet = doc["wet"];
      if (thWet <= thDry) thWet = thDry + 5;
      Serial.printf("[CONFIG] dry=%d wet=%d\n", thDry, thWet);
    }
  }
}

void connectMqtt() {
  netSecure.setInsecure();  // za simulaciju/projekt; u produkciji ucitaj CA cert
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);

  int tries = 0;
  while (!mqtt.connected() && tries < 5) {
    Serial.print("MQTT...");
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println(" spojen");
      mqtt.subscribe(TOPIC_CMD);
      mqtt.subscribe(TOPIC_CONFIG);
      publishStatus("inicijalizacija");
    } else {
      Serial.printf(" greska rc=%d, ponovo za 2 s\n", mqtt.state());
      delay(2000);
      tries++;
    }
  }
}

// ================= TELEMETRIJA ===============================
void publishTelemetry() {
  StaticJsonDocument<256> doc;
  doc["soil"] = soilPct;
  doc["raw"]  = rawAdc;
  doc["temp"] = isnan(temperature) ? 0 : round(temperature * 10) / 10.0;
  doc["hum"]  = isnan(humidity)    ? 0 : round(humidity * 10) / 10.0;
  doc["pump"] = pumpOn;
  doc["mode"] = autoMode ? "AUTO" : "MANUAL";
  doc["dry"]  = thDry;
  doc["wet"]  = thWet;
  doc["up_s"] = millis() / 1000;

  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqtt.publish(TOPIC_TELEMETRY, buf, n);
  Serial.printf("[MQTT TX] %s\n", buf);
}

// ================= THINGSPEAK ================================
void sendThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String(TS_URL) + "?api_key=" + TS_API_KEY
             + "&field1=" + String(soilPct)
             + "&field2=" + String(isnan(temperature) ? 0 : temperature, 1)
             + "&field3=" + String(isnan(humidity) ? 0 : humidity, 1)
             + "&field4=" + String(pumpOn ? 1 : 0);
  http.begin(url);
  int code = http.GET();
  Serial.printf("[ThingSpeak] HTTP %d\n", code);
  http.end();
}

// ================= LOGIKA UPRAVLJANJA ========================
void controlLogic() {
  // temperaturni alarm
  digitalWrite(PIN_ALARM, (!isnan(temperature) && temperature > TEMP_ALARM) ? HIGH : LOW);

  // sigurnosni timeout pumpe (radi u oba rezima)
  if (pumpOn && millis() - pumpStart > PUMP_MAX_MS) {
    setPump(false, "sigurnosni timeout");
    return;
  }
  if (!autoMode) return;

  if (!pumpOn && soilPct < thDry)      setPump(true,  "vlaga ispod donjeg praga");
  else if (pumpOn && soilPct >= thWet) setPump(false, "vlaga iznad gornjeg praga");
}

// ================= SETUP / LOOP ==============================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_ALARM, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
  digitalWrite(PIN_ALARM, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SOIL, ADC_11db);  // puni raspon 0–3.3 V
  dht.begin();

  connectWiFi();
  connectMqtt();
  Serial.println("Sistem spreman.\n");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  unsigned long now = millis();

  if (now - tSample >= T_SAMPLE) {
    tSample = now;
    soilPct = readSoilPercent();

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temperature = t;
    if (!isnan(h)) humidity = h;

    Serial.printf("Vlaga tla: %d%% | T: %.1f C | RH: %.1f%% | pumpa: %s | rezim: %s\n",
                  soilPct, temperature, humidity,
                  pumpOn ? "ON" : "OFF", autoMode ? "AUTO" : "MANUAL");
    controlLogic();
  }

  if (now - tMqtt >= T_MQTT && mqtt.connected()) { tMqtt = now; publishTelemetry(); }
  if (now - tTs   >= T_TS)                       { tTs   = now; sendThingSpeak();   }
}
