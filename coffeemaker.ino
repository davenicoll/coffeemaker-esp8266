#include <FS.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson
#include <PubSubClient.h> // https://github.com/knolleary/pubsubclient
#include <AWSWebSocketClient.h> // https://github.com/odelot/aws-mqtt-websockets
#include <HX711.h> // https://github.com/bogde/HX711 but delete yield override in hx711.cpp (https://github.com/bogde/HX711/issues/73)
#include <TimeLib.h> // https://github.com/PaulStoffregen/Time
#include <NtpClientLib.h> // https://github.com/gmag11/NtpClient

#define SETUP_PIN 0
#define SDA_PIN 4
#define SCL_PIN 5

#define HOSTNAME "coffeemaker"
#define AP_NAME "Coffee Maker"

#define LED_THRESHOLD_LOW 800
#define LED_THRESHOLD_HIGH 1000

#define LIGHT_STATE_ON "on"
#define LIGHT_STATE_OFF "off"
#define LIGHT_STATE_FLASHING "flash"

#define JUG_STATE_PRESENT "present"
#define JUG_STATE_REMOVED "removed"

#define COFFEE_STATE_OFF "off"
#define COFFEE_STATE_BREWING "brewing"
#define COFFEE_STATE_BREWED "brewed"
#define COFFEE_STATE_STALE "stale"
#define COFFEE_STATE_REHEATING "reheating"
#define COFFEE_STATE_PREPARING "preparing"

#define FLASH_PERIOD 3000
#define BREW_TIME 540000
#define JUG_CLEANING_TIME 60000
#define JUG_CLEANING_TIMEOUT 300000

#define SCALE_CALIBRATION -22.75
#define COFFEE_JUG_WEIGHT 330
#define COFFEE_PORTION_WEIGHT 200
#define COFFEE_FULL_WEIGHT 1200
#define WEIGHT_CHANGE_THRESHOLD 60
#define WEIGHT_SETTLING_TIME 2000
#define WEIGHT_EASING 0

#define MQTT_PORT 443
#define MQTT_ID "coffee_maker"

#define LOGGING true

ESP8266WebServer server(80);
HTTPClient http;

const int maxMQTTpackageSize = 512;
const int maxMQTTMessageHandlers = 1;
AWSWebSocketClient awsClient(1000, 10000);
PubSubClient client(awsClient);

bool configChanged = false;
char mqttServer[51];
char mqttTopic[66];
char awsKeyID[21];
char awsSecret[41];
char awsRegion[11];

int lightMeasurement;
bool lightIsOn = false;
uint32_t lastLightOnTime = 0;
uint32_t lastLightOffTime = 0;
String lightState = LIGHT_STATE_OFF;

HX711 scale(SDA_PIN, SCL_PIN);

String lastBrewTime;
String coffeeState = COFFEE_STATE_OFF;

int32_t weightMeasurement;
uint32_t lastWeightChangeTime;
uint32_t lastJugRemovedTime;
int32_t currentWeight;
int32_t referenceWeight;
int32_t coffeeWeight;
String jugState = JUG_STATE_PRESENT;


void debugLog (String name, String message) {
  if (!LOGGING) {
    return;
  }
  Serial.println(String("")+"["+name+"] "+message);
}

void handleLightHigh () {
  lastLightOnTime = millis();
  debugLog("LDR", "Bright");
}
void handleLightLow () {
  lastLightOffTime = millis();
  debugLog("LDR", "Dark");
}

void handleWeightChange (int32_t newWeight) {
  int32_t delta = newWeight - currentWeight;
  if (jugState == JUG_STATE_REMOVED && delta > COFFEE_JUG_WEIGHT * 0.75)
  {
    debugLog("COFFEE", "Jug replaced");
    handleJugStateChange(JUG_STATE_PRESENT);
    int32_t newCoffeeWeight = newWeight - referenceWeight;
    if (abs(newCoffeeWeight - coffeeWeight) > WEIGHT_CHANGE_THRESHOLD) {
      handleCoffeeWeightChange(newCoffeeWeight);
    }
  }
  else if (jugState == JUG_STATE_PRESENT && delta < -COFFEE_JUG_WEIGHT * 0.75)
  {
    debugLog("COFFEE", "Jug removed");
    handleJugStateChange(JUG_STATE_REMOVED);
    handleReferenceWeightChange(newWeight + COFFEE_JUG_WEIGHT);
    int32_t newCoffeeWeight = currentWeight - referenceWeight;
    if (abs(newCoffeeWeight - coffeeWeight) > WEIGHT_CHANGE_THRESHOLD) {
      handleCoffeeWeightChange(newCoffeeWeight);
    }
  }
  debugLog("COFFEE", "Delta weight" + String(delta));
  currentWeight = newWeight;
  mqttSend("rawWeight", String(currentWeight));
}
void handleReferenceWeightChange (int32_t newWeight) {
  referenceWeight = newWeight;
  mqttSend("tareWeight", String(newWeight));
}
void handleCoffeeWeightChange (int32_t newWeight) {
  coffeeWeight = newWeight;
  mqttSend("coffeeWeight", String(newWeight));
}

