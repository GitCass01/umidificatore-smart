/*
TODO: migliorare la modalità spento, ora non è una vera modalità spenta, nel senso che continua a fare i task
      soluzione: si dovrebbe spegnere proprio l'alimentazione, oppure abilitare/disabilitare dinamicamente i task
TODO: water level sensor
      --> migliorare la normalizzazione dei valori
      --> migliorare la calibrazione
TODO: migliorare il treshold del "manca acqua"
TODO: migliorare MQTT
TODO: breadboard power supply + 9V battery
TODO: commentare per bene il codice
*/

/*
    Autore:   Davide Carniselli
    Github:   https://github.com/GitCass01
    Progetto: Umidificatore Smart
    Licenza:  GNU GENERAL PUBLIC LICENSE version 3 (GPLv3)
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

// WIFI + mqtt setup (vedi Credentials.h)
const String ssid = WIFI_SSID;
const String pass = WIFI_PASSWORD;
WiFiClient espClient;
PubSubClient client(MQTT_SERVER, 1883, espClient);
PubSubClientTools mqtt(client);

// topic mqtt
String ID = "UmidificatoreSmartCass";
const char *DHT11_TEMP = "UmidificatoreSmartCass/dht11/temp";
const char *DHT11_HUM = "UmidificatoreSmartCass/dht11/hum";
const char *DHT11_HIC = "UmidificatoreSmartCass/dht11/hic";
const char *WATER_LEVEL = "UmidificatoreSmartCass/waterLevel";
const char *ATOMIZER = "UmidificatoreSmartCass/atomizer";
const char* MODALITA = "UmidificatoreSmartCass/modalita";
const char *TRESHOLD = "UmidificatoreSmartCass/treshold";
const char *INTERMIT_TIME = "UmidificatoreSmartCass/intermitTime";

#define DHTTYPE DHT11
#define DHTPIN 14
#define RED 32                          // led informativo per capire quando sta/dovrebbe atomizzare l'acqua
#define waterSensorPower 27
#define waterSensorOutput 34
#define WATER_NIL 0                     // valore del water sensor non in acqua
#define WATER_MIN 205                   // valore del water sensor con acqua minima
#define WATER_MID 260                   // valore del water sensor con acqua a metà
#define WATER_MAX 275                   // valore del water sensor completamente immerso
#define ATOMIZER_EN 14                  // pin per abilitare l'atomizzatore
#define ROTARY_ENCODER_A_PIN 33
#define ROTARY_ENCODER_B_PIN 25
#define ROTARY_ENCODER_BUTTON_PIN 26
#define ROTARY_ENCODER_VCC_PIN -1
#define ROTARY_ENCODER_STEPS 4
#define SCREEN_WIDTH 128                // OLED display width, in pixels
#define SCREEN_HEIGHT 64                // OLED display height, in pixels

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // oled ssd1306 128x64
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, ROTARY_ENCODER_VCC_PIN, ROTARY_ENCODER_STEPS);

DHT dht(DHTPIN, DHTTYPE);
Scheduler ts;

// Variabili gloabali

int Mode = 0;
float t = 0;
float h = 0;
float hic = 0;
float temp_treshold = 24.0;
unsigned long previousMillis = 0;
unsigned long millisIntermittenza = 30000;    // delay di atomizzazione (normalmente è più alto, ma può essere cambiato dall'utente tramite rotary encoder)
unsigned long seconds;
unsigned long minutes;
int waterLevelValue;                          // valore ottenuto dal water leve sensor compreso tra [WATER_MIN,WATER_MAX]
int waterLevelY;                              // valore tra 0 e 127, ottenuto normalizzando waterLevelValue
int atomizerState = LOW;                           // qui è un led, poi sarà il water atomizator
//int waterLevelDisplayY2 = 127;                // 127 = pieno

void rotary_onButtonClick() {
    static unsigned long lastTimePressed = 0;
    if (millis() - lastTimePressed < 500) {
            return;
    }
    lastTimePressed = millis();
    
    // gestione click bottone rotary encoder
    Mode = (Mode+1) % 3;
    if (Mode == 1) {
      Serial.println("Modalità Automatica.");
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Modalita' Automatica.");
      display.display();
      mqtt.publish(MODALITA, "Automatico");
    } else if (Mode == 2) {
      atomizza(LOW);
      mqtt.publish(MODALITA, "Intermittenza");
      Serial.println("Modalita' Intermittenza.");
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Modalita' Intermittenza.");
      display.display();
    } else if (Mode == 0) {
      Serial.println("Umidificatore spento.");
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Umidificatore Spento.");
      display.display();
      atomizza(LOW);
      mqtt.publish(MODALITA, "Spento");
    }

    digitalWrite(RED,LOW);
}

void rotary_loop() {
  if (rotaryEncoder.isEncoderButtonClicked()) {
    rotary_onButtonClick();
  }

  int encoderDelta;

  if (Mode == 1) {     // se modalità automatica modifico il treshold
    encoderDelta = rotaryEncoder.encoderChanged();
    if (encoderDelta == 0) {
      return;
    }
  
    if (encoderDelta > 0) {
      temp_treshold = temp_treshold + 0.1;
    } else {
      temp_treshold = temp_treshold - 0.1;
    }

    Serial.print("New Temperature treshold: ");
    Serial.print(temp_treshold);
    Serial.println(F("°C"));
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("New temp treshold: ");
    display.println();
    display.setTextSize(2);
    display.print(temp_treshold);
    display.println(F("C"));
    display.setTextSize(1);
    display.display();
    mqtt.publish(TRESHOLD, String(temp_treshold).c_str());
  } else if (Mode == 2) {        // altrimenti modifico il delay dell'intermittenza
    encoderDelta = rotaryEncoder.encoderChanged();
    if (encoderDelta == 0) {
      return;
    }

    // girare il rotary significa aumentare/diminuire l'intermittenza di 30s
    if (encoderDelta > 0) {
      millisIntermittenza = millisIntermittenza + 15000;
    } else {
      if (millisIntermittenza > 0)
        millisIntermittenza = millisIntermittenza - 15000;
    }

    // formattazione in mm:ss del nuovo tempo di intermittenza da stampare a video
    seconds = millisIntermittenza / 1000;
    minutes = (millisIntermittenza / 1000) / 60;
    seconds %= 60;
    minutes %= 60;
    Serial.print("New intermit time (mm:ss): ");
    Serial.print(minutes);
    Serial.print(":");
    Serial.println(seconds);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("New intermit time:");
    display.println();
    display.print("(mm:ss) ");
    display.setTextSize(2);
    display.print(minutes);
    display.print(":");
    display.println(seconds);
    display.setTextSize(1);
    display.display(); 

    String timeStr = "";
    timeStr += minutes;
    timeStr += ":";
    timeStr += seconds;
    mqtt.publish(INTERMIT_TIME, timeStr);
    
    digitalWrite(RED,LOW);        // ogni volta che cambio il delay spengo il 'led'
  }
}

void IRAM_ATTR readEncoderISR() {
    rotaryEncoder.readEncoder_ISR();
}

// TASKS
Task checkDHT11Sensors(13 * TASK_SECOND, TASK_FOREVER, &dht11Sensors);
Task checkWaterLevelSensor(13 * TASK_SECOND, TASK_FOREVER, &activateWaterSensorPower);
Task printSensorsValues(15 * TASK_SECOND, TASK_FOREVER, &printSensors);
Task printDisplaySensors(15 * TASK_SECOND, TASK_FOREVER, &displaySensors);
Task atomizer(TASK_IMMEDIATE, TASK_FOREVER, &atomize);
//Task keepAlive(14 * TASK_SECOND, TASK_FOREVER, &keepAliveMqtt);

void dht11Sensors() {
  if (Mode == 0)
    return;

  //Serial.println("Checking DHT11 sensor...");
  
  h = dht.readHumidity();
  t = dht.readTemperature();

  // controllo errori del dht, se ci sono esco per ripetere la lettura del sensore
  if (isnan(h) || isnan(t)) {
    Serial.println(F("Failed to read from DHT sensor!"));
    return;
  }

  // Calcolo l'indice di calore (ovvero l'afa, causata da alte temperature e alta umidità)
  hic = dht.computeHeatIndex(t, h, false);
}

void activateWaterSensorPower() {
  if (Mode == 0)
    return;
  
  digitalWrite(waterSensorPower, HIGH);
  checkWaterLevelSensor.setCallback(&waterLevel);
  checkWaterLevelSensor.delay(20);
}

void waterLevel() {
  waterLevelValue = analogRead(waterSensorOutput);
  digitalWrite(waterSensorPower, LOW);

  // calcoli ottenuti dopo una calibrazione iniziale
  if (waterLevelValue <= WATER_MID) {
    waterLevelValue *= 0.6;
  } else if (waterLevelValue <= WATER_MIN) {
    waterLevelValue *= 0.2;
  }

  Serial.print("The water sensor value: ");
  Serial.println(waterLevelValue);

  waterLevelY = map(waterLevelValue, WATER_NIL, WATER_MAX, 0, 128);
  Serial.print("Water level: ");
  Serial.println(waterLevelY);

  checkWaterLevelSensor.setCallback(&activateWaterSensorPower);
}

void displaySensors() {
  if (Mode == 0)
    return;
  
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print(F("Humidity:      "));
  display.print(h);
  display.println(F("%"));
  display.println();
  display.print(F("Temperature:   "));
  display.print(t);
  display.println(F("C"));
  display.println();
  display.print(F("Heat Index:    "));
  display.print(hic);
  display.println(F("C"));
  display.println();

  if (Mode == 1) {
    display.print(F("Temp treshold: "));
    display.print(temp_treshold);
    display.println(F("C"));
    display.println();
  } else {
    display.print("Intermit time: ");
    display.print(minutes);
    display.print(":");
    display.println(seconds);
  }
  
  //display.print(F("Water level:   "));
  display.drawLine(0, 61, waterLevelY, 61, WHITE);
  display.display(); 
}

void printSensors() {
  if (Mode == 0)
    return;

  // DHT11
  Serial.print(F("Humidity: "));
  Serial.print(h);
  Serial.print(F("%  Temperature: "));
  Serial.print(t);
  Serial.print(F("°C "));
  Serial.print(F("  Heat Index: "));
  Serial.print(hic);
  Serial.println(F("°C"));
  Serial.print(F("Temperature treshold: "));
  Serial.print(temp_treshold);
  Serial.println(F("°C"));

  // water level sensor
  Serial.print("Water level: ");
  Serial.println(waterLevelY);

  // invio mqtt
  mqtt.publish(DHT11_TEMP, String(t).c_str());
  mqtt.publish(DHT11_HUM, String(h).c_str());
  mqtt.publish(DHT11_HIC, String(hic).c_str());
  mqtt.publish(WATER_LEVEL, String(waterLevelY).c_str());
}

void intermit() {  
  unsigned long currentMillis = millis();
  
  if (previousMillis == 0)
    previousMillis = currentMillis;
  
  if (currentMillis - previousMillis >= millisIntermittenza) {
    atomizza(!atomizerState);  
    previousMillis = currentMillis;
  }
}

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

void atomize() {
  /*if (waterLevelY <= 40) {
    Serial.println("Livello acqua troppo basso (waterLevelY).");
    digitalWrite(RED, LOW);
    digitalWrite(ATOMIZER_EN, LOW);
    return;
  }*/
  
  if (Mode == 1) {    
    if (t >= temp_treshold) {
      atomizza(HIGH);
    } else {
      atomizza(LOW);
    }
  } else if (Mode == 2) {
    intermit();
  }
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.print("Connecting...");
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
  }
  Serial.println();

  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
}

