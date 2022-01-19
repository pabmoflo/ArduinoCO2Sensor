#include <WiFiEsp.h>
#include <WiFiEspClient.h>
#include <WiFiEspUdp.h>
#include <PubSubClient.h>
#include <MHZ19.h>
#include "EEPROMWearLevel.h"
#include <avr/wdt.h>
#include "configData.h"

/*
VS Code Intellisense c_cpp_properties.json defines
"UBRR0H",
"UBRR1H",
"UBRR2H",
"WDP3"
*/


// Global configuration, comes from configData.h (not included for obvious reasons)

char ssid[] = SSID_VALUE; // SSID 
char pass[] = PASS_VALUE; // Password
char brokerServer[] = BROKER_ADDR; // MQTT broker server
int brokerPort = BROKER_PORT; // MQTT broker port
char announceTopic[] = "CO2S/announce";
char dataTopic[] = "CO2S/data";
char confTopic[] = "CO2S/conf";

// Initialize the Ethernet client object
WiFiEspClient espClient;
PubSubClient client(espClient);

// CO2 sensor
MHZ19 co2Sensor;

#define CONFIG_VERSION 0 // Save data version 0
void genRandomUUID(uint8_t* data);
struct PersistentData
{
  struct CData
  {
    uint8_t uniqueID[0x10]; // Unique ID (hopefully) for this device
    int reserved[5]; // Reserve 5 ints (10 bytes) to prevent changing the EEPROM layout if a change is needed
  } data;
  unsigned int checksum;

  void Init() {
    genRandomUUID(data.uniqueID);
    for (int i = 0; i < sizeof(data.reserved) / sizeof(int); i++) data.reserved[i] = 0xFFFF;
  }
};

struct ConfigData
{
  unsigned int measureEachMsec; // Msec between each meassurement
  unsigned int sendAfterMeasures; // Send the data after a certain amount of measurements have taken place. (Mean is calculated)
  unsigned int greenLEDThreshold; // Values below this will make the led green
  unsigned int yellowLEDThreshold; // Values below this will make the led yellow
  unsigned int orangeLEDThreshold; // Values below this will make the led orange, anything else red
  int makeBuzzEverySec; // Time to wait between each buzz when the led is red. Set to negative to disable Buzz
  int enableLEDEverySec; // Time to wait between each time the LED shows a value. Set to negative to disable LED
};

PersistentData pData;
ConfigData currConfig;

enum class LedColor : unsigned long {
  BLACK = 0x0,
  WHITE = 0x3F3F3F,
  RED = 0x3F0000,
  GREEN = 0x003F00,
  BLUE = 0x00003F,
  YELLOW = 0x3F3F00,
  ORANGE = 0x3F1F00,
};
static volatile LedColor ledColor = LedColor::BLUE;
static volatile bool buzzState = 0; // 0 -> Disable, 1 -> Enable, 2 -> Force

void setLedColor(LedColor color) {
  byte r = ((unsigned long)color >> 16);
  byte g = ((unsigned long)color >> 8);
  byte b = (unsigned long)color;
  analogWrite(8, r);
  analogWrite(9, g);
  analogWrite(10, b);
}

void doBuzz(unsigned int msec) {
  tone(22, 2500, msec ? msec : 1);
}

void tickLED() {
  static unsigned int ledCounter = 0;
  static unsigned int lastCounterVal = 0;
  static bool ledEnabled = false;
  if (currConfig.enableLEDEverySec >= 0) {
    if (ledEnabled) {
      if ((ledCounter - lastCounterVal) >= 10 && (currConfig.enableLEDEverySec != 0)) {
        setLedColor(LedColor::BLACK);
        lastCounterVal = ledCounter;
        ledEnabled = false;
      }
    } else {
      if ((ledCounter - lastCounterVal) >= currConfig.enableLEDEverySec * 10) {
        setLedColor(ledColor);
        lastCounterVal = ledCounter;
        ledEnabled = true;
      }
    }
  }
  ledCounter++;
}

void tickBuzz() {
  static unsigned int buzzCounter = 0;
  static unsigned int lastBuzzCounterVal = 0;
  if (buzzState > 0 && currConfig.makeBuzzEverySec > 0) {
    if (buzzState == 2 || buzzCounter - lastBuzzCounterVal >= currConfig.makeBuzzEverySec * 10) {
      doBuzz(500);
      lastBuzzCounterVal = buzzCounter;
      if (buzzState == 2) buzzState = 1;
    }
  }
  buzzCounter++;
}

void tickLEDBuzzer() { // Should be called every 100 ms
  tickLED();
  tickBuzz();
}

void testLEDBuzz() {
  setLedColor(LedColor::WHITE);
  delay(100);
  setLedColor(LedColor::BLACK);
  
  doBuzz(100);
}