void handleLEDChange (String newState) {
  lightState = newState;
  mqttSendString("light", lightState);
  mqttSend("heaterOn", lightState == LIGHT_STATE_OFF ? "false" : "true");
  if (newState == LIGHT_STATE_ON) {
    mqttSend("lightOnValue", String(lightMeasurement));
  } else if (newState == LIGHT_STATE_OFF) {
    mqttSend("lightOffValue", String(lightMeasurement));
  }
}
void handleJugStateChange (String newState) {
  jugState = newState;
  if (newState == JUG_STATE_REMOVED) {
    lastJugRemovedTime = millis();
  }
  mqttSend("jugPresent", newState == JUG_STATE_PRESENT ? "true" : "false");
}
void handleCoffeeStateChange (String newState) {
  coffeeState = newState;
  mqttSendString("coffee", coffeeState);
  if (newState == COFFEE_STATE_BREWING) {
    mqttSendString("lastBrewTime", getDateTimeString());
    handleReferenceWeightChange(currentWeight - COFFEE_FULL_WEIGHT);
    handleCoffeeWeightChange(COFFEE_FULL_WEIGHT);
  }
}

bool ledHasBeenOnFor (uint32_t t) {
  return (lightIsOn && millis() - lastLightOnTime > t);
}
bool ledHasBeenOffFor (uint32_t t) {
  return (!lightIsOn && millis() - lastLightOffTime > t);
}
bool ledHasFlashed (uint32_t t) {
  return (lightIsOn && lastLightOffTime > 0 && millis() - lastLightOffTime < t) || (!lightIsOn && lastLightOnTime > 0 && millis() - lastLightOnTime < t);
}

bool weightHasBeenSettledFor (uint32_t t) {
  return millis() - lastWeightChangeTime > t;
}
bool weightHasChangedBy (uint32_t w) {
  return abs(currentWeight - weightMeasurement) > w;
}
bool jugHasBeenGoneFor (uint32_t t) {
  return jugState != JUG_STATE_PRESENT && millis() - lastJugRemovedTime > t;
}

void handleTick () {
  // LED status checking
  if (ledHasBeenOffFor(2*FLASH_PERIOD) && lightState != LIGHT_STATE_OFF) {
    handleLEDChange(LIGHT_STATE_OFF);
  } else if (ledHasBeenOnFor(2*FLASH_PERIOD) && lightState != LIGHT_STATE_ON) {
    handleLEDChange(LIGHT_STATE_ON);
  } else if (ledHasFlashed(FLASH_PERIOD) && lightState != LIGHT_STATE_FLASHING) {
    handleLEDChange(LIGHT_STATE_FLASHING);
  }

  // Weight status checking
  if (weightHasChangedBy(WEIGHT_CHANGE_THRESHOLD) && weightHasBeenSettledFor(WEIGHT_SETTLING_TIME)) {
    handleWeightChange(weightMeasurement);
  }

  // Update state machine
  if (coffeeState == COFFEE_STATE_OFF) {
    if (lightState == LIGHT_STATE_ON) {
      handleCoffeeStateChange(COFFEE_STATE_BREWING);
    } else if (jugHasBeenGoneFor(JUG_CLEANING_TIME) && !jugHasBeenGoneFor(JUG_CLEANING_TIMEOUT)) {
      handleCoffeeStateChange(COFFEE_STATE_PREPARING);
    }
  } else if (coffeeState == COFFEE_STATE_BREWING) {
    if (lightState == LIGHT_STATE_OFF) {
      handleCoffeeStateChange(COFFEE_STATE_OFF);
    } else if (ledHasBeenOnFor(BREW_TIME)) {
      handleCoffeeStateChange(COFFEE_STATE_BREWED);
    }
  } else if (coffeeState == COFFEE_STATE_BREWED) {
    if (lightState == LIGHT_STATE_OFF) {
      handleCoffeeStateChange(COFFEE_STATE_STALE);
    }
  } else if (coffeeState == COFFEE_STATE_STALE) {
    if (lightState == LIGHT_STATE_ON) {
      handleCoffeeStateChange(COFFEE_STATE_REHEATING);
    } else if (jugHasBeenGoneFor(JUG_CLEANING_TIME)) {
      handleCoffeeStateChange(COFFEE_STATE_PREPARING);
    }
  } else if (coffeeState == COFFEE_STATE_REHEATING) {
    if (lightState == LIGHT_STATE_OFF) {
      handleCoffeeStateChange(COFFEE_STATE_STALE);
    }
  } else if (coffeeState == COFFEE_STATE_PREPARING) {
    if (jugState == JUG_STATE_PRESENT) {
      handleCoffeeStateChange(COFFEE_STATE_OFF);
    } else if (jugHasBeenGoneFor(JUG_CLEANING_TIMEOUT)) {
      handleCoffeeStateChange(COFFEE_STATE_OFF);
    }
  }
}

