#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <Adafruit_GFX.h>
#include "Adafruit_SH1106.h"
#include <WebServer.h>
#include "secrets.h"

// -------- Function declarations --------
void OnConnect();
void NotFound();
void updateDisplay();
void readSensor();
void publishMQTT();
String SendHTML();

// -------- Webserver --------
WebServer server(80);

// -------- OLED --------
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SH1106 display(OLED_SDA, OLED_SCL);

// -------- BME280 --------
Adafruit_BME280 bme;

float temperature, humidity, pressure, altitude;
double SEALEVELPRESSURE_HPA;

#define corr_altitude (5.75)

// -------- MQTT Topics --------
#define MQTT_PUB_TEMP "esp32/bme280/temperature"
#define MQTT_PUB_HUM  "esp32/bme280/humidity"
#define MQTT_PUB_PRES "esp32/bme280/pressure"
#define MQTT_PUB_ALT  "esp32/bme280/altitude"

// -------- MQTT Client --------
AsyncMqttClient mqttClient;

TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;

// -------- Timer --------
unsigned long previousMillis = 0;
const long interval = 60000; // 60000 = 1 Minute
bool firstPublishDone = false;


// ---------------- WIFI ----------------

void connectToWifi() {

  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void connectToMqtt() {

  Serial.println("Connecting to MQTT...");
  mqttClient.connect();
}

void WiFiEvent(WiFiEvent_t event) {

  switch(event) {

    case SYSTEM_EVENT_STA_GOT_IP:

      Serial.println("WiFi connected");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());

      connectToMqtt();
      break;

    case SYSTEM_EVENT_STA_DISCONNECTED:

      Serial.println("WiFi lost connection");

      xTimerStop(mqttReconnectTimer,0);
      xTimerStart(wifiReconnectTimer,0);

      break;
  }
}


// ---------------- MQTT ----------------

void onMqttConnect(bool sessionPresent) {

  Serial.println("Connected to MQTT");
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {

  Serial.println("MQTT disconnected");

  if(WiFi.isConnected())
    xTimerStart(mqttReconnectTimer,0);
}

void onMqttPublish(uint16_t packetId) {

  Serial.print("Publish acknowledged: ");
  Serial.println(packetId);
}


// ---------------- SENSOR ----------------

void readSensor() {

  temperature = bme.readTemperature();
  humidity = bme.readHumidity();
  pressure = bme.readPressure()/100.0F;

  SEALEVELPRESSURE_HPA = pressure + corr_altitude;

  altitude = bme.readAltitude(SEALEVELPRESSURE_HPA);
}


// ---------------- MQTT PUBLISH ----------------

void publishMQTT() {

  if(!mqttClient.connected())
    return;

  mqttClient.publish(MQTT_PUB_TEMP,1,true,String(temperature).c_str());
  mqttClient.publish(MQTT_PUB_HUM,1,true,String(humidity).c_str());
  mqttClient.publish(MQTT_PUB_PRES,1,true,String(pressure).c_str());
  mqttClient.publish(MQTT_PUB_ALT,1,true,String(altitude).c_str());
}

// ---------------- OLED DISPLAY ----------------

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);

  // Temperatur
  display.setCursor(0, 0);
  display.print("TEMP: ");
  display.print(temperature); display.println(" C");

  // Luftfeuchtigkeit
  display.setCursor(0, 11);
  display.print("HUMI: ");
  display.print(humidity);
  display.println(" %");

  // Druck
  display.setCursor(0, 22);
  display.print("PRES: ");
  display.print(pressure);
  display.println(" hPa");

  // Höhe
  display.setCursor(0, 33);
  display.print("ALTI: ");
  display.print(altitude);
  display.println(" m");

  // IP-Adresse
  display.setCursor(0, 45);
  display.print("WiFi: "); display.println(WiFi.localIP());

  // mqtt-Broker-IP auflösen und anzeigen
  display.setCursor(0, 56);
  display.print("MQTT: "); display.println(MQTT_HOST);

  display.display();
}


// ---------------- SETUP ----------------

void setup() {

  Serial.begin(115200);
  Serial.println();

  // Sensor
  if(!bme.begin(0x76)) {

    Serial.println("BME280 not found!");
    while(1);
  }

  // OLED
  display.begin(SH1106_I2C_ADDRESS,0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // MQTT / WiFi Timer
  mqttReconnectTimer = xTimerCreate("mqttTimer",
                                    pdMS_TO_TICKS(2000),
                                    pdFALSE,
                                    (void*)0,
                                    reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));

  wifiReconnectTimer = xTimerCreate("wifiTimer",
                                    pdMS_TO_TICKS(2000),
                                    pdFALSE,
                                    (void*)0,
                                    reinterpret_cast<TimerCallbackFunction_t>(connectToWifi));

  WiFi.onEvent(WiFiEvent);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onPublish(onMqttPublish);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);

  connectToWifi();

  // Webserver
  server.on("/",OnConnect);
  server.onNotFound(NotFound);
  server.begin();

  Serial.println("Setup finished");
}


// ---------------- LOOP ----------------

void loop() {

  server.handleClient();

  unsigned long currentMillis = millis();

  // Prüfen, ob WLAN verbunden ist
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastTry = 0;
    if (currentMillis - lastTry >= 3000) {  // alle 3 Sekunden prüfen
      lastTry = currentMillis;
      Serial.println("Waiting for WiFi...");
    }
    return; // erst weitermachen, wenn WLAN verbunden
  }

  // Prüfen, ob MQTT verbunden ist
  if (!mqttClient.connected()) {
    static unsigned long lastTryMQTT = 0;
    if (currentMillis - lastTryMQTT >= 3000) { // alle 3 Sekunden prüfen
      lastTryMQTT = currentMillis;
      Serial.println("Waiting for MQTT...");
      mqttClient.connect(); // ggf. erneut verbinden
    }
    return; // erst weitermachen, wenn MQTT verbunden
  }

  // Erstes Update nach Verbindung
  static bool firstRunDone = false;
  if (!firstRunDone) {
    readSensor();
    publishMQTT();
    updateDisplay();
    previousMillis = currentMillis;
    firstRunDone = true;
    return;
  }

  // Folgende Messungen nach Interval
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    readSensor();
    publishMQTT();
    updateDisplay();
  }
}


// ---------------- WEB ----------------

void OnConnect() {

  server.send(200,"text/html",SendHTML());
}

void NotFound() {

  server.send(404,"text/plain","Not found");
}


// ---------------- HTML PAGE ----------------

String SendHTML() {

  String str;

  str += "<!DOCTYPE html><html>";
  str += "<head>";
  str += "<meta name='viewport' content='width=device-width'>";
  str += "<meta http-equiv='refresh' content='30'>";
  str += "<title>ESP32 Meteodaten</title>";
  str += "</head>";

  str += "<body style='font-family:Arial;text-align:center;'>";

  str += "<h2>BME280 Messwerte</h2>";

  str += "<p>-----------------------------------------</p>";
  str += "<p>Temperatur: " + String(temperature) + " C</p>";
  str += "<p>Feuchte: " + String(humidity) + " %</p>";
  str += "<p>Druck: " + String(pressure) + " hPa</p>";
  str += "<p>Hoehe: " + String(altitude) + " m</p>";
  str += "<p>-----------------------------------------</p>";
  str += "<p>IP (esp32dev): " + WiFi.localIP().toString() + "</p>";
  str += "<p>MQTT-Broker: " + MQTT_HOST.toString() + "</p>";

  str += "</body></html>";

  return str;
}