void loadSaveData() {
  int configSize = sizeof(PersistentData) + 10; // Why doesn't the library count the amount of bytes needed....
  EEPROMwl.begin(CONFIG_VERSION, &configSize, 1);
  EEPROMwl.get(0, pData);
  unsigned int checksum = 0xB5B5;
  uint8_t* rawData = (uint8_t*)&pData.data;
  for (int i = 0; i < sizeof(PersistentData::CData); i++) {
    checksum += rawData[i];
  }
  if (checksum != pData.checksum) {
    Serial.println("Config data is corrupted, initializing new data...");
    pData.Init();
    saveSaveData();
  }
}

void saveSaveData() {
  unsigned int checksum = 0xB5B5;
  uint8_t* rawData = (uint8_t*)&pData.data;
  for (int i = 0; i < sizeof(PersistentData::CData); i++) {
    checksum += rawData[i];
  }
  pData.checksum = checksum;
  EEPROMwl.put(0, pData);
}

void setup() {
  int status = WL_IDLE_STATUS;
  int retryCounter = 0;

  // Disable the watchdog
  wdt_disable();

  // Wait a bit before starting
  delay(2000);

  // initialize serial for debugging
  Serial.begin(9600);
  // initialize serial for ESP module
  Serial1.begin(9600);
  // initialize serial for MHZ19
  Serial2.begin(9600);

  // Send data through serial
  Serial.println("Arduino CO2 Sensor v1.0");

  // Test the LED and buzz work properly and add interrupt callback
  testLEDBuzz();
  currConfig.makeBuzzEverySec = -1;
  currConfig.enableLEDEverySec = 1;

  // Try to load the save data
  loadSaveData();

  // Send UUID through serial
  Serial.print("Device UUID: ");
  printUuid(pData.data.uniqueID);
  Serial.println();

  // initialize ESP module
  WiFi.init(&Serial1);
  // initialize CO2 sensor
  co2Sensor.begin(Serial2);
  co2Sensor.autoCalibration();
  
  // check for the presence of the shield
  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println("WiFi shield not present");
    // don't continue, wait 2 seconds to restart
    wdt_enable(WDTO_2S);
    while (true);
  }

  // attempt to connect to WiFi network
  retryCounter = 0;
  while ( status != WL_CONNECTED) {
    retryCounter++;
    if (retryCounter >= 6) {
      Serial.println("Failed to connect to the WiFi network");
      // don't continue, wait 2 seconds to restart
      wdt_enable(WDTO_2S);
      while (true);
    }
    Serial.print("Attempting to connect to WPA SSID: ");
    Serial.println(ssid);
    // Connect to WPA/WPA2 network
    status = WiFi.begin(ssid, pass);
  }

  // you're connected now, so print out the data
  Serial.println("Connected to the network!");
  printWifiStatus();

  //connect to MQTT server
  client.setServer(brokerServer, brokerPort);
  client.setCallback(mqttcallback);
    
  // Enable 8 second watchdog, the arduino will automatically reset if wdt_reset is not called in this time
  wdt_enable(WDTO_8S);
}

static bool waitingForConfig = true;
static bool announcementSent = false;
static int subscribedToConfTimer = 50;
char finalConfigtopic[30] = { 0 };

void mqttcallback(char* topic, byte* payload, unsigned int length) {
  Serial.println("Got mqtt payload...");
  if (strcmp(topic, finalConfigtopic) == 0) {
    // New configuration recieved, update config
    ConfigData* newData = (ConfigData*)payload;
    // Check if the config data is for us
    if (length == sizeof(ConfigData)) {
      // Copy the new config data
      memcpy(&currConfig, payload, sizeof(ConfigData));
      // If we are waiting to get our config, continue. Otherwise, we need to reset the Arduino
      if (waitingForConfig) {
        waitingForConfig = false;
        Serial.println("Config data recieved! Started data sampling");
        ledColor = LedColor::BLACK;
      }
      else
        while(true);
    } else if (length == 7) { // Indicates the device should reset
      Serial.println("Recieved reset request, resetting...");
      while(true);
    }
  }
}

