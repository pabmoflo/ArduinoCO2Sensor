import struct
from time import sleep
import traceback
import sys
import json
import paho.mqtt.client as mqtt
import uuid
from datetime import datetime, timedelta
import requests

ANNOUNCE_TOPIC = "CO2S/announce"
DATA_TOPIC = "CO2S/data"
CONFIG_TOPIC = "CO2S/conf"
RESET_TOPIC = "CO2S/reset"

announceResetStruct = struct.Struct("<16s")
configStruct = struct.Struct("<5H2h")
dataStruct = struct.Struct("<16s2h")

client : mqtt.Client = None 
configData = {}
connectedDevices = {}


# The callback for when the client receives a CONNACK response from the server.
def on_connect(client, userdata, flags, rc):
    print("Connected to ", client._host, "port: ", client._port)
    print("Flags: ", flags, "returned code: ", rc)

    client.subscribe(ANNOUNCE_TOPIC, qos=2)
    client.subscribe(DATA_TOPIC, qos=0)

# The callback for when a message is published.
def on_publish(client, userdata, mid):
    pass

def on_message(client, userdata, msg):
    if (msg.topic == ANNOUNCE_TOPIC):
        try:
            assert len(msg.payload) == announceResetStruct.size
            newDevice = uuid.UUID(bytes=announceResetStruct.unpack(msg.payload)[0])
            if (newDevice in connectedDevices):
                # The device may have rebooted due to error
                del connectedDevices[newDevice]
            connectedDevices[newDevice] = [0, 0, 0] # CO2, temp, times
            print("{} has connected ({})!".format(getDeviceName(newDevice), str(newDevice)))
            sendConfig(newDevice)
        except:
            print("Got corrupted annoucement data!")
            traceback.print_exc()
            pass
    if (msg.topic == DATA_TOPIC):
        try:
            assert len(msg.payload) == dataStruct.size
            data = dataStruct.unpack(msg.payload)
            device = uuid.UUID(bytes=data[0])
            if (not device in connectedDevices):
                print("Unrecognized device: {}".format(str(device)))
                sleep(2)
                client.publish(CONFIG_TOPIC + "/" + str(device)[24:], b'aaaaaaa') # Send 7 byte payload to request disconnect
            else:
                print("{} ({}): CO2: {}ppm, Temp: {}ºC".format(getDeviceName(device), str(device), data[1], data[2]))
                connectedDevices[device][0] += data[1]
                connectedDevices[device][1] += data[2]
                connectedDevices[device][2] += 1
        except:
            print("Got corrupted annoucement data!")
            traceback.print_exc()
            pass
    pass

def getDeviceName(device: uuid.UUID):
    global configData
    try:
        return configData["devices"][str(device)]["name"]
    except:
        return ""

def sendConfig(device: uuid.UUID):
    global configData
    uuidStr = str(device)
    if not uuidStr in configData["devices"]:
        # First time this device is connected, generate config
        newConf = {}
        newConf["name"] = "Device-{}".format(len(configData["devices"]))
        newConf["usesGlobalConfig"] = True
        newConf["apiName"] = "XXXX"
        newConf["config"] = dict.copy(configData["globalConf"]) # The config entries are generated anyways, so that the user can edit them
        configData["devices"][uuidStr] = newConf
        print("Generating configuration for {} ({})".format(newConf["name"], uuidStr))
        saveConfig()
    useConf = configData["globalConf"] if configData["devices"][uuidStr]["usesGlobalConfig"] else configData["devices"][uuidStr]["config"]
    try:
        cBinaryData = configStruct.pack(useConf["measureEachMsec"], useConf["sendAfterMeasures"],
                                        useConf["greenLEDThreshold"], useConf["yellowLEDThreshold"], useConf["orangeLEDThreshold"],
                                        useConf["makeBuzzEverySec"], useConf["enableLEDEverySec"])
        sleep(2)
        client.publish(CONFIG_TOPIC + "/" + uuidStr[24:], cBinaryData)
    except:
        print("Failed to send config data!")
        traceback.print_exc()

def saveConfig():
    global configData
    try:
        with open(sys.argv[1], "w") as f:
            json.dump(configData, f, indent=4)
    except:
        print("Failed to save configuration file:")
        traceback.print_exc()

def loadConfigDefault():
    global configData
    configData.clear()
    globalConf = {}
    globalConf["measureEachMsec"] = 2000
    globalConf["sendAfterMeasures"] = 10
    globalConf["greenLEDThreshold"] = 700
    globalConf["yellowLEDThreshold"] = 800
    globalConf["orangeLEDThreshold"] = 1000
    globalConf["makeBuzzEverySec"] = 300
    globalConf["enableLEDEverySec"] = 5
    configData["globalConf"] = globalConf
    configData["devices"] = {}

    reportConfig = {}
    reportConfig["token"] = "XXXX"
    reportConfig["deviceName"] = "XXXX"
    reportConfig["co2VariableName"] = "XXXX"
    reportConfig["tempVariableName"] = "XXXX"
    reportConfig["reportEverySeconds"] = 300.0
    configData["reportConfig"] = reportConfig

    mqttConfig = {}
    mqttConfig["address"] = "localhost"
    mqttConfig["port"] = 1883
    configData["mqttConfig"] = mqttConfig

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

def sendUbidotsPayload(payload, deviceName):
    global configData
    # Creates the headers for the HTTP requests
    url = "http://industrial.api.ubidots.com"
    url = "{}/api/v1.6/devices/{}".format(url, deviceName)
    headers = {"X-Auth-Token": configData["reportConfig"]["token"], "Content-Type": "application/json"}

    # Makes the HTTP requests
    status = 400
    attempts = 0
    while status >= 400 and attempts <= 5:
        req = requests.post(url=url, headers=headers, json=payload)
        status = req.status_code
        attempts += 1
        sleep(1)

    # Processes results
    print(req.status_code, req.json())
    if status >= 400:
        return False

    return True

def main():
    global client
    global configData
    if (len(sys.argv) != 2):
        print("Invalid arguments\nUsage: {} (configFile)".format(sys.argv[0]))
        sys.exit(1)
    
    loadConfig()

    client = mqtt.Client(client_id="", 
                        clean_session=True, 
                        userdata=None, 
                        protocol=mqtt.MQTTv311, 
                        transport="tcp")

    client.on_connect = on_connect
    client.on_message = on_message

    client.username_pw_set(None, password=None)
    client.connect(configData["mqttConfig"]["address"], port=configData["mqttConfig"]["port"], keepalive=60)

    client.loop_start()
    print("Started MQTT listener.")
    lastDataSent = datetime.now()

    try:
        while True:
            if (datetime.now() - lastDataSent > timedelta(seconds=configData["reportConfig"]["reportEverySeconds"])):
                if (len(connectedDevices) == 0):
                    lastDataSent = datetime.now()
                    continue
                for device in connectedDevices:
                    deviceData = connectedDevices[device]
                    if (deviceData[2] == 0):
                        continue
                    co2Mean = deviceData[0] // deviceData[2]
                    tempMean = deviceData[1] // deviceData[2]
                    print("Sending data for {}: CO2: {}ppm, Temp: {}ºC".format(str(device), co2Mean, tempMean))
                    sendUbidotsPayload({configData["reportConfig"]["co2VariableName"]: co2Mean, configData["reportConfig"]["tempVariableName"]: tempMean}, configData["devices"][str(device)]["apiName"])
                lastDataSent = datetime.now()
            sleep(0.1)
    except KeyboardInterrupt:
        pass
    
    print("Exiting...")
    client.loop_stop()


if __name__ == '__main__':
    main()