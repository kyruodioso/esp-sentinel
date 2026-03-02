/**
 * Sentinel Node Firmware v3.0 - Actuator Support
 * Architecture: ESP8266 | Board: NodeMCU v2
 *
 * Changelog v3.0:
 *  - Actuator support (electrovalves, relays) with safety auto-shutoff timer
 *  - MQTT command subscriber: sentinel/v1/cmd/<token>
 *    Commands: {"action":"irrigate","duration":15} | {"action":"stop"} |
 * {"action":"toggle","actuator_id":"VALVE_01"}
 *  - Actuator status reports published to sentinel/v1/status/<token>
 *  - Web UI for actuator management
 *  - LittleFS persistence for actuator config
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <vector>

// --- CONFIGURATION ---
#define MQTT_BROKER "broker.hivemq.com"
#define MQTT_PORT 1883
#define DATA_TOPIC_PREFIX "sentinel/v1/data/"
#define CMD_TOPIC_PREFIX "sentinel/v1/cmd/"
#define STATUS_TOPIC_PREFIX "sentinel/v1/status/"
#define CONFIG_FILE "/config.json"
#define SENSORS_FILE "/sensors.json"
#define ACTUATORS_FILE "/actuators.json"
#define MEASUREMENT_INTERVAL 30000
#define STATUS_INTERVAL 15000 // Publicar estado de actuadores cada 15s

// --- ACTUATOR TYPES ---
enum ActuatorType {
  ACT_RELAY_NO, // Relay Normally Open  (HIGH = activado)
  ACT_RELAY_NC, // Relay Normally Closed (LOW = activado)
  ACT_DIGITAL   // Digital output genérico (HIGH = activado)
};

// --- SENSOR TYPES ---
enum SensorType {
  TYPE_DHT11_TEMP,
  TYPE_DHT11_HUM,
  TYPE_DHT22_TEMP,
  TYPE_DHT22_HUM,
  TYPE_ANALOG
};

// --- DATA STRUCTURES ---
struct Sensor {
  String id;
  int pin;
  String unit;
  SensorType type;
};

struct Actuator {
  String id; // Ej: "VALVE_01", "PUMP_01"
  int pin;   // GPIO físico
  ActuatorType type;
  bool state; // true = activo/encendido
  unsigned long
      timer_ms; // 0 = sin timer. >0 = apagar en timer_ms ms desde activación
  unsigned long activated_at; // millis() cuando se activó
};

// --- GLOBAL VARIABLES ---
std::vector<Sensor> sensors;
std::vector<Actuator> actuators;

char sentinel_token[40] = "";
char device_name[40] = "ESP8266_SENTINEL_DYN";
char mqtt_host[64] = "broker.hivemq.com";
char mqtt_port_str[6] = "1883";
char mqtt_user[32] = "";
char mqtt_pass[32] = "";

unsigned long lastMsg = 0;
unsigned long lastStatus = 0;

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

// ============================================================
// ACTUATOR LOGIC
// ============================================================

/** Activa un actuador. Si duration_ms > 0 configura apagado automático. */
void activateActuator(Actuator &a, unsigned long duration_ms = 0) {
  a.state = true;
  a.timer_ms = duration_ms;
  a.activated_at = millis();

  // Para relay NC, LOW activa; para NO y DIGITAL, HIGH activa
  int writeVal = (a.type == ACT_RELAY_NC) ? LOW : HIGH;
  pinMode(a.pin, OUTPUT);
  digitalWrite(a.pin, writeVal);

  Serial.print("✅ Actuator ON: ");
  Serial.print(a.id);
  if (duration_ms > 0) {
    Serial.print(" | Auto-OFF in ");
    Serial.print(duration_ms / 1000);
    Serial.println("s");
  } else {
    Serial.println(" (indefinite)");
  }
}

/** Desactiva un actuador inmediatamente. */
void deactivateActuator(Actuator &a) {
  a.state = false;
  a.timer_ms = 0;

  int writeVal = (a.type == ACT_RELAY_NC) ? HIGH : LOW;
  pinMode(a.pin, OUTPUT);
  digitalWrite(a.pin, writeVal);

  Serial.print("⛔ Actuator OFF: ");
  Serial.println(a.id);
}

