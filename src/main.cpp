// Altrobyte OTA client for ESP32 (MQTT-triggered)
// Two ways to update: the web tool uploads the .bin in chunks over MQTT, or
// it sends an https URL that this board downloads itself.
// Libraries: PubSubClient, ArduinoJson (install from Library Manager)
// Tools > Partition Scheme must be an OTA one (e.g. "Default 4MB with spiffs").
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "Munira's Galaxy";
const char* WIFI_PASS = "Itsmunira";
const char* MQTT_HOST = "broker.hivemq.com";   // plain TCP port 1883
const int   MQTT_PORT = 1883;
const char* BASE      = "altrobyte/device1";   // same base topic as the web tool
const char* FW_VERSION = "2.0.0";

WiFiClient net;
PubSubClient mqtt(net);
String cmdTopic, dataTopic, ackTopic, statusTopic;

// State of an in-progress upload-over-MQTT
bool rxActive = false;
size_t rxSize = 0, rxWritten = 0;
uint32_t rxSeq = 0;
int rxLastPct = -1;

void publishStatus(const char* state, int progress, const char* msg = "") {
  JsonDocument d;
  d["state"] = state;
  d["progress"] = progress;
  d["version"] = FW_VERSION;
  d["msg"] = msg;
  char buf[200];
  serializeJson(d, buf);
  mqtt.publish(statusTopic.c_str(), buf);
}

void ack(const String& s) { mqtt.publish(ackTopic.c_str(), s.c_str()); }

// ---- Mode 1: device downloads the .bin from an https URL ----
void doUrlOta(const String& url) {
  publishStatus("downloading", 0);
  WiFiClientSecure secure;
  secure.setInsecure();  // demo only: pin a CA certificate in production
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([](int cur, int total) {
    static int last = -1;
    int pct = total ? (cur * 100) / total : 0;
    if (pct != last && pct % 5 == 0) { last = pct; publishStatus("flashing", pct); mqtt.loop(); }
  });
  t_httpUpdate_return r = httpUpdate.update(secure, url);
  if (r == HTTP_UPDATE_OK) {
    publishStatus("done", 100, "rebooting");
    mqtt.loop();
    delay(500);
    ESP.restart();
  } else {
    publishStatus("failed", 0, httpUpdate.getLastErrorString().c_str());
  }
}

// ---- Mode 2: the web tool streams the file to us over MQTT ----
void startUpload(size_t size) {
  if (size == 0 || !Update.begin(size)) {
    publishStatus("failed", 0, "not enough space / bad size");
    return;
  }
  rxActive = true;
  rxSize = size;
  rxWritten = 0;
  rxSeq = 0;
  rxLastPct = -1;
  publishStatus("receiving", 0);
  ack("ready");
}

void onData(byte* p, unsigned int len) {
  if (!rxActive || len < 4) return;
  uint32_t seq = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  if (seq < rxSeq) { ack(String(seq)); return; }  // duplicate: re-ack, don't rewrite
  if (seq > rxSeq) return;                         // gap: the web tool will resend
  size_t n = len - 4;
  if (Update.write(p + 4, n) != n) {
    Update.abort();
    rxActive = false;
    publishStatus("failed", 0, "flash write failed");
    return;
  }
  rxWritten += n;
  rxSeq++;
  ack(String(seq));
  int pct = (int)((rxWritten * 100) / rxSize);
  if (pct / 10 != rxLastPct / 10) { rxLastPct = pct; publishStatus("receiving", pct); }
  if (rxWritten >= rxSize) {
    rxActive = false;
    if (Update.end(true)) {
      publishStatus("done", 100, "rebooting");
      mqtt.loop();
      delay(500);
      ESP.restart();
    } else {
      publishStatus("failed", 0, Update.errorString());
    }
  }
}

void onCmd(byte* payload, unsigned int len) {
  JsonDocument d;
  if (deserializeJson(d, payload, len)) { publishStatus("failed", 0, "bad json"); return; }
  String mode = d["mode"] | "url";
  String version = d["version"] | "";
  if (version.length() && version == FW_VERSION) { publishStatus("idle", 100, "already on this version"); return; }
  if (mode == "mqtt") { startUpload(d["size"] | 0); return; }
  String url = d["url"] | "";
  if (url.isEmpty()) { publishStatus("failed", 0, "no url"); return; }
  doUrlOta(url);
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  if (dataTopic == topic) onData(payload, len);
  else if (cmdTopic == topic) onCmd(payload, len);
}

void reconnect() {
  while (!mqtt.connected()) {
    String id = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(id.c_str())) {
      mqtt.subscribe(cmdTopic.c_str());
      mqtt.subscribe(dataTopic.c_str());
      publishStatus("idle", 100, "online");
    } else delay(2000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\nBooting, firmware %s\n", FW_VERSION);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - t0 > 20000) {
      Serial.printf("\nWiFi failed (status %d). Check SSID/password and use 2.4 GHz.\n", WiFi.status());
      t0 = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
  Serial.printf("\nWiFi OK, IP %s\n", WiFi.localIP().toString().c_str());
  cmdTopic = String(BASE) + "/ota/cmd";
  dataTopic = String(BASE) + "/ota/data";
  ackTopic = String(BASE) + "/ota/ack";
  statusTopic = String(BASE) + "/ota/status";
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(8192 + 64);  // one 8 KB firmware chunk + header
  Serial.printf("Running firmware %s\n", FW_VERSION);
}

void loop() {
  if (!mqtt.connected()) reconnect();
  mqtt.loop();
  // ...your normal application code here...
}