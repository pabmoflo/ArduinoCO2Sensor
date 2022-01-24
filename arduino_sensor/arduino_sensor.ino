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
char announceTopic[] = "CO2S/announce"; // MQTT topic for announcing this device has connected (triggers sending the config)
char dataTopic[] = "CO2S/data"; // MQTT topic for sending the CO2 and temperature data
char confTopic[] = "CO2S/conf"; // MQTT topic for recieving the configuration data
constexpr byte LEDRPin = 8; // LED Red output pin
constexpr byte LEDGPin = 9; // LED Green output pin
constexpr byte LEDBPin = 10; // LED Blue output pin
constexpr byte BuzzPin = 22; // Buzzer output pin
constexpr unsigned int BuzzFreq = 2500; // Buzzer frequency (piezo resonance)

// Create the Wifi client and the MQTT client
WiFiEspClient espClient;
PubSubClient client(espClient);

// Create the CO2 sensor
MHZ19 co2Sensor;

// Persistent save data format version 0, if the save layout is changed, this value has to get changed as well
// This data is saved to the internal EEPROM
#define CONFIG_VERSION 0
void genRandomUUID(uint8_t* data);
struct PersistentData
{
  struct CData
  {
    uint8_t uniqueID[0x10]; // Unique ID (hopefully) for this device
    int reserved[5]; // Reserve 5 ints (10 bytes) to prevent changing the EEPROM layout if a change is needed
  } data;
  // Checksum of the data
  unsigned int checksum;

  // Initialize save data function, generates a new random UUID and fills the reserved bytes
  void Init() {
    genRandomUUID(data.uniqueID);
    for (int i = 0; i < sizeof(data.reserved) / sizeof(int); i++) data.reserved[i] = 0xFFFF;
  }
};

// Configuration data sent over MQTT by the nexus program
struct ConfigData
{
  unsigned int measureEachMsec; // Msec between each meassurement
  unsigned int sendAfterMeasures; // Send the data after the specified amount of measurements have taken place. (Mean is calculated)
  unsigned int greenLEDThreshold; // Values below this will make the led green
  unsigned int yellowLEDThreshold; // Values below this will make the led yellow
  unsigned int orangeLEDThreshold; // Values below this will make the led orange, anything else red
  int makeBuzzEverySec; // Time to wait between each buzz when the led is red. Set to negative to disable Buzz
  int enableLEDEverySec; // Time to wait between each time the LED is activated. Set to negative to disable LED
};

// Create the persistent data and the config data
PersistentData pData;
ConfigData currConfig;

// Define the different led colors
enum class LedColor : unsigned long {
  BLACK = 0x0,
  WHITE = 0x3F3F3F,
  RED = 0x3F0000,
  GREEN = 0x003F00,
  BLUE = 0x00003F,
  YELLOW = 0x3F3F00,
  ORANGE = 0x3F1F00,
};

// Change these variables to change the LED color and the buzzer state 
static volatile LedColor ledColor;
static volatile int buzzState = 0; // 0 -> Disable, 1 -> Enable, 2 -> Force

// Variables used to calculate the elapsed time while ticking the LED and buzzer
static unsigned int prevTickTime = 0;
static unsigned int currTickTime = 0;

// Function to immediately change the LED color
void setLedColor(LedColor color) {
  // Calculate the RGB values from the color
  byte r = ((unsigned long)color >> 16);
  byte g = ((unsigned long)color >> 8);
  byte b = (unsigned long)color;
  // Change the RGB pins accordingly
  analogWrite(LEDRPin, r);
  analogWrite(LEDGPin, g);
  analogWrite(LEDBPin, b);
}

// Function to play a tone in the buzzer
void doBuzz(unsigned int msec) {
  // Use the tone function to create a square wave in the digital pin specified.
  tone(BuzzPin, BuzzFreq, msec ? msec : 1);
}

