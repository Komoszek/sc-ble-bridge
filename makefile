LIBS=gio-unix-2.0
CFLAGS=-O0 -ggdb3 -Wall -Werror `pkg-config --cflags $(LIBS)`
LDFLAGS=`pkg-config --libs $(LIBS)`

.PHONY: clean
default: all
all: bin sc-ble-bridge
sc-ble-bridge: bin/sc-ble-bridge.o
	gcc $^ $(LDFLAGS) -o bin/$@

bin/%.o: %.c
	gcc $(INCLUDE) $(CFLAGS) -o $@ -c $^

bin:
	mkdir bin

clean:
	rm -rf *.o bin/sc-ble-bridge bin
