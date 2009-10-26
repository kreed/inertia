CFLAGS+=-Wall -lcrypt -lXxf86vm $(shell pkg-config --libs --cflags x11 xext xtst)
SRC=inertia.c
TARGET=inertia
DEST=/usr/local/bin

$(TARGET): inertia.c
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS)

install: $(TARGET)
	install -sm 4755 $(TARGET) $(DEST)
