# Description

This sample demonstrates the use case "Lora Central Display". It receives data from an LHT65N-E3 sensor and displays it on the screen.

## Prerequisites
### LTS5 LA66
Integrated LA66 has to be programmed with LTS5 P2P firmware and pre-configured via UART.
Default configuration needs to be changed to a higher data rate - SF 7 e.g. - and to inverted downlink polarity using
```
AT+SF=7,7
AT+IQ=1,0
```
Finally this should result in the following configuration:
```
Group: 0 , 0
Syncword: 0x3444 for Public Network
Frequency: 868.700 MHZ , 868.700 MHZ
BandWidth: 125 kHZ , 125 kHZ
Spreading Factor: 7 , 7
Output Power: 14 dBm
Coding Rate: 4/5 , 4/5
Header Type: explicit header , explicit header
CRC: ON , ON
Invert IQ: 1 , 0
Preamble: 8 , 8
RX Mode: 65535 , 0
```

### LHT65N
LHT65N has to be configured to work in ABP mode with fixed channel (frequency) and fixed data rate. Additionally all functions influencing frequency or data rate need to be disabled (ADR and DDETECT e.g.).
You can choose any NwkSKey and AppSKey values. You need to copy these keys and the DevAddr of your LHT65N into LTS5 code then. 
```
AT+NJM=0 
AT+ADR=0
AT+DR=5
AT+CHS=868700000 
AT+NWKSKEY=00000000000000000000000000000000 
AT+APPSKEY=00000000000000000000000000000000 
AT+DDETECT=0,1440,2880
```

## LTS5 ESP32 code
Program is very simple. It receives serial data from integrated LA66, parses the ASCII stream, extracts LoRaWAN PHY frames and MAC frames, checks for correct DevAddr, verifies MIC, AES decrypts payload and finally decodes LHT65N payload.
Received data is displayed in main screen. Additionally if we didn't hear from LHT65 for more than one hour three red exclamation marks are displayed.
If outdoor temperature falls below 2 degrees celsius, screen background turns red.  
If none of the above mentioned alarms are pending LCD backlight is switched off after 20s and turns on again on a touch gesture or if an alarm appears. 

# Changelog

## [1.0.0] - 2026-04-15

Initial version