void setupMQTT() {
  // Connect to MQTT
  Serial.print("Connecting to MQTT: "+String(MQTT_SERVER)+" ... ");
  if (client.connect(ID.c_str())) {
      Serial.println("connected");
      mqtt.publish(MODALITA, "Spento");
  } else {
      Serial.println("Failed, rc="+client.state());
  }
}

/*void keepAliveMqtt() {
  mqtt.publish("UmidificatoreSmartCass/keepAlive", "sono ancora qui");
}*/

void setup() {
  Serial.begin(115200);

  setupWifi();
  setupMQTT();

  // Rotary encoder setup
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  //set boundaries and if values should cycle or not
  //in this example we will set possible values between 0 and 1000;
  //bool circleValues = false;
  //rotaryEncoder.setBoundaries(0, 1000, circleValues); //minValue, maxValue, circleValues true|false (when max go to min and vice versa)
  rotaryEncoder.disableAcceleration(); //acceleration is now enabled by default - disable if you dont need it
  //rotaryEncoder.setAcceleration(250); //or set the value - larger number = more accelearation; 0 or 1 means disabled acceleration

  // dht11 setup
  dht.begin();

  // water level sensor setup
  pinMode(waterSensorPower,OUTPUT);
  digitalWrite(waterSensorPower,LOW);

  // atomizer setup
  pinMode(RED,OUTPUT);
  pinMode(ATOMIZER_EN,OUTPUT);
  digitalWrite(ATOMIZER_EN,atomizerState); 
  
  // Oled setup
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
  }
  delay(2000);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.print("Umidificatore spento.");
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
  //keepAlive.enable();
}

void loop() {
  ts.execute();
  
  rotary_loop();
}
