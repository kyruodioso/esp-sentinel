/**
 * Sentinel Node Firmware v2.5 - Final Production Version
 * Architecture: ESP8266 | Board: NodeMCU v2
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <vector>

// --- CONFIGURATION ---
#define MQTT_BROKER "broker.hivemq.com"
#define MQTT_PORT 1883
#define DATA_TOPIC_PREFIX "sentinel/v1/data/"
#define CONFIG_FILE "/config.json"
#define SENSORS_FILE "/sensors.json"
#define MEASUREMENT_INTERVAL 30000

// --- DATA STRUCTURES ---
enum SensorType {
  TYPE_DHT11_TEMP,
  TYPE_DHT11_HUM,
  TYPE_DHT22_TEMP,
  TYPE_DHT22_HUM,
  TYPE_ANALOG
};

struct Sensor {
  String id;
  int pin;
  String unit;
  SensorType type;
};

// --- GLOBAL VARIABLES ---
std::vector<Sensor> sensors;
char sentinel_token[40] = "";
char device_name[40] = "ESP8266_SENTINEL_DYN";
char mqtt_host[64] = "broker.hivemq.com";
char mqtt_port_str[6] = "1883";
char mqtt_user[32] = "";
char mqtt_pass[32] = "";

unsigned long lastMsg = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
ESP8266WebServer server(80);
DHTesp dht;

// --- PIN MAPPING ---
int getGpio(String pinStr) {
  if (pinStr == "A0")
    return A0;
  if (pinStr == "D0")
    return 16;
  if (pinStr == "D1")
    return 5;
  if (pinStr == "D2")
    return 4;
  if (pinStr == "D3")
    return 0;
  if (pinStr == "D4")
    return 2;
  if (pinStr == "D5")
    return 14;
  if (pinStr == "D6")
    return 12;
  if (pinStr == "D7")
    return 13;
  if (pinStr == "D8")
    return 15;
  return pinStr.toInt();
}

// --- PERSISTENCE ---
void loadConfig() {
  if (!LittleFS.begin())
    return;
  if (LittleFS.exists(CONFIG_FILE)) {
    File f = LittleFS.open(CONFIG_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    strcpy(sentinel_token, doc["token"] | "");
    strcpy(device_name, doc["name"] | "ESP8266_SENTINEL_DYN");
    strcpy(mqtt_host, doc["mqtt_host"] | "broker.hivemq.com");
    strcpy(mqtt_port_str, doc["mqtt_port"] | "1883");
    strcpy(mqtt_user, doc["mqtt_user"] | "");
    strcpy(mqtt_pass, doc["mqtt_pass"] | "");
    f.close();
  }
  if (LittleFS.exists(SENSORS_FILE)) {
    File f = LittleFS.open(SENSORS_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    JsonArray arr = doc.as<JsonArray>();
    sensors.clear();
    for (JsonObject obj : arr) {
      sensors.push_back({obj["id"].as<String>(), obj["pin"].as<int>(),
                         obj["unit"].as<String>(),
                         (SensorType)obj["type"].as<int>()});
    }
    f.close();
  }
}

void saveSensors() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &s : sensors) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = s.id;
    obj["pin"] = s.pin;
    obj["unit"] = s.unit;
    obj["type"] = (int)s.type;
  }
  File f = LittleFS.open(SENSORS_FILE, "w");
  serializeJson(doc, f);
  f.close();
}

void saveConfig() {
  JsonDocument doc;
  doc["token"] = sentinel_token;
  doc["name"] = device_name;
  doc["mqtt_host"] = mqtt_host;
  doc["mqtt_port"] = mqtt_port_str;
  doc["mqtt_user"] = mqtt_user;
  doc["mqtt_pass"] = mqtt_pass;
  File f = LittleFS.open(CONFIG_FILE, "w");
  serializeJson(doc, f);
  f.close();
}

// --- CORE LOGIC ---
float readSensorValue(const Sensor &s) {
  static int lastPin = -1;
  static unsigned long lastReadTime = 0;
  static float lastTemp = NAN;
  static float lastHum = NAN;

  // Cache logic: If reading from the same pin within 2 seconds, use cached data
  if (s.pin != A0 && s.pin == lastPin && (millis() - lastReadTime < 2000)) {
    if (s.type == TYPE_DHT11_TEMP || s.type == TYPE_DHT22_TEMP)
      return lastTemp;
    if (s.type == TYPE_DHT11_HUM || s.type == TYPE_DHT22_HUM)
      return lastHum;
  }

  float val = NAN;
  if (s.type == TYPE_DHT11_TEMP || s.type == TYPE_DHT11_HUM) {
    dht.setup(s.pin, DHTesp::DHT11);
    delay(100);
    TempAndHumidity th = dht.getTempAndHumidity();
    if (dht.getStatus() == DHTesp::ERROR_NONE) {
      lastTemp = th.temperature;
      lastHum = th.humidity;
      lastPin = s.pin;
      lastReadTime = millis();
    }
    val = (s.type == TYPE_DHT11_TEMP) ? lastTemp : lastHum;
  } else if (s.type == TYPE_DHT22_TEMP || s.type == TYPE_DHT22_HUM) {
    dht.setup(s.pin, DHTesp::DHT22);
    delay(100);
    TempAndHumidity th = dht.getTempAndHumidity();
    if (dht.getStatus() == DHTesp::ERROR_NONE) {
      lastTemp = th.temperature;
      lastHum = th.humidity;
      lastPin = s.pin;
      lastReadTime = millis();
    }
    val = (s.type == TYPE_DHT22_TEMP) ? lastTemp : lastHum;
  } else if (s.type == TYPE_ANALOG) {
    val = analogRead(s.pin);
  }

  return val;
}

// --- WEB SERVER ---
void handleRoot() {
  String html =
      "<html><head><meta name='viewport' content='width=device-width, "
      "initial-scale=1'><meta charset='UTF-8'>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', system-ui, sans-serif; background: "
          "linear-gradient(135deg, #1d4e89 0%, #00b295 100%); min-height: "
          "100vh; margin: 0; padding: 20px; color: white; display: flex; "
          "flex-direction: column; align-items: center; }";
  html += ".glass { background: rgba(255, 255, 255, 0.1); backdrop-filter: "
          "blur(12px); -webkit-backdrop-filter: blur(12px); border: 1px solid "
          "rgba(255, 255, 255, 0.2); border-radius: 20px; box-shadow: 0 8px "
          "32px 0 rgba(0, 0, 0, 0.37); padding: 25px; margin-bottom: 25px; "
          "width: 100%; max-width: 450px; box-sizing: border-box; }";
  html += "h1 { font-weight: 300; letter-spacing: 2px; text-transform: "
          "uppercase; margin: 0 0 5px 0; text-align: center; }";
  html += ".dev-name { font-size: 14px; opacity: 0.8; margin-bottom: 30px; "
          "letter-spacing: 1px; }";
  html +=
      "h3 { margin-top: 0; font-weight: 400; color: #a2ffd1; border-bottom: "
      "1px solid rgba(255,255,255,0.1); padding-bottom: 10px; display: flex; "
      "justify-content: space-between; align-items: center; }";
  html += ".live-val { background: #a2ffd1; color: #1d4e89; padding: 2px 8px; "
          "border-radius: 20px; font-weight: bold; font-size: 14px; }";
  html += "ul { list-style: none; padding: 0; }";
  html += "li { background: rgba(0,0,0,0.2); padding: 12px 15px; "
          "border-radius: 10px; margin-bottom: 10px; display: flex; "
          "justify-content: space-between; align-items: center; border: 1px "
          "solid rgba(255,255,255,0.05); }";
  html +=
      "input, select { width: 100%; padding: 12px; margin: 10px 0; background: "
      "rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.2); "
      "border-radius: 10px; color: white; outline: none; }";
  html += "button { width: 100%; padding: 12px; margin-top: 10px; border: "
          "none; border-radius: 10px; cursor: pointer; font-weight: 600; "
          "text-transform: uppercase; transition: 0.3s; }";
  html += ".btn-add { background: linear-gradient(90deg, #2ecc71, #27ae60); "
          "color: white; }";
  html += ".btn-token { background: linear-gradient(90deg, #f39c12, #e67e22); "
          "color: white; }";
  html += ".btn-reboot { background: linear-gradient(90deg, #3498db, #2980b9); "
          "color: white; }";
  html += ".btn-wifi { background: linear-gradient(90deg, #9b59b6, #8e44ad); "
          "color: white; }";
  html += ".btn-del { width: auto; padding: 5px 12px; margin: 0; background: "
          "#e74c3c; font-size: 11px; }";
  html += "option { background: #1d4e89; color: white; }";
  html += "small { display: block; margin-top: 10px; color: #a2ffd1; "
          "font-weight: bold; font-size: 10px; text-transform: uppercase; }";
  html += "</style>";
  html += "<script>function toggle(id){ var x = document.getElementById(id); "
          "x.style.display = (x.style.display==='block')?'none':'block'; "
          "}</script>";
  html += "</head><body>";
  html += "<h1>Sentinel Node</h1><div class='dev-name'>" + String(device_name) +
          "</div>";

  // LIVE MONITOR
  html += "<div class='glass'><h3>Live Monitor <span "
          "class='live-val'>Real-time</span></h3><ul>";
  if (sensors.empty())
    html +=
        "<p style='opacity:0.5; font-size:13px;'>No sensors configured.</p>";
  for (size_t i = 0; i < sensors.size(); i++) {
    float val = readSensorValue(sensors[i]);
    String valStr = isnan(val) ? "--" : String(val);
    html += "<li><div><b>" + sensors[i].id +
            "</b></div><div style='color:#a2ffd1; font-weight:bold'>" + valStr +
            " " + sensors[i].unit + " </div>";
    html += "<a href='/delete?index=" + String(i) +
            "'><button class='btn-del'>Del</button></a></li>";
  }
  html += "</ul><small style='opacity:0.5'>Refresh page to update "
          "readings</small></div>";

  // ADD SENSOR
  html += "<div class='glass'><h3>Add New Sensor</h3>";
  html += "<form action='/add' method='GET'>";
  html += "<input name='id' placeholder='Sensor ID (e.g. TEMP_01)'>";
  html += "<select name='pin'><option value='A0'>A0 (Analog)</option><option "
          "value='D1'>D1</option><option value='D2'>D2</option><option "
          "value='D5'>D5</option><option value='D6'>D6</option></select>";
  html += "<select name='type'>";
  html += "<option value='0'>DHT11 Temperature (Blue)</option>";
  html += "<option value='1'>DHT11 Humidity (Blue)</option>";
  html += "<option value='2'>DHT22 Temperature (White)</option>";
  html += "<option value='3'>DHT22 Humidity (White)</option>";
  html += "<option value='4'>Analog Raw</option></select>";
  html += "<input name='unit' placeholder='Unit (e.g. C, %, v)'>";
  html += "<button type='submit' class='btn-add'>+ Add "
          "Sensor</button></form></div>";

  // SYSTEM CONFIG
  html += "<div class='glass'><h3>System Settings</h3>";
  html += "<button onclick=\"toggle('set-form')\" class='btn-token'>Configure "
          "Node & MQTT</button>";
  html += "<div id='set-form' style='display:none; margin-top:15px; "
          "border-top:1px solid rgba(255,255,255,0.1); padding-top:15px;'>";
  html += "<form action='/save_sys' method='GET'>";

  html += "<small>Device Name</small><input name='name' value='" +
          String(device_name) + "'>";
  html += "<small>Sentinel Token</small><input type='password' name='token' "
          "value='" +
          String(sentinel_token) + "'>";

  html += "<div style='margin-top:20px; padding:10px; "
          "background:rgba(0,0,0,0.2); border-radius:10px;'>";
  html += "<p style='font-size:12px; margin:0 0 10px 0; color:#f39c12;'>MQTT "
          "Broker Settings</p>";
  html +=
      "<small>Host</small><input name='mh' value='" + String(mqtt_host) + "'>";
  html += "<small>Port</small><input name='mp' value='" +
          String(mqtt_port_str) + "'>";
  html +=
      "<small>User</small><input name='mu' value='" + String(mqtt_user) + "'>";
  html += "<small>Password</small><input type='password' name='mpx' value='" +
          String(mqtt_pass) + "'>";
  html += "</div>";

  html += "<button type='submit' style='background:#d35400; color:white; "
          "margin-top:20px;'>Save "
          "Changes</button></form></div></div>";

  // FOOTER
  html += "<div style='width: 100%; max-width: 450px;'>";
  html +=
      "<a href='/reboot'><button class='btn-reboot'>Reboot Node</button></a>";
  html += "<a href='/reset_wifi'><button class='btn-wifi'>Reset WiFi "
          "Connection</button></a></div>";
  html += "<p style='font-size: 11px; opacity: 0.5; margin-top: "
          "20px;'>Firmware v2.6 | Sentinel Project | Closed Hardware Ready</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSaveSys() {
  strncpy(device_name, server.arg("name").c_str(), 39);
  strncpy(sentinel_token, server.arg("token").c_str(), 39);
  strncpy(mqtt_host, server.arg("mh").c_str(), 63);
  strncpy(mqtt_port_str, server.arg("mp").c_str(), 5);
  strncpy(mqtt_user, server.arg("mu").c_str(), 31);
  strncpy(mqtt_pass, server.arg("mpx").c_str(), 31);

  saveConfig();

  // Reconfigure MQTT client immediately
  mqttClient.setServer(mqtt_host, atoi(mqtt_port_str));
  mqttClient.disconnect(); // Force reconnection in loop

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleAdd() {
  sensors.push_back({server.arg("id"), getGpio(server.arg("pin")),
                     server.arg("unit"),
                     (SensorType)server.arg("type").toInt()});
  saveSensors();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleDelete() {
  int i = server.arg("index").toInt();
  if (i >= 0 && i < (int)sensors.size()) {
    sensors.erase(sensors.begin() + i);
    saveSensors();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleResetWiFi() {
  server.send(200, "text/plain", "WiFi reset. Node will restart into AP mode.");
  delay(1000);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// --- MQTT ---
void reconnectMQTT() {
  static unsigned long lastRetry = 0;
  if (!mqttClient.connected() && millis() - lastRetry > 10000) {
    lastRetry = millis();
    Serial.print("Connecting to MQTT at ");
    Serial.print(mqtt_host);
    Serial.print("...");

    bool connected = false;
    if (strlen(mqtt_user) > 0) {
      connected = mqttClient.connect(device_name, mqtt_user, mqtt_pass);
    } else {
      connected = mqttClient.connect(device_name);
    }

    if (connected) {
      Serial.println("Success!");
    } else {
      Serial.print("Failed, rc=");
      Serial.println(mqttClient.state());
    }
  }
}

void collectAndPublish() {
  if (strlen(sentinel_token) < 5)
    return;
  JsonDocument doc;
  doc["token"] = sentinel_token;
  doc["device_id"] = device_name; // Consistency with backend
  JsonArray readings = doc["readings"].to<JsonArray>();
  for (const auto &s : sensors) {
    float val = readSensorValue(s);
    if (!isnan(val)) {
      JsonObject r = readings.add<JsonObject>();
      r["sensor_id"] = s.id;
      r["value"] = val;
      r["unit"] = s.unit;
    }
  }
  doc["ip"] = WiFi.localIP().toString();
  char buffer[1024];
  serializeJson(doc, buffer);
  char topic[100];
  snprintf(topic, sizeof(topic), "sentinel/v1/data/%s", sentinel_token);
  mqttClient.publish(topic, buffer);
  Serial.println("Data published to MQTT.");
}

// --- APP ---
void setup() {
  Serial.begin(115200);
  loadConfig();
  WiFiManager wm;
  if (!wm.autoConnect("Sentinel_Node_AP"))
    ESP.restart();

  server.on("/", handleRoot);
  server.on("/add", handleAdd);
  server.on("/delete", handleDelete);
  server.on("/save_sys", handleSaveSys);
  server.on("/reset_wifi", handleResetWiFi);
  server.on("/reboot", []() {
    server.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });
  server.begin();

  mqttClient.setServer(mqtt_host, atoi(mqtt_port_str));
  Serial.println("Sentinel Node Ready.");
}

void loop() {
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected())
      reconnectMQTT();
    mqttClient.loop();
    if (millis() - lastMsg > MEASUREMENT_INTERVAL) {
      lastMsg = millis();
      collectAndPublish();
    }
  }
}