/** Inicializa todos los pines de actuadores en estado seguro (apagado). */
void initActuators() {
  for (auto &a : actuators) {
    a.state = false;
    a.timer_ms = 0;
    // Relay NC → HIGH = apagado. Relay NO / DIGITAL → LOW = apagado
    int safeVal = (a.type == ACT_RELAY_NC) ? HIGH : LOW;
    pinMode(a.pin, OUTPUT);
    digitalWrite(a.pin, safeVal);
  }
}

/**
 * Safety Watchdog: apaga automáticamente actuadores con timer vencido.
 * Se llama en cada iteración de loop().
 */
void checkActuatorTimers() {
  for (auto &a : actuators) {
    if (a.state && a.timer_ms > 0) {
      if (millis() - a.activated_at >= a.timer_ms) {
        Serial.print("⏰ Auto-shutoff: ");
        Serial.println(a.id);
        deactivateActuator(a);
      }
    }
  }
}

// ============================================================
// PERSISTENCE
// ============================================================

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

  if (LittleFS.exists(ACTUATORS_FILE)) {
    File f = LittleFS.open(ACTUATORS_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    JsonArray arr = doc.as<JsonArray>();
    actuators.clear();
    for (JsonObject obj : arr) {
      Actuator a;
      a.id = obj["id"].as<String>();
      a.pin = obj["pin"].as<int>();
      a.type = (ActuatorType)obj["type"].as<int>();
      a.state = false;
      a.timer_ms = 0;
      a.activated_at = 0;
      actuators.push_back(a);
    }
    f.close();
  }
}

void saveActuators() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &a : actuators) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = a.id;
    obj["pin"] = a.pin;
    obj["type"] = (int)a.type;
  }
  File f = LittleFS.open(ACTUATORS_FILE, "w");
  serializeJson(doc, f);
  f.close();
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

// ============================================================
// SENSOR READING
// ============================================================