// Function that handles the state of the LED
void tickLED() {
  static unsigned int ledCounter = 0;
  static bool ledEnabled = false;
  ledCounter += (currTickTime - prevTickTime);
  // If the LED is enabled in the config
  if (currConfig.enableLEDEverySec >= 0) {
    if (ledEnabled) {
      // Disable the led after 1 second if it's enabled (if the config is 0, keep it enabled always)
      if (ledCounter >= 100 && (currConfig.enableLEDEverySec != 0)) {
        setLedColor(LedColor::BLACK);
        ledCounter = 0;
        ledEnabled = false;
      }
    } else {
      // Enable the led after the time specified in the config
      if (ledCounter >= currConfig.enableLEDEverySec * 100) {
        setLedColor(ledColor);
        ledCounter = 0;
        ledEnabled = true;
      }
    }
  }
}

// Function that handles the state of the buzzer
void tickBuzz() {
  static unsigned int buzzCounter = 0;
  buzzCounter += (currTickTime - prevTickTime); 
  if (buzzState > 0 && currConfig.makeBuzzEverySec > 0) {
    // Do a 500ms buzz after the time specified in the config, or force it if the state is set to 2
    if (buzzState == 2 || buzzCounter >= (currConfig.makeBuzzEverySec * 100)) {
      doBuzz(500);
      buzzCounter = 0;
      if (buzzState == 2) buzzState = 1;
    }
  }
}

// Function that handles the state of the LED and the buzzer, should be called about every 100ms
void tickLEDBuzzer() {
  currTickTime = millis() / 10;
  tickLED();
  tickBuzz();
  prevTickTime = currTickTime;
}

// Function to test the LED and buzzer are working properly, called when the device reboots
void testLEDBuzz() {
  setLedColor(LedColor::WHITE);
  delay(100);
  setLedColor(LedColor::BLACK);
  
  doBuzz(100);
}

// Initializes the EEPROM wear leveling library, used to increase the life time of the EEPROM chip
void initSaveData() {
  // Calculate the size of the configuration data.
  // For some reason, the size of the control bytes used by the lib is not calculated automatically,
  // that's why 10 bytes are added to th size (not the actual size of the control bytes, but enough
  // to make it work properly as the size is not specified in the library documentation)
  int configSize = sizeof(PersistentData) + 10;
  // Start the EEPROM wear leveling library, we only need a single entry of the persistent data
  EEPROMwl.begin(CONFIG_VERSION, &configSize, 1);
}

// Loads the persistent save data from the EEPROM
void loadSaveData() {
  // Obtain the persistent data from EEPROM
  EEPROMwl.get(0, pData);

  // Calculate the checksum
  unsigned int checksum = 0xB5B5;
  uint8_t* rawData = (uint8_t*)&pData.data;
  for (int i = 0; i < sizeof(PersistentData::CData); i++) {
    checksum += rawData[i];
  }

  // If the checksum is invalid, initialize new persistent save data
  if (checksum != pData.checksum) {
    Serial.println("Config data is corrupted, initializing new data...");
    pData.Init();
    saveSaveData();
  }
}

// Saves the persistent data to EEPROM
void saveSaveData() {
  // Calculate the checksum
  unsigned int checksum = 0xB5B5;
  uint8_t* rawData = (uint8_t*)&pData.data;
  for (int i = 0; i < sizeof(PersistentData::CData); i++) {
    checksum += rawData[i];
  }
  pData.checksum = checksum;

  // Write the data to EEPROM
  EEPROMwl.put(0, pData);
}

