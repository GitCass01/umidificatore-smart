/*
 * Autore:   Davide Carniselli
 * Github:   https://github.com/GitCass01
 * Progetto: Umidificatore Smart
 * Licenza:  GNU GENERAL PUBLIC LICENSE version 3 (GPLv3)
*/

#include "Credentials.h"
#include "DHT.h"
#include "AiEsp32RotaryEncoder.h"
#include "TaskScheduler.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <PubSubClientTools.h>

// WIFI + mqtt setup (vedi Credentials.h per credenziali)
const String ssid = WIFI_SSID;
const String pass = WIFI_PASSWORD;
WiFiClient espClient;
PubSubClient client(MQTT_SERVER, 1883, espClient);
PubSubClientTools mqtt(client);

// topic (publish) mqtt
String ID = "UmidificatoreSmartCass";
const char *DHT11_TEMP = "UmidificatoreSmartCass/dht11/temperature";
const char *DHT11_HUM = "UmidificatoreSmartCass/dht11/humidity";
const char *DHT11_HIC = "UmidificatoreSmartCass/dht11/hic";
const char *WATER_LEVEL = "UmidificatoreSmartCass/waterLevel";
const char *ATOMIZER = "UmidificatoreSmartCass/atomizer";
const char *MODALITA = "UmidificatoreSmartCass/modalita";
const char *TRESHOLD = "UmidificatoreSmartCass/treshold";
const char *INTERMIT_TIME = "UmidificatoreSmartCass/intermitTime";

#define DHTTYPE DHT11
#define DHTPIN 14
#define RED 32                          // led informativo per capire quando sta/dovrebbe atomizzare l'acqua
#define waterSensorPower 27             // pin per dare corrente al water sensor solo quando necessario, per evitare corrosione rapida del sensore
#define waterSensorOutput 34
#define WATER_NIL 0                     // valore del water sensor non in acqua
#define WATER_MIN 205                   // valore del water sensor con acqua minima
#define WATER_MID 260                   // valore del water sensor con acqua a metà
#define WATER_MAX 310                   // valore del water sensor completamente immerso
#define NEED_WATER 35
#define ATOMIZER_EN 4                   // pin per abilitare l'atomizzatore
#define ROTARY_ENCODER_A_PIN 33         // clk pin
#define ROTARY_ENCODER_B_PIN 25         // dt pin
#define ROTARY_ENCODER_BUTTON_PIN 26    // button pin
#define ROTARY_ENCODER_VCC_PIN -1
#define ROTARY_ENCODER_STEPS 4
#define SCREEN_WIDTH 128                // OLED display width, in pixels
#define SCREEN_HEIGHT 64                // OLED display height, in pixels

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // oled ssd1306 128x64
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, ROTARY_ENCODER_VCC_PIN, ROTARY_ENCODER_STEPS);

DHT dht(DHTPIN, DHTTYPE);
Scheduler ts;

// Variabili globali
int Mode = 0;                                 // modalità dell'umidificatore
float temp = 0;                               // temperatura corrente
float hum = 0;                                // umidità corrente
float hic = 0;                                // heat index corrente
float hum_treshold = 70;                      // threshold per atomizzatore
unsigned long previousMillis = 0;
unsigned long millisIntermittenza = 30000;    // delay di atomizzazione
unsigned long seconds;
unsigned long minutes;
int waterLevelValue;                          // valore ottenuto dal water level sensor compreso tra [WATER_MIN,WATER_MAX]
int waterLevelY;                              // valore tra 0 e 127, ottenuto normalizzando waterLevelValue
int atomizerState = LOW;                      // stato dell'atomizzatore corrente

