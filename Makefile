CFLAGS+=-Wall -lcrypt -lXxf86vm $(shell pkg-config --libs --cflags x11 xext)
SRC=inertia.c
TARGET=inertia

all:
	$(CC) -s -fomit-frame-pointer $(SRC) -o $(TARGET) $(CFLAGS)