// Function that gets called at the start of the execution
void setup() {
  int status = WL_IDLE_STATUS;
  int retryCounter = 0;

  // Disable the watchdog during the setup
  wdt_disable();

  // Wait a bit before starting
  delay(2000);

  // initialize serial for debugging
  Serial.begin(9600);
  // initialize serial1 for ESP module
  Serial1.begin(9600);
  // initialize serial2 for MH-Z19C
  Serial2.begin(9600);

  // Hello world!
  Serial.println("Arduino CO2 Sensor v1.0");

  // Test the LED and buzz work properly, set the led config temporarily to blink blue and disable the buzzer
  testLEDBuzz();
  ledColor = LedColor::BLUE;
  currConfig.makeBuzzEverySec = -1;
  currConfig.enableLEDEverySec = 1;
  buzzState = 0;

  // Initialize and load the persistent save data
  initSaveData();
  loadSaveData();

  // Send the device UUID through serial
  Serial.print("Device UUID: ");
  printUuid(pData.data.uniqueID);
  Serial.println();

  // initialize ESP module
  WiFi.init(&Serial1);

  // initialize CO2 sensor and enable auto-calibration
  co2Sensor.begin(Serial2);
  co2Sensor.autoCalibration();
  
  // check for the presence of the wifi module
  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println("WiFi shield not present");
    // If the wifi module is not found, reboot
    wdt_enable(WDTO_2S);
    while (true);
  }

  // attempt to connect to WiFi network
  retryCounter = 0;
  while ( status != WL_CONNECTED) {
    retryCounter++;
    if (retryCounter >= 6) {
      Serial.println("Failed to connect to the WiFi network");
      // don't continue, wait 2 seconds to reboot
      wdt_enable(WDTO_2S);
      while (true);
    }
    Serial.print("Attempting to connect to WPA SSID: ");
    Serial.println(ssid);
    // Connect to WPA/WPA2 network using ssid and password
    status = WiFi.begin(ssid, pass);
  }

  // Connected to the network, print wifi status
  Serial.println("Connected to the network!");
  printWifiStatus();

  // Set the MQTT server to connect to
  client.setServer(brokerServer, brokerPort);
  client.setCallback(mqttcallback);
    
  // Enable 8 second watchdog, the arduino will automatically reboot if wdt_reset is not called in this time
  wdt_enable(WDTO_8S);
}

// Variables to keep track of process
static bool waitingForConfig = true;
static bool announcementSent = false;
static int subscribedToConfTimer = 50;

// Final MQTT topic for recieving the config data
char finalConfigtopic[30] = { 0 };

// Function called when data is recieved from a subscribed MQTT topic
void mqttcallback(char* topic, byte* payload, unsigned int length) {
  Serial.println("Got mqtt payload...");
  // Check if it's the config topic
  if (strcmp(topic, finalConfigtopic) == 0) {
    // New configuration recieved, update config
    ConfigData* newData = (ConfigData*)payload;
    // Check if the config data is valid
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
    // Check if it's a reboot request and reboot if so
    } else if (length == 7) {
      Serial.println("Recieved reset request, resetting...");
      while(true);
    }
  }
}