/*
 * gestione del click button del rotary encoder
*/
void rotary_onButtonClick() {
    static unsigned long lastTimePressed = 0;
    if (millis() - lastTimePressed < 500) {
            return;
    }
    lastTimePressed = millis();
    
    // gestione cambio modalità umdificatore
    Mode = (Mode+1) % 3;
    if (Mode == 1) {
      Serial.println(F("Modalità Automatica."));
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(2);
      display.println(F("Modalita'"));
      display.println(F("Automatica"));
      display.setTextSize(1);
      display.display();
      mqtt.publish(MODALITA, "Automatico");
    } else if (Mode == 2) {
      mqtt.publish(MODALITA, "Intermittenza");
      Serial.println(F("Modalita' Intermittenza."));
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(2);
      display.println(F("Modalita'"));
      display.println(F("Intermittenza"));
      display.setTextSize(1);
      display.display();
    } else if (Mode == 0) {
      Serial.println(F("Umidificatore spento."));
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(2);
      display.println(F("Umidificatore"));
      display.println(F("spento"));
      display.setTextSize(1);
      display.display();
      mqtt.publish(MODALITA, "Spento");
    }

    atomizza(LOW);
}

/*
 * funzione di gestione del rotary encorder
*/
void rotary_loop() {
  if (rotaryEncoder.isEncoderButtonClicked()) {
    rotary_onButtonClick();
  }

  int encoderDelta;

  /* 
   *  se la modalità corrente è automatica, allora la rotazione modifica il threshold
   *  altrimenti, se la modalità è ad intermittenza, modifica il delay dell'atomizzazione
  */
  if (Mode == 1) {
    encoderDelta = rotaryEncoder.encoderChanged();
    if (encoderDelta == 0) {
      return;
    }
  
    if (encoderDelta > 0 && hum_treshold < 100) {
      hum_treshold = hum_treshold + 5;
    } else if (encoderDelta < 0 && hum_treshold >= 5) {
      hum_treshold = hum_treshold - 5;
    }

    Serial.print(F("New Humidity treshold: "));
    Serial.print(hum_treshold);
    Serial.println(F("%"));
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("New hum treshold: "));
    display.println();
    display.setTextSize(2);
    display.print(hum_treshold);
    display.println(F("%"));
    display.setTextSize(1);
    display.display();
    mqtt.publish(TRESHOLD, String(hum_treshold).c_str());
  } else if (Mode == 2) {
    encoderDelta = rotaryEncoder.encoderChanged();
    if (encoderDelta == 0) {
      return;
    }

    // girare il rotary significa aumentare/diminuire l'intermittenza di 15s
    if (encoderDelta > 0) {
      millisIntermittenza = millisIntermittenza + 15000;
    } else {
      if (millisIntermittenza >= 15000)
        millisIntermittenza = millisIntermittenza - 15000;
    }

    // formattazione in mm:ss del nuovo tempo di intermittenza da stampare a video
    seconds = millisIntermittenza / 1000;
    minutes = (millisIntermittenza / 1000) / 60;
    seconds %= 60;
    minutes %= 60;
    Serial.print(F("New intermit time (mm:ss): "));
    Serial.print(minutes);
    Serial.print(F(":"));
    Serial.println(seconds);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("New intermit time:"));
    display.println();
    display.print(F("(mm:ss) "));
    display.setTextSize(2);
    display.print(minutes);
    display.print(F(":"));
    display.println(seconds);
    display.setTextSize(1);
    display.display(); 

    String timeStr = "";
    timeStr += minutes;
    timeStr += ":";
    timeStr += seconds;
    mqtt.publish(INTERMIT_TIME, timeStr);

    // ogni volta che cambio il delay spengo l'atomizzazione
    atomizza(LOW);
  }
}

void IRAM_ATTR readEncoderISR() {
    rotaryEncoder.readEncoder_ISR();
}

// TASKS
Task checkDHT11Sensors(4 * TASK_SECOND, TASK_FOREVER, &dht11Sensors);
Task checkWaterLevelSensor(3 * TASK_SECOND, TASK_FOREVER, &activateWaterSensorPower);
Task printSensorsValues(5 * TASK_SECOND, TASK_FOREVER, &printSensors);
Task printDisplaySensors(5 * TASK_SECOND, TASK_FOREVER, &displaySensors);
Task atomizer(TASK_IMMEDIATE, TASK_FOREVER, &atomize);

