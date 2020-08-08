# tradfrimqtt_c
c implementation of coap to mqtt daemon to control IKEA tradfri devices

## What's it for?
Enable control of tradfri lighting, outlets and blinds via the tradfri gateway using CoAP.  
The daemon uses the mosquitto mqtt client library to subscribe to settings topics and publishes device status when changes occur.  

## Why a daemon written in c rather than Python?
Many Tradfri connection applications or home-automation bindings utilise coap-client as their mechanism to communicate with  
the tradfri gateway. Each time coap-client is used to read or set tradfri parameters a DTLS session is created and a single  
coap poll is actioned. In my experience repeated connections using this mechanism can cause the tradfri gateway to stop  
responding and occasionally reset.  
I wrote this daemon in c (derived from coap-client) to use libcoap and hold a single connection context to the gateway for  
all polls/commands. It is very stable even when polling large numbers of devices repeatedly. This in turn means that tradfri  
devices controlled using their own remote control devices can rapidly update their current status via mqtt publish messages.  

## Testing
This has been tested with my tradfri gateway with 16 bulbs, two fyrtur blinds and an outlet all controlled via mqtt from openhab.  
I run openhab and tradfrimqttc on a Raspberry Pi 3B+ running openhabian.  

## Build
This daemon depends on libmosquitto, libjson-c, libcoap and libtinydtls.  

## usage
tradfrimqtt <opts> OR use the start script - this can be used with systemd or init.d  
options:  
-d \<time in s> - startup delay - use if broker runs on localhost and starting at boot to give the broker time to start  
-u "\<username>" - username for the tradfri gateway  
-k "\<key>" - key for tradfri gateway  
-g "\<tradfri gateway ip>" - ip of tradfri gateway  
-b "\<broker ip>" - ip for mqtt broker  
-c "tradfriclient" - mqtt client id  
-s \<port (default 1883)> - mqtt port  
-t \<basetopic (default tradfri/)> - mqtt base topic for publish/subscription  
-l \<0-4> - log level (none/changes/errors/trace/debug)  
-f \<tracefile> e.g. /tmp/log/tradfri_mqtt.log
-w \<waittime> maximum wait time for response

## Features
All devices are polled every 10 seconds and changes to any parameters are published. This keeps track of any changes from dimmers/tradfri app.
The gateway device list is polled every 15 minutes to discover new devices.

Added in 0.95 is restarting the DTLS session after a few failed polls. This fixes a previous issue where after a reset of the gateway
a manual reset of this application was required to rebuild the CoAP DTLS context.

Added in 0.97 is support for fyrtur blinds (should also work with kadrilj). Also monitoring of battery status for battery powered devices  
like blinds and remote controls.

## MQTT commands
For status subscribe to tradfri/status/#  
Daemon will publish to status topics each time a value changes (all values reported at startup or when a new device is discovered)  
bulbs:  
if mains power state changes **tradfri/status/\<devicename>/lamp** and **tradfri/status/\<deviceid>/lamp** message is **'1'** or **'0'**   
if brightness changes **tradfri/status/\<devicename>/brightness** and **tradfri/status/\<deviceid>/brightness** message is **'0'** - **'100'**  
if colour temperature changes **tradfri/status/\<devicename>/temp** and **tradfri/status/\<deviceid>/temp** message is **'0'** - **'100'**  
if on/off state changes **tradfri/status/\<devicename>/power** and **tradfri/status/\<deviceid>/power** message is **'1'** or **'0'**   
outlets:  
if on/off state changes **tradfri/status/\<devicename>/power** and **tradfri/status/\<deviceid>/power** message is **'1'** or **'0'**   
if mains power state changes **tradfri/status/\<devicename>/mains** and **tradfri/status/\<deviceid>/mains** message is **'1'** or **'0'**   
blinds:
if level changes **tradfri/status/\<devicename>/level** and **tradfri/status/\<deviceid>/level** message is **'0.0'** - **'100.0'**   
battery state:
if status changes **tradfri/status/\<devicename>/battery** and **tradfri/status/\<deviceid>/battery** message is **'0'** - **'100'**
Also 'low' state **tradfri/status/\<devicename>/batterylow** and **tradfri/status/\<deviceid>/batterylow** message is **'true'** or **'false'**

devicename is the name of the individual device from the tradfri application  
deviceid is the internal identifier used by the tradfri gateway  

To send commands publish to **tradfri/set/\<devicename>/\<parameter>** or **tradfri/set/\<deviceid>/\<parameter>** or **tradfri/set/\<groupname>/\<parameter>**   
'power', 'brightness' and 'temp' parameters are writeable but will be ignored if not valid for the device type (e.g. temp for single-colour bulbs or outlets).  
groupname can be used to set all devices within a group (group name from tradfri app).  
