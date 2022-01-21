import struct
from time import sleep
import traceback
import sys
import json
import paho.mqtt.client as mqtt
import uuid
from datetime import datetime, timedelta
import requests
import threading

# MQTT topics
ANNOUNCE_TOPIC = "CO2S/announce"
DATA_TOPIC = "CO2S/data"
CONFIG_TOPIC = "CO2S/conf"
RESET_TOPIC = "CO2S/reset"

# Formats for the MQTT payloads
announceResetStruct = struct.Struct("<16s") # 16 bytes (UUID)
configStruct = struct.Struct("<5H2h") # 5 unsigned shorts, 2 signed shorts
dataStruct = struct.Struct("<16s2h") # 16 bytes (UUID), 2 signed shorts

# MQTT client and other data structures
# Use a lock to prevent the mqtt thread and the main thread to access connectedDevices at the same time
client : mqtt.Client = None 
configData = {}
connectedDevices = {}
connectedLock = threading.Lock()

# The callback for when the client receives a CONNACK response from the server.
def on_connect(client, userdata, flags, rc):
    print("Connected to ", client._host, "port: ", client._port)
    print("Flags: ", flags, "returned code: ", rc)

    client.subscribe(ANNOUNCE_TOPIC, qos=2)
    client.subscribe(DATA_TOPIC, qos=0)

# The callback for when a message is published. (Unused)
def on_publish(client, userdata, mid):
    pass

# The callback for when a MQTT message is recieved
def on_message(client, userdata, msg):
    global connectedLock
    # Announcement message recieved
    if (msg.topic == ANNOUNCE_TOPIC):
        with connectedLock:
            try:
                # Make sure it's the proper size
                assert len(msg.payload) == announceResetStruct.size
                # Get the UUID
                newDevice = uuid.UUID(bytes=announceResetStruct.unpack(msg.payload)[0])
                # Check the device is already connected
                if (newDevice in connectedDevices):
                    # The device may have rebooted due to error, so we remove it before connecting again
                    del connectedDevices[newDevice]
                # Initialize the values to calculate the mean
                connectedDevices[newDevice] = [0, 0, 0] # CO2, temp, times
                # Print and send the config
                print("{} has connected ({})!".format(getDeviceName(newDevice), str(newDevice)))
                sendConfig(newDevice)
            except:
                print("Got corrupted annoucement data!")
                traceback.print_exc()
                pass
    # Data message recieved
    if (msg.topic == DATA_TOPIC):
        try:
            # Make sure it's the proper size
            assert len(msg.payload) == dataStruct.size
            # Get the data from the binary payload
            data = dataStruct.unpack(msg.payload)
            # Obtain its UUID
            device = uuid.UUID(bytes=data[0])
            # If the device has not announced its presence, tell it to reboot by sending a 7 byte payload through the config topic
            if (not device in connectedDevices):
                print("Unrecognized device: {}".format(str(device)))
                sleep(2)
                client.publish(CONFIG_TOPIC + "/" + str(device)[24:], b'aaaaaaa')
            # Print the recieved values and store them to calculate the mean later
            else:
                with connectedLock:
                    print("{} ({}): CO2: {}ppm, Temp: {}ºC".format(getDeviceName(device), str(device), data[1], data[2]))
                    connectedDevices[device][0] += data[1]
                    connectedDevices[device][1] += data[2]
                    connectedDevices[device][2] += 1
        except:
            print("Got corrupted annoucement data!")
            traceback.print_exc()
            pass
    pass

# Get the device name from its UUID
def getDeviceName(device: uuid.UUID):
    global configData
    try:
        return configData["devices"][str(device)]["name"]
    except:
        return ""

# Send the device configuration from its UUID
def sendConfig(device: uuid.UUID):
    global configData
    uuidStr = str(device)
    if not uuidStr in configData["devices"]:
        # First time this device is connected, generate config
        newConf = {}
        newConf["name"] = "Device-{}".format(len(configData["devices"])) # CONF: Display name
        newConf["usesGlobalConfig"] = True # CONF: Wether to use the global config or the decive specific config
        newConf["apiName"] = "XXXX" # CONF: Name of the device in Ubidots
        newConf["config"] = dict.copy(configData["globalConf"]) # The config entries are generated anyways, so that the user can edit them
        configData["devices"][uuidStr] = newConf
        print("Generating configuration for {} ({})".format(newConf["name"], uuidStr))
        # Save the config to the config file
        saveConfig()
    # Calculate which config data to use
    useConf = configData["globalConf"] if configData["devices"][uuidStr]["usesGlobalConfig"] else configData["devices"][uuidStr]["config"]
    try:
        # Pack the config into a binary payload and send it
        cBinaryData = configStruct.pack(useConf["measureEachMsec"], useConf["sendAfterMeasures"],
                                        useConf["greenLEDThreshold"], useConf["yellowLEDThreshold"], useConf["orangeLEDThreshold"],
                                        useConf["makeBuzzEverySec"], useConf["enableLEDEverySec"])
        sleep(2)
        client.publish(CONFIG_TOPIC + "/" + uuidStr[24:], cBinaryData)
    except:
        print("Failed to send config data!")
        traceback.print_exc()

