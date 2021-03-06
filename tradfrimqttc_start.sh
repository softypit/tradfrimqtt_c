#!/bin/sh

start() {
    mkdir -p /tmp/log
    /usr/bin/tradfrimqtt -d 15 -u "<username>" -k "<key>" -g "<tradfri gateway ip>" -b "<broker ip>" -c "tradfriclient" -s 1883 -l 1 -f /tmp/log/tradfri_mqtt.log &
    mypid = $!
    echo -n $mypid > /var/tradfri_mqtt_pid
}

stop() {
    [ -e /var/tradfri_mqtt_pid ] && {
        mypid = $(cat /var/tradfri_mqtt_pid)
        kill $mypid
	    rm /var/tradfri_mqtt_pid
    }
}

case $1 in 
  start|stop) "$1" ;;
esac