/*
 * gestione del sensore DHT11 (temperatura ed umidità)
*/
void dht11Sensors() {
  if (Mode == 0)
    return;

  //Serial.println("Checking DHT11 sensor...");
  
  hum = dht.readHumidity();
  temp = dht.readTemperature();

  // controllo errori del dht, se ci sono esco per ripetere la lettura del sensore
  if (isnan(hum) || isnan(temp)) {
    Serial.println(F("Failed to read from DHT sensor!"));
    return;
  }

  // Calcolo l'indice di calore (ovvero l'afa, causata da alte temperature e alta umidità)
  hic = dht.computeHeatIndex(temp, hum, false);
}

/*
 * attivazione del water level sensor
*/
void activateWaterSensorPower() {
  if (Mode == 0)
    return;
  
  digitalWrite(waterSensorPower, HIGH);
  checkWaterLevelSensor.setCallback(&waterLevel);
  checkWaterLevelSensor.delay(20);
}

/*
 * lettura del water level sensor e normalizzazione del valore per stamparlo sull'oled
*/
void waterLevel() {
  waterLevelValue = analogRead(waterSensorOutput);
  digitalWrite(waterSensorPower, LOW);

  // calcoli ottenuti dopo una calibrazione iniziale (con acqua di rubinetto di casa)
  // in base alla conduttività dell'acqua bisogna ricalibrare il sensore
  if (waterLevelValue <= WATER_MID) {
    waterLevelValue *= 0.6;
  } else if (waterLevelValue <= WATER_MIN) {
    waterLevelValue *= 0.2;
  }

  /*Serial.print("The water sensor value: ");
  Serial.println(waterLevelValue);*/

  waterLevelY = map(waterLevelValue, WATER_NIL, WATER_MAX, 0, 128);
  /*Serial.print("Water level: ");
  Serial.println(waterLevelY);*/

  checkWaterLevelSensor.setCallback(&activateWaterSensorPower);
}

/*
 * visualizzazione dei valori dei sensori sull'oled
*/
void displaySensors() {
  if (Mode == 0)
    return;

  if (waterLevelY <= NEED_WATER) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(2);
    display.println("Aggiungere");
    display.println("Acqua");
    display.setTextSize(1);
    display.display();
    return;
  }
  
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print(F("Humidity:      "));
  display.print(hum);
  display.println(F("%"));
  display.println();
  display.print(F("Temperature:   "));
  display.print(temp);
  display.println(F("C"));
  display.println();
  display.print(F("Heat Index:    "));
  display.print(hic);
  display.println(F("C"));
  display.println();

  if (Mode == 1) {
    display.print(F("Hum treshold: "));
    display.print(hum_treshold);
    display.println(F("%"));
    display.println();
  } else {
    display.print(F("Intermit time: "));
    display.print(minutes);
    display.print(":");
    display.println(seconds);
  }
  
  //display.print(F("Water level:   "));
  display.drawLine(0, 61, waterLevelY, 61, WHITE);
  display.display(); 
}

/*
 * visualizzazione dei sensori su Serial Monitor e su MQTT
*/
void printSensors() {
  if (Mode == 0)
    return;

  if (waterLevelY <= NEED_WATER) {
    Serial.println(F("Aggiungere acqua."));
  }

  // DHT11
  Serial.print(F("Humidity: "));
  Serial.print(hum);
  Serial.print(F("%  Temperature: "));
  Serial.print(temp);
  Serial.print(F("°C "));
  Serial.print(F("  Heat Index: "));
  Serial.print(hic);
  Serial.println(F("°C"));
  Serial.print(F("Humidity treshold: "));
  Serial.print(hum_treshold);
  Serial.println(F("%"));

  // water level sensor
  Serial.print(F("Water level: "));
  Serial.println(waterLevelY);

  // invio mqtt
  mqtt.publish(DHT11_TEMP, String(temp).c_str());
  mqtt.publish(DHT11_HUM, String(hum).c_str());
  mqtt.publish(DHT11_HIC, String(hic).c_str());
  mqtt.publish(WATER_LEVEL, String(waterLevelY).c_str());
}