# Save the configuration data in JSON format to the config file
def saveConfig():
    global configData
    try:
        with open(sys.argv[1], "w") as f:
            json.dump(configData, f, indent=4)
    except:
        print("Failed to save configuration file:")
        traceback.print_exc()

# Load the default configuration data (in case it's missing)
def loadConfigDefault():
    global configData
    configData.clear()
    globalConf = {}
    globalConf["measureEachMsec"] = 2000 # CONF: Msec between each meassurement
    globalConf["sendAfterMeasures"] = 10 # CONF: Send the data after the specified amount of measurements have taken place. (Mean is calculated)
    globalConf["greenLEDThreshold"] = 700 # CONF: Values below this will make the led green
    globalConf["yellowLEDThreshold"] = 800 # CONF: Values below this will make the led yellow
    globalConf["orangeLEDThreshold"] = 1000 # CONF: Values below this will make the led orange, anything else red
    globalConf["makeBuzzEverySec"] = 300 # CONF: Time to wait between each buzz when the led is red. Set to negative to disable Buzz
    globalConf["enableLEDEverySec"] = 5 # CONF: Time to wait between each time the LED is activated. Set to negative to disable LED
    configData["globalConf"] = globalConf
    configData["devices"] = {}

    reportConfig = {}
    reportConfig["token"] = "XXXX" # CONF: Ubidots API token
    reportConfig["co2VariableName"] = "XXXX" # CONF: Ubidots CO2 variable name
    reportConfig["tempVariableName"] = "XXXX" # CONF: Ubidots temperature name
    reportConfig["reportEverySeconds"] = 300.0 # CONF: Interval to calculate the mean and upload to Ubidots
    configData["reportConfig"] = reportConfig

    mqttConfig = {}
    mqttConfig["address"] = "localhost" # CONF: Address of the MQTT broker
    mqttConfig["port"] = 1883 # CONF: Port of the MQTT broker
    configData["mqttConfig"] = mqttConfig

# Load the configuration from the config file
def loadConfig():
    global configData
    fConfig = None
    try:
        fConfig = open(sys.argv[1], "r")
        configData = json.load(fConfig)
    except:
        if (fConfig is None):
            print("Config file is missing. generating new file...")
        else:
            print("Config file is corrupted.")
            traceback.print_exc()
            fConfig.close()
            sys.exit(1)
        loadConfigDefault()
        saveConfig()
        return
    fConfig.close()

# Send the specified payload to Ubidots using the device name
def sendUbidotsPayload(payload, deviceName):
    global configData
    # Creates the headers for the HTTP requests
    url = "http://industrial.api.ubidots.com"
    url = "{}/api/v1.6/devices/{}".format(url, deviceName)
    headers = {"X-Auth-Token": configData["reportConfig"]["token"], "Content-Type": "application/json"}

    # Makes the HTTP requests and sends the payload through POST
    status = 400
    attempts = 0
    while status >= 400 and attempts <= 5:
        req = requests.post(url=url, headers=headers, json=payload)
        status = req.status_code
        attempts += 1
        sleep(1)

    # Show the results
    print(req.status_code, req.json())
    if status >= 400:
        return False

    return True

# Main method
def main():
    global client
    global configData
    global connectedLock
    if (len(sys.argv) != 2):
        print("Invalid arguments\nUsage: {} (configFile)".format(sys.argv[0]))
        sys.exit(1)
    
    # Load the configuration
    loadConfig()

    # Init the MQTT client
    client = mqtt.Client(client_id="", 
                        clean_session=True, 
                        userdata=None, 
                        protocol=mqtt.MQTTv311, 
                        transport="tcp")

    # Set the MQTT callbacks
    client.on_connect = on_connect
    client.on_message = on_message

    # Set the MQTT user and password (not implemented) and connect
    client.username_pw_set(None, password=None)
    client.connect(configData["mqttConfig"]["address"], port=configData["mqttConfig"]["port"], keepalive=60)

    # Start the MQTT loop
    client.loop_start()
    print("Started MQTT listener.")


    lastDataSent = datetime.now()

    try:
        while True:
            # When it's time to send the data...
            if (datetime.now() - lastDataSent > timedelta(seconds=configData["reportConfig"]["reportEverySeconds"])):
                if (len(connectedDevices) == 0):
                    lastDataSent = datetime.now()
                    continue
                # For each device connected
                with connectedLock:
                    for device in connectedDevices:
                        deviceData = connectedDevices[device]
                        if (deviceData[2] == 0):
                            continue
                        # Generate the mean of CO2 and temperature and send it to Ubidots using the config
                        co2Mean = deviceData[0] // deviceData[2]
                        tempMean = deviceData[1] // deviceData[2]
                        print("Sending data for {}: CO2: {}ppm, Temp: {}ºC".format(str(device), co2Mean, tempMean))
                        sendUbidotsPayload({configData["reportConfig"]["co2VariableName"]: co2Mean, configData["reportConfig"]["tempVariableName"]: tempMean}, configData["devices"][str(device)]["apiName"])
                        # Reset the data to start calculating the mean again
                        connectedDevices[device] = [0, 0, 0]
                lastDataSent = datetime.now()
            sleep(0.1)
    except KeyboardInterrupt:
        pass
    
    print("Exiting...")
    client.loop_stop()


if __name__ == '__main__':
    main()