void initPins () {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(SETUP_PIN, INPUT_PULLUP);
}

void initScale () {
  scale.set_scale(SCALE_CALIBRATION);
}

void saveConfigCallback () {
  debugLog("CONFIG", "Config set by user");
  configChanged = true;
}

void mqttCallback (char* topic, byte* payload, unsigned int len) {
  digitalWrite(LED_BUILTIN, LOW);
  debugLog("MQTT", "receiving message on topic" + String(topic) + ":");
  for (int i = 0; i < len; i++) { Serial.print((char)payload[i]); }
  Serial.println();
  digitalWrite(LED_BUILTIN, HIGH);
}

void mqttSend (String field, String value) {
  digitalWrite(LED_BUILTIN, LOW);
  String payload = "{\"state\":{\"reported\":{\"" + field + "\":" + value + "}}}";
  char buf[100];
  payload.toCharArray(buf, 100);
  int rc = client.publish(mqttTopic, buf);
  digitalWrite(LED_BUILTIN, HIGH);
}

void mqttSendString (String field, String value) {
  return mqttSend(field, "\"" + value + "\"");
}

bool initConfig () {
  debugLog("CONFIG", "Loading config file...");
  if (!SPIFFS.begin()) {
    debugLog("CONFIG", "Failed to mount file system");
    return false;
  }
  if (SPIFFS.exists("/config.json.fault")) {
    debugLog("CONFIG", "Config fault file found. Deleting config");
    if (!SPIFFS.remove("/config.json")) {
      debugLog("CONFIG", "Failed to delete config file");
    }
    if (!SPIFFS.remove("/config.json.fault")) {
      debugLog("CONFIG", "Failed to delete config fault file");
    }
    return false;
  }
  if (!SPIFFS.exists("/config.json")) {
    debugLog("CONFIG", "Config file not found");
    return false;
  }
  File configFault = SPIFFS.open("/config.json.fault", "w");
  configFault.print("1");
  configFault.close();
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    debugLog("CONFIG", "Failed to open config file");
    return false;
  }
  size_t size = configFile.size();
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  debugLog("CONFIG", "Config file loaded. Parsing...");
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());
  if (!json.success()) {
    debugLog("CONFIG", "Failed to parse config JSON");
    return false;
  }
  debugLog("CONFIG", "Config file parsed:");
  json.printTo(Serial);
  bool success = true;
  strcpy(mqttServer, json["mqttServer"]);
  success = success && strlen(mqttServer) != 0;
  strcpy(mqttTopic, json["mqttTopic"]);
  success = success && strlen(mqttTopic) != 0;
  strcpy(awsKeyID, json["awsKeyID"]);
  success = success && strlen(awsKeyID) != 0;
  strcpy(awsSecret, json["awsSecret"]);
  success = success && strlen(awsSecret) != 0;
  strcpy(awsRegion, json["awsRegion"]);
  success = success && strlen(awsRegion) != 0;
  configFile.close();
  if (!SPIFFS.remove("/config.json.fault")) {
    debugLog("CONFIG", "Failed to delete config fault file");
  }
  debugLog("CONFIG", "Done");
  return success;
}

bool saveConfig () {
  debugLog("CONFIG", "Generating JSON");
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["mqttServer"] = mqttServer;
  json["mqttTopic"] = mqttTopic;
  json["awsKeyID"] = awsKeyID;
  json["awsSecret"] = awsSecret;
  json["awsRegion"] = awsRegion;
  debugLog("CONFIG", "Saving config file:");
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    debugLog("CONFIG", "Failed to open config file for writing");
  }
  json.printTo(Serial);
  json.printTo(configFile);
  configFile.close();
  debugLog("CONFIG", "Done");
}