/*
 * gestione atomizzatore in modalità intermittenza
*/
void intermit() {  
  unsigned long currentMillis = millis();
  
  if (previousMillis == 0)
    previousMillis = currentMillis;
  
  if (currentMillis - previousMillis >= millisIntermittenza) {
    atomizza(!atomizerState);  
    previousMillis = currentMillis;
  }
}

/*
 * attivazione/disattivazione atomizzatore
*/
void atomizza (int state) {
  if (state == HIGH && atomizerState == LOW) {
      atomizerState = HIGH;
      digitalWrite(RED, atomizerState);
      digitalWrite(ATOMIZER_EN, atomizerState);
      mqtt.publish(ATOMIZER, "Atomizzatore attivo");
  } else if (state == LOW && atomizerState == HIGH) {
      atomizerState = LOW;
      digitalWrite(RED, atomizerState);
      digitalWrite(ATOMIZER_EN, atomizerState);
      mqtt.publish(ATOMIZER, "Atomizzatore spento");
  }
}

/*
 * gestione atomizzatore in base alla modalità e al livello del'acqua
*/
void atomize() {
  if (Mode == 0)
    return;
  
  if (waterLevelY <= NEED_WATER) {
    // Serial.println("Livello acqua troppo basso (waterLevelY) per atomizzare.");
    atomizza(LOW);
    return;
  }
  
  if (Mode == 1) {    
    if (hum < hum_treshold) {
      atomizza(HIGH);
    } else {
      atomizza(LOW);
    }
  } else if (Mode == 2) {
    intermit();
  }
}

/*
 * connessione al WiFi
*/
void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.print(F("Connecting..."));
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.println(F("Connecting "));
  display.println(F("to wifi..."));
  display.display();
  
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(F("."));
  }
  Serial.println();

  Serial.print(F("Connected, IP address: "));
  Serial.println(WiFi.localIP());
}

/*
 * connessione al server MQTT
*/
void setupMQTT() {
  // Connect to MQTT
  Serial.print("Connecting to MQTT: "+String(MQTT_SERVER)+" ... ");
  if (client.connect(ID.c_str())) {
      Serial.println(F("connected"));
      mqtt.publish(MODALITA, "Spento");
  } else {
      Serial.println("Failed, rc="+client.state());
  }
}

/*
 * setup wifi, mqtt, sensori, oled, atomizzatore e scheduler
*/
void setup() {
  Serial.begin(115200);

  // Oled setup
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
  }

  // setup wifi + mqtt
  setupWifi();
  setupMQTT();

  // Rotary encoder setup
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.disableAcceleration(); //acceleration is now enabled by default - disable if you dont need it

  // dht11 setup
  dht.begin();

  // water level sensor setup
  pinMode(waterSensorPower,OUTPUT);
  digitalWrite(waterSensorPower,LOW);

  // atomizer setup
  pinMode(RED,OUTPUT);
  pinMode(ATOMIZER_EN,OUTPUT);
  atomizza(atomizerState);
  Serial.println(F("Umidificatore spento."));
  
  // Oled display umidificatore spento
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(F("Umidificatore"));
  display.println(F("spento"));
  display.setTextSize(1);
  display.display();

  // adding/enabling tasks
  ts.addTask(checkDHT11Sensors);
  ts.addTask(checkWaterLevelSensor);
  ts.addTask(printSensorsValues);
  ts.addTask(printDisplaySensors);
  ts.addTask(atomizer);
  //ts.addTask(keepAlive);
  checkDHT11Sensors.enable();
  checkWaterLevelSensor.enable();
  printSensorsValues.enable();
  printDisplaySensors.enable();
  atomizer.enable();
}

void loop() {
  ts.execute();
  
  rotary_loop();
}