// Function that gets called repeatedly after setup()
void loop() {
  static unsigned int counter = 0;
  static unsigned int secondaryCounter = 0;
  static unsigned int oldCounter = 0;
  static unsigned int measurementsTaken = 0;
  static unsigned long currCO2Total = 0;
  static unsigned long currTempTotal = 0;
  static unsigned int configNotRecievedCounter = 0;
  // Wait 50ms for correct timing and to allow the wifi module to process data
  delay(50);
  // Tick the MQTT client, if it fails try to reconnect
  if (!client.loop()) {
    reconnect();
  // If not waiting for config (already go it)
  } else if (!waitingForConfig && (secondaryCounter & 1)) {
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
          // If publishing fails too many times, disconnect the client so it can properly connect again
          if (publishTries > 10) {
            Serial.print("Failed to publish data multiple times, trying to reconnect...");
            client.disconnect();
            break;
          }
        }

        // Log the data through the serial
        sprintf(str, "CO2: %dppm, Temp: %dC", sendData.CO2, sendData.temp);
        Serial.println(str);

        // Update the LED and buzz states using the config thresholds
        if (sendData.CO2 < currConfig.greenLEDThreshold) {
          ledColor = LedColor::GREEN;
          buzzState = 0;
        } else if (sendData.CO2 < currConfig.yellowLEDThreshold) {
          ledColor = LedColor::YELLOW;
          buzzState = 0;
        } else if (sendData.CO2 < currConfig.orangeLEDThreshold) {
          ledColor = LedColor::ORANGE;
          buzzState = 0;
        } else {
          ledColor = LedColor::RED;
          if (buzzState == 0) buzzState = 2; // Force a beep and enable the buzzer
        }

        // Reset the variables
        measurementsTaken = 0;
        currCO2Total = 0;
        currTempTotal = 0;
      }
      oldCounter = counter;
    }
  // If we are waiting for the configuration
  } else if (waitingForConfig && (secondaryCounter & 1)) {
    // Step 1: Subscribe to the configuration topic
    if (subscribedToConfTimer) {
      if (subscribedToConfTimer == 50) {
        Serial.print("Subscribing to config topic: ");
        // Build the topic string from the last segment of the UUID
        char uuidlast[13];
        strcpy(finalConfigtopic, confTopic);
        strcat(finalConfigtopic, "/");
        getLastUUIDSegment(uuidlast, pData.data.uniqueID);
        strcat(finalConfigtopic, uuidlast);
        Serial.println(finalConfigtopic);
        // Try to subscribe to the topic
        int subscribeCunt = 0;
        while (!client.subscribe(finalConfigtopic, 0)) {
          Serial.print("Failed to subscribe to config topic...");
          wdt_reset();
          subscribeCunt++;
          // If subscribing fails too many times, reboot
          if (subscribeCunt > 10) {
            Serial.print("Failed to subscribe to config topic multiple times, restarting...");
            while(true);
          }
        }
        Serial.println("Subscribed to config topic, waiting to send announcement...");
      }
      // Wait a bit before going to step 2
      subscribedToConfTimer--;
    // Step 2: Send announcement through the announcement channel
    }else if (!announcementSent) {
      // Publish our UUID
      client.publish(announceTopic, pData.data.uniqueID, 0x10);
      Serial.println("Announcement sent, waiting for config data...");
      announcementSent = true;
    }
    configNotRecievedCounter++;
    // If config data is not recieved in 1 min, reboot the arduino
    if (configNotRecievedCounter >= 600) {
      Serial.println("Config data not recieved, resetting...");
      while(true);
    }
  }
  if (secondaryCounter & 1) {
    counter++;
    // Tick the LED and buzzer
    tickLEDBuzzer();
  }
  secondaryCounter++;
  // Reset the watchdog, to indicate the code is running and prevent an automatic reboot
  wdt_reset();
}

void reconnect() {
  // Disable the watchdog, as connecting can sometimes take more than 8 seconds
  wdt_disable();
  // Loop until we're connected
  int retryAmount = 0;
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect to the MQTT broker
    if (client.connect("")) {
      Serial.println("connected");
      
    } else {
      // Print the error code and try again in 1 second
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 1 seconds");
      delay(1000);
      retryAmount++;
      // If we retried too many times, reset the arduino
      if (retryAmount >= 3) {
        wdt_enable(WDTO_2S);
        while(true);
      }
    }
  }
  // Enable back the watchdog
  wdt_enable(WDTO_8S);
}

// Algorithm to generate a new random UUID. 
// Uses the (unconnected) analog pin 0 as a source of entrophy.
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

// Prints a hex digit into out buffer
void printHex(char out[3], uint8_t number) {
  int topDigit = number >> 4;
  int bottomDigit = number & 0x0f;
  out[0] = "0123456789abcdef"[topDigit];
  out[1] = "0123456789abcdef"[bottomDigit];
  out[2] = '\0';
}

// Gets the last segment of the UUID, which should be unique enough for the configuration topic
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

// Prints the UUID to serial
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

// Prints the WiFI status to serial
void printWifiStatus()
{
  // print the SSID of the network
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print the WiFi module IP address
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength
  long rssi = WiFi.RSSI();
  Serial.print("Signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
}
