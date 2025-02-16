# ESP32 WiFi Monitor

This is a simple esp-idf project used to monitor 802.11 probe requests.

Upon observing a probe request, a json message will be sent over mqtt to the configured broker.

## Configuration

`MQTT_BROKER_URL` - The URL of the mqtt broker (e.g., mqtt://192.168.1.2)  
`NTP_SERVER` - The IP or hostname of the NTP server to use for time synchronization  
`WIFI_SSID` - The SSID of the access point to connect to  
`WIFI_PSK` - The Pre-Shared Key for the specified access point
