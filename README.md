# Nextwaves Industries firmware for fixed reader
> Version 0.0.1
> ESP-IDF Version: 5.3.3

```MQTT BROKER CONFIG:
Host: 9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud
Port: 8883
Username: helloworld
Password: Hh1234567

COMMAND TOPICS:
reader/esp32_rfid_reader/cmd/rfid
reader/esp32_rfid_reader/cmd/power
reader/esp32_rfid_reader/cmd/inventory

DATA TOPICS:
reader/esp32_rfid_reader/data/realtime
rfid/tags/status

RFID COMMANDS:
{"action": "start"}
{"action": "stop"}
{"action": "status"}
{"action": "get"}

POWER COMMANDS:
{"action": "get"}
{"action": "set", "ant1": 30, "ant2": 30, "ant3": 30, "ant4": 30}
{"action": "query"}
{"action": "status"}

MOSQUITTO COMMANDS:
# Listen to real-time data
mosquitto_sub -h 9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud -p 8883 --capath /etc/ssl/certs/ -u helloworld -P Hh1234567 -t "reader/esp32_rfid_reader/data/realtime"

# Listen to all topics
mosquitto_sub -h 9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud -p 8883 --capath /etc/ssl/certs/ -u helloworld -P Hh1234567 -t "reader/esp32_rfid_reader/#"

# Start RFID inventory
mosquitto_pub -h 9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud -p 8883 --capath /etc/ssl/certs/ -u helloworld -P Hh1234567 -t "reader/esp32_rfid_reader/cmd/rfid" -m '{"action": "start"}'

# Stop RFID inventory
mosquitto_pub -h 9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud -p 8883 --capath /etc/ssl/certs/ -u helloworld -P Hh1234567 -t "reader/esp32_rfid_reader/cmd/rfid" -m '{"action": "stop"}'

# Get RFID status
mosquitto_pub -h 9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud -p 8883 --capath /etc/ssl/certs/ -u helloworld -P Hh1234567 -t "reader/esp32_rfid_reader/cmd/rfid" -m '{"action": "get"}'

# Get power levels
mosquitto_pub -h 9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud -p 8883 --capath /etc/ssl/certs/ -u helloworld -P Hh1234567 -t "reader/esp32_rfid_reader/cmd/power" -m '{"action": "get"}'

# Set power levels
mosquitto_pub -h 9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud -p 8883 --capath /etc/ssl/certs/ -u helloworld -P Hh1234567 -t "reader/esp32_rfid_reader/cmd/power" -m '{"action": "set", "ant1": 25, "ant2": 30, "ant3": 35, "ant4": 40}'

QUICK TEST SEQUENCE:
1. Run subscriber first: mosquitto_sub ... -t "reader/esp32_rfid_reader/#"
2. Send start command: mosquitto_pub ... -m '{"action": "start"}'
3. Watch for real-time tag data in subscriber
4. Send stop command: mosquitto_pub ... -m '{"action": "stop"}'