void loop() {
  static unsigned int counter = 0;
  static unsigned int oldCounter = 0;
  static unsigned int measurementsTaken = 0;
  static unsigned long currCO2Total = 0;
  static unsigned long currTempTotal = 0;
  static unsigned int configNotRecievedCounter = 0;
  // put your main code here, to run repeatedly:
  delay(100);
  if (!client.loop()) {
    reconnect();
  } else if (!waitingForConfig) {
    // Take measurements when it's time to do so
    if (counter - oldCounter >= currConfig.measureEachMsec / 100) {
      // Obtain CO2 and temperature values and add them to current total
      currCO2Total += co2Sensor.getCO2();
      currTempTotal += co2Sensor.getTemperature();
      measurementsTaken++;
      // If it's time to send the data...
      if (measurementsTaken == currConfig.sendAfterMeasures) {
        char str[30];
        // Format of the payload
        struct SendData
        {
          uint8_t uniqueID[0x10];
          int CO2;
          int temp;
        } sendData;
        
        // Calculate the mean from the data taken and populate the data to send
        memcpy(sendData.uniqueID, pData.data.uniqueID, 0x10);
        sendData.CO2 = currCO2Total / measurementsTaken;
        sendData.temp = currTempTotal / measurementsTaken;

        // Publish the data
        int publishTries = 0;
        while (!client.publish(dataTopic, (uint8_t*)&sendData, sizeof(SendData))) {
          Serial.print("Failed to publish data...");
          wdt_reset();
          publishTries++;
          if (publishTries > 10) {
            Serial.print("Failed to publish data multiple times, trying to reconnect...");
            client.disconnect();
            break;
          }
        }

        // Log the data through the serial
        sprintf(str, "CO2: %dppm, Temp: %dC", sendData.CO2, sendData.temp);
        Serial.println(str);

        // Update the LED and buzz states
        if (sendData.CO2 < currConfig.greenLEDThreshold) {
          ledColor = LedColor::GREEN;
        } else if (sendData.CO2 < currConfig.yellowLEDThreshold) {
          ledColor = LedColor::YELLOW;
        } else if (sendData.CO2 < currConfig.orangeLEDThreshold) {
          ledColor = LedColor::ORANGE;
        } else {
          ledColor = LedColor::RED;
          buzzState = 2;
        }
        // Reset the variables
        measurementsTaken = 0;
        currCO2Total = 0;
        currTempTotal = 0;
      }
      oldCounter = counter;
    }
  } else if (waitingForConfig) {
    if (subscribedToConfTimer) {
      if (subscribedToConfTimer == 50) {
        char uuidlast[13];
        Serial.print("Subscribing to config topic: ");
        strcpy(finalConfigtopic, confTopic);
        strcat(finalConfigtopic, "/");
        getLastUUIDSegment(uuidlast, pData.data.uniqueID);
        strcat(finalConfigtopic, uuidlast);
        Serial.println(finalConfigtopic);
        int subscribeCunt = 0;
        while (!client.subscribe(finalConfigtopic, 0)) {
          Serial.print("Failed to subscribe to config topic...");
          wdt_reset();
          subscribeCunt++;
          if (subscribeCunt > 10) {
            Serial.print("Failed to subscribe to config topic multiple times, restarting...");
            while(true);
          }
        }
        Serial.println("Subscribed to config topic, waiting to send announcement...");
      }
      subscribedToConfTimer--;
    }else if (!announcementSent) {
      client.publish(announceTopic, pData.data.uniqueID, 0x10);
      Serial.println("Announcement sent, waiting for config data...");
      announcementSent = true;
    }
    configNotRecievedCounter++;
    // If config data is not recieved in 1 min, reset the arduino
    if (configNotRecievedCounter >= 600) {
      Serial.println("Config data not recieved, resetting...");
      while(true);
    }
  }
  counter++;
  tickLEDBuzzer();
  wdt_reset();
}

void reconnect() {
  wdt_disable();
  // Loop until we're reconnected
  int retryAmount = 0;
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("")) {
      Serial.println("connected");
      
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 1 seconds");
      // Wait 5 seconds before retrying
      delay(1000);
      retryAmount++;
      // If we retried too many times, reset the arduino
      if (retryAmount >= 3) {
        wdt_enable(WDTO_2S);
        while(true);
      }
    }
  }
  wdt_enable(WDTO_8S);
}

void genRandomUUID(uint8_t* data) {
  for (int j = 0; j < 0x10; j++) {
    int iter = analogRead(0) + 1;
    randomSeed(analogRead(0) * iter + random());
    for (int i = 0; i < iter; i++) {
      random();
    }
    data[j] = random();
  }
  data[6] = 0x40 | (0x0F & data[6]); 
  data[8] = 0x80 | (0x3F & data[8]);
}

void printHex(char out[3], uint8_t number) {
  int topDigit = number >> 4;
  int bottomDigit = number & 0x0f;
  // Print high hex digit
  out[0] = "0123456789abcdef"[topDigit];
  out[1] = "0123456789abcdef"[bottomDigit];
  out[2] = '\0';
}

void getLastUUIDSegment(char out[13], uint8_t* uuidNumber) {
  int i;
  for (i=10; i<16; i++) {
    char print[3];
    printHex(print, uuidNumber[i]);
    *out++ = print[0];
    *out++ = print[1];
  }
  *out = '\0';
}

void printUuid(uint8_t* uuidNumber) {
  int i;
  for (i=0; i<16; i++) {
    if (i==4) Serial.print("-");
    if (i==6) Serial.print("-");
    if (i==8) Serial.print("-");
    if (i==10) Serial.print("-");
    char print[3];
    printHex(print, uuidNumber[i]);
    Serial.print(print);
  }
}

void printWifiStatus()
{
  // print the SSID of the network you're attached to
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your WiFi shield's IP address
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength
  long rssi = WiFi.RSSI();
  Serial.print("Signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
}
