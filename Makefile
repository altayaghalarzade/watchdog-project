CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

SOURCES = src/main.c src/signal_manager.c src/process_manager.c src/logger.c
HEADERS = include/signal_manager.h include/process_manager.h include/logger.h

.PHONY: all test clean
all: watchdog watchdog-worker

watchdog: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

watchdog-worker: workers/worker.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test: all
	sh tests/test_scenarios.sh

clean:
	rm -f watchdog watchdog-worker
