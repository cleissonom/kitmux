CC ?= cc
PKG_CONFIG ?= pkg-config
BUILD ?= build
PACKAGES = json-c yaml-0.1 ncursesw libcrypto
CPPFLAGS += -Iinclude $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS += -std=c17 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Wall -Wextra -Wpedantic -g -O2
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))
SOURCES = $(wildcard src/*.c)
OBJECTS = $(patsubst src/%.c,$(BUILD)/%.o,$(SOURCES))

.PHONY: all test test-native sanitize clean install
all: $(BUILD)/kitmux
$(BUILD)/kitmux: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(BUILD)/%.o: src/%.c include/kitmux.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
-include $(OBJECTS:.o=.d)
$(BUILD)/test_runtime_stop: tests/fixtures/runtime_stop.c $(filter-out $(BUILD)/main.o $(BUILD)/tui.o $(BUILD)/tui_%.o,$(OBJECTS))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
test: all $(BUILD)/test_runtime_stop
	KITMUX_BINARY=$(CURDIR)/$(BUILD)/kitmux python3 -m unittest discover -s tests -p 'test_*.py' -v
test-native: all
	KITMUX_NATIVE_TESTS=1 KITMUX_BINARY=$(CURDIR)/$(BUILD)/kitmux PYTHONPATH=$(CURDIR)/tests python3 -m unittest test_client_compatibility test_subagent_native_codex test_subagent_native_claude test_subagent_native_opencode -v
sanitize:
	$(MAKE) BUILD=build-sanitize CFLAGS='-std=c17 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Wall -Wextra -Wpedantic -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
PREFIX ?= /usr/local
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/kitmux $(DESTDIR)$(PREFIX)/bin/kitmux
clean:
	rm -rf build build-sanitize
