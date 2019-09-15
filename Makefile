CFLAGS += -I /usr/local/include -std=c99 -g -O3 -Wall -D_GNU_SOURCE -DWITH_POSIX #-Werror
LDFLAGS += -lmosquitto -ljson-c -lcoap-1 -ltinydtls -lpthread

# Uncomment this to print out debugging info.
#CFLAGS += -DDEBUG

PROJECT=tradfrimqtt

all: ${PROJECT}

tradfrimqtt: tradfrimqtt.o
	gcc -o tradfrimqtt tradfrimqtt.o $(LDFLAGS)

clean:
	rm -rf *.o ${PROJECT}

install:
	cp tradfrimqtt /usr/local/bin/tradfrimqtt