void initWifi (bool forcePortal) {
  debugLog("WIFI", "Attempting to start WIFI");
  WiFiManagerParameter custMQTT("MQTT Server", "MQTT Server", mqttServer, 51);
  WiFiManagerParameter custTopic("MQTT Topic", "MQTT Topic", mqttTopic, 66);
  WiFiManagerParameter custAWSID("AWS Access Key ID", "AWS Access Key ID", awsKeyID, 21);
  WiFiManagerParameter custAWSSec("AWS Secret Access Key", "AWS Secret Access Key", awsSecret, 41);
  WiFiManagerParameter custAWSReg("AWS Region", "AWS Region", awsRegion, 11);
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setConfigPortalTimeout(300);
  wifiManager.addParameter(&custMQTT);
  wifiManager.addParameter(&custTopic);
  wifiManager.addParameter(&custAWSID);
  wifiManager.addParameter(&custAWSSec);
  wifiManager.addParameter(&custAWSReg);
  if (forcePortal) {
    debugLog("WIFI", "Starting config portal");
    wifiManager.startConfigPortal(AP_NAME);
  } else {
    debugLog("WIFI", "Auto-connecting");
    wifiManager.autoConnect(AP_NAME);
  }
  if (configChanged) {
    debugLog("WIFI", "Updating config");
    strcpy(mqttServer, custMQTT.getValue());
    strcpy(mqttTopic, custTopic.getValue());
    strcpy(awsKeyID, custAWSID.getValue());
    strcpy(awsSecret, custAWSSec.getValue());
    strcpy(awsRegion, custAWSReg.getValue());
    saveConfig();
  }
  for (int i=0; i<150; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      break;
    }
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    debugLog("WIFI", "Connected! IP address: " + WiFi.localIP());
  } else {
    debugLog("WIFI", "Failed to connect after 15 seconds");
    WiFi.disconnect();
    return initWifi(true);
  }
}

void handleServer () {
  String res;
  res += "I am a coffee maker.\n";
  res += "Light measurement is " + String(lightMeasurement) + "\n";
  res += "My light is " + String(lightIsOn ? "on" : "off") + ".\n";
  res += "Jug is " + jugState + ".\n";
  res += (String)"There are " + String(coffeeWeight) + "g of coffee remaining.\n";
  server.send(200, "text/plain", res);
}

void initServer () {
  server.onNotFound(handleServer);
  server.begin();
  debugLog("Server", "Server started");
}

bool initMDNS () {
  if (!MDNS.begin(HOSTNAME)) {
    return false;
  }
  MDNS.addService("http", "tcp", 80);
  debugLog("MDNS", "MDNS started");
  return true;
}

void initAWS () {
  debugLog("AWS", "Intializing AWS client:");
  debugLog("AWS", String(awsKeyID));
  debugLog("AWS", String(awsRegion));
  debugLog("AWS", String("") + awsSecret[0] + "..." + awsSecret[strlen(awsSecret) - 1]);
  awsClient.setAWSRegion(awsRegion);
  awsClient.setAWSDomain(mqttServer);
  awsClient.setAWSKeyID(awsKeyID);
  awsClient.setAWSSecretKey(awsSecret);
  awsClient.setUseSSL(true);
}

String getDateTimeString () {
  time_t t = now();
  char str[21];
  sprintf(str, "%4d-%02d-%02dT%02d:%02d:%02dZ", year(t), month(t), day(t), hour(t), minute(t), second(t));
  return str;
}

void initNTP () {
  NTP.begin("pool.ntp.org", 0, false, 0);
  NTP.setInterval(60);
}

bool initMQTT () {
  debugLog("MQTT", "Connecting to MQTT endpoint:");
  debugLog("MQTT", String(mqttServer));
  debugLog("MQTT", String(mqttTopic));
  if (client.connected()) {    
    client.disconnect ();
  }  
  client.setServer(mqttServer, MQTT_PORT);
  if (!client.connect(MQTT_ID)) {
    debugLog("MQTT", "Failed to make connection. ");
    return false;
  }
  debugLog("MQTT", "Connection established");
  client.setCallback(mqttCallback);
  client.subscribe(mqttTopic);
  return true;
}

void setup() {
  Serial.begin(115200);

  initPins();
  initScale();
  bool configLoaded = initConfig();
  delay(3000);
  bool forceConfig = (digitalRead(SETUP_PIN) == LOW);
  initWifi(forceConfig || !configLoaded);
  initServer();
  initMDNS();
  initAWS();
  initNTP();
  initMQTT();

  mqttSendString("lastBootTime", getDateTimeString());
  mqttSendString("localIP", WiFi.localIP().toString());
}

void loop() {
  lightMeasurement = analogRead(A0);
  if (lightIsOn && lightMeasurement < LED_THRESHOLD_LOW) {
    lightIsOn = false;
    handleLightLow();
  } else if (!lightIsOn && lightMeasurement > LED_THRESHOLD_HIGH) {
    lightIsOn = true;
    handleLightHigh();
  }

  int w = scale.get_units(1);
  if (abs(w - weightMeasurement) > WEIGHT_CHANGE_THRESHOLD) {
    lastWeightChangeTime = millis();
  }
  weightMeasurement = (weightMeasurement*WEIGHT_EASING + w) / (WEIGHT_EASING + 1);

  if (WiFi.status() != WL_CONNECTED){
    initWifi(false);
  }

  server.handleClient();
  if (awsClient.connected()) {    
      client.loop();
  } else {
    initMQTT();
  }
  handleTick();
  delay(20);
}
