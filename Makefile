CC ?= cc
PKGS = libpipewire-0.3 libmicrohttpd json-c
CPPFLAGS += $(shell pkg-config --cflags $(PKGS)) -I.
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm -pthread

all: eqweb
build:
	mkdir -p build
build/web.h: web.html | build
	xxd -i web.html | sed 's/};/, 0x00 };/' > $@
build/js.h: app.js | build
	xxd -i app.js | sed 's/};/, 0x00 };/' > $@
eqweb: main.c config.c eq.h build/web.h build/js.h
	$(CC) $(CPPFLAGS) $(CFLAGS) main.c config.c -o $@ $(LDFLAGS) $(LDLIBS)
build/test-config: tests/config.c config.c eq.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/config.c config.c -o $@ $(LDFLAGS) $(LDLIBS)
test: eqweb build/test-config
	./build/test-config
	./tests/http.sh
integration: eqweb build/probe build/browser build/latency
	./tests/integration.sh
build/probe: tests/probe.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)
build/browser: tests/browser.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)
build/latency: tests/latency.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)
clean:
	rm -rf build eqweb
.PHONY: all test integration clean
