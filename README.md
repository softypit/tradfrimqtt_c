# tradfrimqtt_c
c implementation of coap to mqtt daemon to control IKEA tradfri devices

## What's it for?
Enable control of tradfri lighting and outlets via the tradfri gateway using CoAP.  
The daemon uses the mosquitto mqtt client library to subscribe to settings topics and publishes device status when changes occur.  

## Why c?
I've tried a few methods for tradfri control using openhab but these tend to rely on OS calls to coap-client for every command sent to the tradfri gateway.   
Each call results in a new coap context being built and a single exchange occuring with the gateway. The tradfri gateway seems unable to cope with lots
of these calls and after a period of time stops responding and sometimes resets.
I wrote this daemon in c (derived from coap-client) to use libcoap and hold a single connection context to the gateway for all polls/commands.  
My limited testing indicates the gateway remains more stable using this mechanism.

## Testing
This has been tested with my tradfri gateway with 12 bulbs and an outlet controlled via mqtt from openhab.

## Build
This daemon depends on libmosquitto, libjson-c, libcoap and libtinydtls

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

## MQTT commands
For status subscribe to tradfri/status/#  
Daemon will publish to status topics each time a value changes (all values reported at startup or when a new device is discovered)  
bulbs:  
if mains power state changes **tradfri/status/<devicename>/lamp** and **tradfri/status/<deviceid>/lamp** message is **'1'** or **'0'**   
if brightness changes **tradfri/status/<devicename>/brightness** and **tradfri/status/<deviceid>/brightness** message is **'0'** - **'100'**  
if colour temperature changes **tradfri/status/<devicename>/temp** and **tradfri/status/<deviceid>/temp** message is **'0'** - **'100'**  
if on/off state changes **tradfri/status/<devicename>/power** and **tradfri/status/<deviceid>/power** message is **'1'** or **'0'**   
outlets:  
if on/off state changes **tradfri/status/<devicename>/power** and **tradfri/status/<deviceid>/power** message is **'1'** or **'0'**   
if mains power state changes **tradfri/status/<devicename>/mains** and **tradfri/status/<deviceid>/mains** message is **'1'** or **'0'**   
  
devicename is the name of the individual device from the tradfri application  
deviceid is the internal identifier used by the tradfri gateway  
  
To send commands publish to **tradfri/set/\<devicename>/\<parameter>** or **tradfri/set/\<deviceid>/\<parameter>** or **tradfri/set/\<groupname>/\<parameter>**   
'power', 'brightness' and 'temp' parameters are writeable but will be ignored if not valid for the device type (e.g. temp for single-colour bulbs or outlets).  
groupname can be used to set all devices within a group (group name from tradfri app).  

## Issues
In a few weeks of testing with a number of devices connected I have seen a single occasion where the main polling thread gets stuck. I have not managed to debug this fully yet
but restarting the daemon fixed things so it doesn't seem like a tradfri gateway problem.