float readSensorValue(const Sensor &s) {
  static int lastPin = -1;
  static unsigned long lastReadTime = 0;
  static float lastTemp = NAN;
  static float lastHum = NAN;

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

// ============================================================
// MQTT — COMMAND HANDLER
// ============================================================

/**
 * Procesa comandos recibidos en sentinel/v1/cmd/<token>
 *
 * Formatos soportados:
 *  {"action":"irrigate","duration":15}          → todos los actuadores (15 min)
 *  {"action":"irrigate","duration":10,"actuator_id":"VALVE_01"} → solo VALVE_01
 *  {"action":"stop"}                             → detiene todos
 *  {"action":"stop","actuator_id":"VALVE_01"}   → detiene solo VALVE_01
 *  {"action":"toggle","actuator_id":"PUMP_01"}  → invierte estado de PUMP_01
 *  {"action":"status"}                           → publica estado actual
 */
void handleCommand(const char *payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("❌ Invalid command JSON");
    return;
  }

  String action = doc["action"].as<String>();
  String actuator_id = doc["actuator_id"] | "";
  int duration = doc["duration"] | 0; // minutos
  unsigned long duration_ms = (unsigned long)duration * 60UL * 1000UL;

  Serial.print("🎮 CMD: action=");
  Serial.print(action);
  if (actuator_id.length()) {
    Serial.print(" actuator=");
    Serial.print(actuator_id);
  }
  if (duration) {
    Serial.print(" duration=");
    Serial.print(duration);
    Serial.print("min");
  }
  Serial.println();

  if (action == "irrigate" || action == "on") {
    for (auto &a : actuators) {
      if (actuator_id.length() == 0 || a.id == actuator_id) {
        activateActuator(a, duration_ms);
      }
    }

  } else if (action == "stop" || action == "off") {
    for (auto &a : actuators) {
      if (actuator_id.length() == 0 || a.id == actuator_id) {
        deactivateActuator(a);
      }
    }

  } else if (action == "toggle") {
    for (auto &a : actuators) {
      if (a.id == actuator_id) {
        if (a.state)
          deactivateActuator(a);
        else
          activateActuator(a, duration_ms);
      }
    }

  } else if (action == "status") {
    // Responder con un publish de estado (se hará en publishStatus)
    lastStatus = 0; // Forzar publicación inmediata

  } else {
    Serial.print("⚠️ Unknown action: ");
    Serial.println(action);
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  payload[length] = '\0';
  String topicStr = String(topic);
  Serial.print("📥 MQTT ← [");
  Serial.print(topicStr);
  Serial.print("] ");
  Serial.println((char *)payload);

  // Solo procesar si el tópico es de comandos para este token
  String cmdTopic = String(CMD_TOPIC_PREFIX) + String(sentinel_token);
  if (topicStr == cmdTopic) {
    handleCommand((char *)payload);
  }
}

// ============================================================
// MQTT — PUBLISH
// ============================================================

/** Publica estado de todos los actuadores en sentinel/v1/status/<token> */
void publishStatus() {
  if (strlen(sentinel_token) < 5)
    return;

  JsonDocument doc;
  doc["device_id"] = device_name;
  doc["token"] = sentinel_token;
  JsonArray acts = doc["actuators"].to<JsonArray>();
  for (const auto &a : actuators) {
    JsonObject obj = acts.add<JsonObject>();
    obj["id"] = a.id;
    obj["state"] = a.state;
    if (a.state && a.timer_ms > 0) {
      unsigned long elapsed = millis() - a.activated_at;
      obj["remaining_s"] =
          (a.timer_ms > elapsed) ? (a.timer_ms - elapsed) / 1000 : 0;
    }
  }

  char buffer[512];
  serializeJson(doc, buffer);
  char topic[100];
  snprintf(topic, sizeof(topic), "%s%s", STATUS_TOPIC_PREFIX, sentinel_token);
  mqttClient.publish(topic, buffer);
}

/** Publica lecturas de sensores y estado de actuadores */
void collectAndPublish() {
  if (strlen(sentinel_token) < 5)
    return;

  JsonDocument doc;
  doc["token"] = sentinel_token;
  doc["device_id"] = device_name;
  doc["ip"] = WiFi.localIP().toString();

  // 1. Lecturas de Sensores
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

  // 2. Estado de Actuadores (para autodescubrimiento en backend)
  JsonArray acts = doc["actuators"].to<JsonArray>();
  for (const auto &a : actuators) {
    JsonObject obj = acts.add<JsonObject>();
    obj["id"] = a.id;
    obj["state"] = a.state;
  }

  char buffer[1536]; // Aumentado para soportar múltiples sensores y actuadores
  serializeJson(doc, buffer);

  char topic[100];
  snprintf(topic, sizeof(topic), "%s%s", DATA_TOPIC_PREFIX, sentinel_token);
  mqttClient.publish(topic, buffer);
  Serial.println("📤 Sensor & Actuator data published.");
}

// ============================================================
// MQTT — CONNECTION
// ============================================================

void reconnectMQTT() {
  static unsigned long lastRetry = 0;
  if (!mqttClient.connected() && millis() - lastRetry > 10000) {
    lastRetry = millis();
    Serial.print("Connecting MQTT → ");
    Serial.print(mqtt_host);
    Serial.print("...");

    bool connected = strlen(mqtt_user) > 0
                         ? mqttClient.connect(device_name, mqtt_user, mqtt_pass)
                         : mqttClient.connect(device_name);

    if (connected) {
      Serial.println(" OK");
      // Suscribirse al tópico de comandos
      char cmdTopic[100];
      snprintf(cmdTopic, sizeof(cmdTopic), "%s%s", CMD_TOPIC_PREFIX,
               sentinel_token);
      mqttClient.subscribe(cmdTopic);
      Serial.print("🎮 Subscribed to: ");
      Serial.println(cmdTopic);
    } else {
      Serial.print(" FAIL rc=");
      Serial.println(mqttClient.state());
    }
  }
}

// ============================================================
// WEB SERVER
// ============================================================

void handleRoot() {
  String html =
      "<html><head><meta name='viewport' "
      "content='width=device-width,initial-scale=1'><meta charset='UTF-8'>";
  html += "<style>";
  html += "body{font-family:'Segoe "
          "UI',system-ui,sans-serif;background:linear-gradient(135deg,#1d4e89 "
          "0%,#00b295 "
          "100%);min-height:100vh;margin:0;padding:20px;color:white;display:"
          "flex;flex-direction:column;align-items:center;}";
  html += ".glass{background:rgba(255,255,255,.1);backdrop-filter:blur(12px);"
          "border:1px solid "
          "rgba(255,255,255,.2);border-radius:20px;box-shadow:0 8px 32px 0 "
          "rgba(0,0,0,.37);padding:25px;margin-bottom:25px;width:100%;max-"
          "width:450px;box-sizing:border-box;}";
  html += "h1{font-weight:300;letter-spacing:2px;text-transform:uppercase;"
          "margin:0 0 5px 0;text-align:center;}";
  html += ".dev-name{font-size:14px;opacity:.8;margin-bottom:30px;letter-"
          "spacing:1px;}";
  html +=
      "h3{margin-top:0;font-weight:400;color:#a2ffd1;border-bottom:1px solid "
      "rgba(255,255,255,.1);padding-bottom:10px;display:flex;justify-content:"
      "space-between;align-items:center;}";
  html += ".live-val{background:#a2ffd1;color:#1d4e89;padding:2px "
          "8px;border-radius:20px;font-weight:bold;font-size:14px;}";
  html += ".act-on{background:#2ecc71;color:white;padding:2px "
          "10px;border-radius:20px;font-weight:bold;font-size:12px;}";
  html += ".act-off{background:#e74c3c;color:white;padding:2px "
          "10px;border-radius:20px;font-weight:bold;font-size:12px;}";
  html += "ul{list-style:none;padding:0;}";
  html += "li{background:rgba(0,0,0,.2);padding:12px "
          "15px;border-radius:10px;margin-bottom:10px;display:flex;justify-"
          "content:space-between;align-items:center;border:1px solid "
          "rgba(255,255,255,.05);}";
  html += "input,select{width:100%;padding:12px;margin:10px "
          "0;background:rgba(255,255,255,.05);border:1px solid "
          "rgba(255,255,255,.2);border-radius:10px;color:white;outline:none;}";
  html += "button{width:100%;padding:12px;margin-top:10px;border:none;border-"
          "radius:10px;cursor:pointer;font-weight:600;text-transform:uppercase;"
          "transition:.3s;}";
  html += ".btn-add{background:linear-gradient(90deg,#2ecc71,#27ae60);color:"
          "white;}";
  html += ".btn-act-on{background:linear-gradient(90deg,#00b4d8,#0077b6);color:"
          "white;}";
  html += ".btn-act-off{background:linear-gradient(90deg,#e74c3c,#c0392b);"
          "color:white;}";
  html += ".btn-token{background:linear-gradient(90deg,#f39c12,#e67e22);color:"
          "white;}";
  html += ".btn-reboot{background:linear-gradient(90deg,#3498db,#2980b9);color:"
          "white;}";
  html += ".btn-wifi{background:linear-gradient(90deg,#9b59b6,#8e44ad);color:"
          "white;}";
  html += ".btn-del{width:auto;padding:5px "
          "12px;margin:0;background:#e74c3c;font-size:11px;}";
  html += "option{background:#1d4e89;color:white;}";
  html += "small{display:block;margin-top:10px;color:#a2ffd1;font-weight:bold;"
          "font-size:10px;text-transform:uppercase;}";
  html += "</style>";
  html += "<script>function toggle(id){var "
          "x=document.getElementById(id);x.style.display=(x.style.display==='"
          "block')?'none':'block';}</script>";
  html += "</head><body>";
  html += "<h1>Sentinel Node</h1><div class='dev-name'>" + String(device_name) +
          "</div>";

  // ── LIVE SENSOR MONITOR ──
  html += "<div class='glass'><h3>Live Sensors <span "
          "class='live-val'>Real-time</span></h3><ul>";
  if (sensors.empty())
    html += "<p style='opacity:.5;font-size:13px;'>No sensors configured.</p>";
  for (size_t i = 0; i < sensors.size(); i++) {
    float val = readSensorValue(sensors[i]);
    String valStr = isnan(val) ? "--" : String(val, 1);
    html += "<li><div><b>" + sensors[i].id +
            "</b></div>"
            "<div style='color:#a2ffd1;font-weight:bold'>" +
            valStr + " " + sensors[i].unit +
            "</div>"
            "<a href='/delete_s?index=" +
            String(i) + "'><button class='btn-del'>Del</button></a></li>";
  }
  html +=
      "</ul><small style='opacity:.5'>Refresh to update readings</small></div>";

  // ── ACTUATOR STATUS & CONTROL ──
  html += "<div class='glass'><h3>Actuators <span "
          "class='live-val'>Control</span></h3><ul>";
  if (actuators.empty())
    html +=
        "<p style='opacity:.5;font-size:13px;'>No actuators configured.</p>";
  for (size_t i = 0; i < actuators.size(); i++) {
    String statusBadge = actuators[i].state
                             ? "<span class='act-on'>ON</span>"
                             : "<span class='act-off'>OFF</span>";
    String toggleBtn =
        actuators[i].state
            ? "<a href='/act?index=" + String(i) +
                  "&cmd=off'><button class='btn-act-off' "
                  "style='width:auto;padding:5px "
                  "12px;margin:0;font-size:11px;'>STOP</button></a>"
            : "<a href='/act?index=" + String(i) +
                  "&cmd=on&dur=15'><button class='btn-act-on' "
                  "style='width:auto;padding:5px "
                  "12px;margin:0;font-size:11px;'>ON 15m</button></a>";
    html += "<li><div><b>" + actuators[i].id +
            "</b><br><small "
            "style='display:inline;font-size:10px;opacity:.6;'>Pin " +
            String(actuators[i].pin) +
            "</small></div>"
            "<div style='display:flex;gap:8px;align-items:center'>" +
            statusBadge + toggleBtn + "<a href='/delete_a?index=" + String(i) +
            "'><button class='btn-del'>Del</button></a></div></li>";
  }
  html += "</ul>";

  // Add Actuator Form
  html += "<form action='/add_a' method='GET' "
          "style='margin-top:15px;border-top:1px solid "
          "rgba(255,255,255,.1);padding-top:15px;'>";
  html += "<small>Actuator ID (e.g. VALVE_01, PUMP_01)</small><input name='id' "
          "placeholder='VALVE_01'>";
  html += "<small>GPIO Pin</small><select name='pin'>"
          "<option value='D1'>D1 (GPIO5)</option><option value='D2'>D2 "
          "(GPIO4)</option>"
          "<option value='D5'>D5 (GPIO14)</option><option value='D6'>D6 "
          "(GPIO12)</option>"
          "<option value='D7'>D7 (GPIO13)</option><option value='D8'>D8 "
          "(GPIO15)</option></select>";
  html += "<small>Type</small><select name='type'>"
          "<option value='0'>Relay Normally Open (HIGH=ON)</option>"
          "<option value='1'>Relay Normally Closed (LOW=ON)</option>"
          "<option value='2'>Digital Output Generic</option></select>";
  html += "<button type='submit' class='btn-act-on'>+ Add "
          "Actuator</button></form></div>";

  // ── ADD SENSOR ──
  html += "<div class='glass'><h3>Add Sensor</h3>";
  html += "<form action='/add_s' method='GET'>";
  html += "<input name='id' placeholder='Sensor ID (e.g. TEMP_01)'>";
  html += "<select name='pin'><option value='A0'>A0 (Analog)</option><option "
          "value='D1'>D1</option>"
          "<option value='D2'>D2</option><option value='D5'>D5</option><option "
          "value='D6'>D6</option></select>";
  html += "<select name='type'>"
          "<option value='0'>DHT11 Temperature</option><option value='1'>DHT11 "
          "Humidity</option>"
          "<option value='2'>DHT22 Temperature</option><option value='3'>DHT22 "
          "Humidity</option>"
          "<option value='4'>Analog Raw</option></select>";
  html += "<input name='unit' placeholder='Unit (e.g. C, %, v)'>";
  html += "<button type='submit' class='btn-add'>+ Add "
          "Sensor</button></form></div>";

  // ── SYSTEM SETTINGS ──
  html += "<div class='glass'><h3>System Settings</h3>";
  html += "<button onclick=\"toggle('set-form')\" class='btn-token'>Configure "
          "Node & MQTT</button>";
  html +=
      "<div id='set-form' style='display:none;margin-top:15px;border-top:1px "
      "solid rgba(255,255,255,.1);padding-top:15px;'>";
  html += "<form action='/save_sys' method='GET'>";
  html += "<small>Device Name</small><input name='name' value='" +
          String(device_name) + "'>";
  html += "<small>Sentinel Token</small><input type='password' name='token' "
          "value='" +
          String(sentinel_token) + "'>";
  html += "<div "
          "style='margin-top:20px;padding:10px;background:rgba(0,0,0,.2);"
          "border-radius:10px;'>";
  html += "<p style='font-size:12px;margin:0 0 10px 0;color:#f39c12;'>MQTT "
          "Broker</p>";
  html +=
      "<small>Host</small><input name='mh' value='" + String(mqtt_host) + "'>";
  html += "<small>Port</small><input name='mp' value='" +
          String(mqtt_port_str) + "'>";
  html +=
      "<small>User</small><input name='mu' value='" + String(mqtt_user) + "'>";
  html += "<small>Password</small><input type='password' name='mpx' value='" +
          String(mqtt_pass) + "'>";
  html += "</div>";
  html += "<button type='submit' "
          "style='background:#d35400;color:white;margin-top:20px;'>Save "
          "Changes</button>";
  html += "</form></div></div>";

  // ── FOOTER ──
  html += "<div style='width:100%;max-width:450px;'>";
  html +=
      "<a href='/reboot'><button class='btn-reboot'>Reboot Node</button></a>";
  html += "<a href='/reset_wifi'><button class='btn-wifi'>Reset "
          "WiFi</button></a></div>";
  html += "<p style='font-size:11px;opacity:.5;margin-top:20px;'>Firmware v3.0 "
          "| Sentinel Actuator Support | Solsteinn Innovations</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleAddActuator() {
  Actuator a;
  a.id = server.arg("id");
  a.pin = getGpio(server.arg("pin"));
  a.type = (ActuatorType)server.arg("type").toInt();
  a.state = false;
  a.timer_ms = 0;
  a.activated_at = 0;
  actuators.push_back(a);
  // Inicializar en estado seguro
  int safeVal = (a.type == ACT_RELAY_NC) ? HIGH : LOW;
  pinMode(a.pin, OUTPUT);
  digitalWrite(a.pin, safeVal);
  saveActuators();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleDeleteActuator() {
  int i = server.arg("index").toInt();
  if (i >= 0 && i < (int)actuators.size()) {
    deactivateActuator(actuators[i]); // Apagar antes de eliminar
    actuators.erase(actuators.begin() + i);
    saveActuators();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

/** Control manual de actuador desde la UI web */
void handleActuatorControl() {
  int i = server.arg("index").toInt();
  String cmd = server.arg("cmd");
  int dur = server.arg("dur").toInt(); // minutos

  if (i >= 0 && i < (int)actuators.size()) {
    if (cmd == "on")
      activateActuator(actuators[i], (unsigned long)dur * 60000UL);
    if (cmd == "off")
      deactivateActuator(actuators[i]);
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSaveSys() {
  strncpy(device_name, server.arg("name").c_str(), 39);
  strncpy(sentinel_token, server.arg("token").c_str(), 39);
  strncpy(mqtt_host, server.arg("mh").c_str(), 63);
  strncpy(mqtt_port_str, server.arg("mp").c_str(), 5);
  strncpy(mqtt_user, server.arg("mu").c_str(), 31);
  strncpy(mqtt_pass, server.arg("mpx").c_str(), 31);
  saveConfig();
  mqttClient.setServer(mqtt_host, atoi(mqtt_port_str));
  mqttClient.disconnect();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleAddSensor() {
  sensors.push_back({server.arg("id"), getGpio(server.arg("pin")),
                     server.arg("unit"),
                     (SensorType)server.arg("type").toInt()});
  saveSensors();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleDeleteSensor() {
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

// ============================================================
// SETUP & LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n🚀 Sentinel Node v3.0 booting...");
  loadConfig();

  WiFiManager wm;
  wm.setConnectTimeout(
      20); // 20s máximo para conectar con credenciales guardadas
  wm.setConnectRetries(3); // 3 intentos antes de rendirse
  wm.setConfigPortalTimeout(
      0); // Portal abierto indefinidamente (0 = sin timeout)

  // Si falla la conexión (contraseña incorrecta u otra razón):
  // borra las credenciales guardadas y reinicia → vuelve a abrir el portal de
  // configuración
  if (!wm.autoConnect("Sentinel_Node_AP")) {
    Serial.println("❌ WiFi falló (contraseña incorrecta o sin señal).");
    Serial.println("🔄 Borrando credenciales y reiniciando en modo portal...");
    delay(1000);
    wm.resetSettings(); // Limpia SSID/password guardados
    ESP.restart(); // Al reiniciar, no hay credenciales → abre portal de nuevo
  }

  Serial.print("📡 IP: ");
  Serial.println(WiFi.localIP());

  // mDNS: hostname único usando últimos 3 bytes de la MAC del chip
  // Garantiza que múltiples ESP8266 en la misma red no colisionen
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macSuffix[8];
  snprintf(macSuffix, sizeof(macSuffix), "%02x%02x%02x", mac[3], mac[4],
           mac[5]);

  String mdnsBase = String(device_name);
  mdnsBase.toLowerCase();
  mdnsBase.replace(" ", "-");
  mdnsBase.replace("_", "-");

  // Si el nombre es el default, usar solo el sufijo MAC para que sea corto
  String mdnsName;
  if (mdnsBase == "esp8266-sentinel-dyn") {
    mdnsName = "sentinel-" + String(macSuffix);
  } else {
    // Nombre personalizado + sufijo MAC para evitar colisiones
    mdnsName = mdnsBase + "-" + String(macSuffix);
  }

  if (MDNS.begin(mdnsName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("\U0001f310 mDNS: http://");
    Serial.print(mdnsName);
    Serial.println(".local");
  }

  // Inicializar actuadores en estado seguro antes de conectar MQTT
  initActuators();

  // Web routes
  server.on("/", handleRoot);
  server.on("/add_s", handleAddSensor);
  server.on("/delete_s", handleDeleteSensor);
  server.on("/add_a", handleAddActuator);
  server.on("/delete_a", handleDeleteActuator);
  server.on("/act", handleActuatorControl);
  server.on("/save_sys", handleSaveSys);
  server.on("/reset_wifi", handleResetWiFi);
  server.on("/reboot", []() {
    server.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });
  server.begin();

  mqttClient.setServer(mqtt_host, atoi(mqtt_port_str));
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024); // Aumentar buffer para payloads de actuador

  Serial.println("✅ Sentinel Node Ready.");
}

void loop() {
  server.handleClient();
  MDNS.update();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected())
      reconnectMQTT();
    mqttClient.loop(); // Procesar mensajes MQTT entrantes (comandos)

    unsigned long now = millis();

    // Publicar datos de sensores cada MEASUREMENT_INTERVAL
    if (now - lastMsg > MEASUREMENT_INTERVAL) {
      lastMsg = now;
      collectAndPublish();
    }

    // Publicar estado de actuadores cada STATUS_INTERVAL
    if (now - lastStatus > STATUS_INTERVAL) {
      lastStatus = now;
      publishStatus();
    }
  }

  // Safety watchdog: apagar actuadores con timer vencido
  // Se ejecuta siempre, incluso sin WiFi
  checkActuatorTimers();
}
