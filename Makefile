
LD = gcc
CC = gcc

CFLAGS = -g -Wall -ansi -D_XOPEN_SOURCE=500
LDFLAGS = -Wall -ansi
VISIBILITY = -fvisibility=hidden

MLIBDIR = lib
DESTDIR = 
BUILD = build
SRC = src
EXAMPLES = $(SRC)/examples

VERSION=0
RELEASE=$(VERSION).1.3

all: $(BUILD)/libpacketstream.so.$(RELEASE) $(BUILD)/packetstream.h

examples: $(BUILD)/texec

static: $(BUILD)/libpacketstream.a

$(BUILD):
	mkdir $(BUILD)

$(BUILD)/libpacketstream.so.$(RELEASE): $(BUILD)/packetstream.o
	$(LD) $(LDFLAGS) -Wl,-soname,libpacketstream.so.$(VERSION) -shared -lpthread $(BUILD)/packetstream.o -o $(BUILD)/libpacketstream.so.$(RELEASE)
	ln -sf libpacketstream.so.$(RELEASE) $(BUILD)/libpacketstream.so.$(VERSION)
	ln -sf libpacketstream.so.$(RELEASE) $(BUILD)/libpacketstream.so

$(BUILD)/libpacketstream.a: $(BUILD)/packetstream.o
	ar crs $(BUILD)/libpacketstream.a $(BUILD)/packetstream.o

$(BUILD)/packetstream.o: $(BUILD) $(SRC)/packetstream.c $(SRC)/packetstream.h
	$(CC) $(CFLAGS) $(VISIBILITY) -fPIC -o $(BUILD)/packetstream.o -c $(SRC)/packetstream.c

$(BUILD)/packetstream.h: $(BUILD) $(SRC)/packetstream.h
	cp $(SRC)/packetstream.h $(BUILD)/packetstream.h

$(BUILD)/texec: $(BUILD) $(EXAMPLES)/texec.c
	$(CC) $(CFLAGS) $(LDFLAGS) -L$(BUILD) -I$(BUILD) -lpacketstream $(EXAMPLES)/texec.c -o $(BUILD)/texec

install: $(BUILD)/libpacketstream.so.$(RELEASE) $(BUILD)/packetstream.h
	install -Dm 0755 $(BUILD)/libpacketstream.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libpacketstream.so.$(RELEASE)
	ln -sf libpacketstream.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libpacketstream.so.$(VERSION)
	ln -sf libpacketstream.so.$(RELEASE) $(DESTDIR)/usr/$(MLIBDIR)/libpacketstream.so
	install -Dm 0644 $(BUILD)/packetstream.h $(DESTDIR)/usr/include/packetstream.h

install-static: $(BUILD)/libpacketstream.a $(BUILD)/packetstream.h
	install -Dm 0755 $(BUILD)/libpacketstream.a $(DESTDIR)/usr/$(MLIBDIR)/libpacketstream.a
	install -Dm 0644 $(BUILD)/packetstream.h $(DESTDIR)/usr/include/packetstream.h

install-examples: $(BUILD)/texec
	install -Dm 0755 $(BUILD)/texec $(DESTDIR)/usr/bin/texec

clean:
	rm -f $(BUILD)/packetstream.o
	rm -f $(BUILD)/libpacketstream.so $(BUILD)/libpacketstream.so.$(VERSION) $(BUILD)/libpacketstream.so.$(RELEASE)
	rm -f $(BUILD)/libpacketstream.a $(BUILD)/packetstream.h
	rm -f $(BUILD)/texec
