CXX ?= g++
CC ?= gcc
BUILD_DIR ?= build/netcode

CPPFLAGS := -Inet/include -Iserver/include -Ipc/include
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -MMD -MP
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -MMD -MP
LDFLAGS ?=
LDLIBS ?= -ldl

NET_SOURCES := \
	net/src/protocol.cpp \
	net/src/crypto.cpp \
	net/src/fragmentation.cpp \
	net/src/messages.cpp \
	net/src/entity_registry.cpp \
	net/src/session.cpp \
	net/src/interpolation.cpp \
	net/src/reliability.cpp \
	net/src/transport.cpp \
	net/src/client.cpp \
	net/src/c_api.cpp \
	net/src/player_query.cpp \
	net/src/movement.cpp \
	net/src/world.cpp \
	net/src/economy.cpp \
	net/src/shop.cpp \
	net/src/encounter.cpp \
	net/src/npc.cpp \
	net/src/zone.cpp \
	net/src/housing.cpp \
	net/src/replication.cpp \
	server/src/persistence.cpp \
	server/src/gci.cpp \
	server/src/database.cpp \
	server/src/config.cpp \
	server/src/town_clock.cpp \
	server/src/town_runtime.cpp \
	server/src/town_state.cpp

NET_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(NET_SOURCES))
TEST_OBJECT := $(BUILD_DIR)/tests/net/test_main.o
NETWORK_CONFIG_OBJECT := $(BUILD_DIR)/pc/src/pc_network_config.o
SERVER_OBJECTS := $(BUILD_DIR)/server/src/main.o
FUZZ_OBJECT := $(BUILD_DIR)/tests/fuzz/protocol_fuzz.o
LOAD_OBJECT := $(BUILD_DIR)/tests/load/town_load.o
CHAOS_OBJECT := $(BUILD_DIR)/tests/load/town_chaos.o
MONTH_SOAK_OBJECT := $(BUILD_DIR)/tests/load/town_month_soak.o

.PHONY: all test server smoke fuzz load chaos month-soak soak long-soak check clean sanitize

all: $(BUILD_DIR)/netcode_tests $(BUILD_DIR)/AnimalCrossingServer

test: $(BUILD_DIR)/netcode_tests
	$(BUILD_DIR)/netcode_tests

server: $(BUILD_DIR)/AnimalCrossingServer

smoke: $(BUILD_DIR)/AnimalCrossingServer
	$(BUILD_DIR)/AnimalCrossingServer --smoke --ticks 120 --config packaging/server.ini --data $(BUILD_DIR)/smoke-town

fuzz: $(BUILD_DIR)/protocol_fuzz
	$(BUILD_DIR)/protocol_fuzz 50000

load: $(BUILD_DIR)/town_load
	$(BUILD_DIR)/town_load 8 600

chaos: $(BUILD_DIR)/town_chaos
	$(BUILD_DIR)/town_chaos 8 2400

month-soak: $(BUILD_DIR)/town_month_soak
	$(BUILD_DIR)/town_month_soak

soak: month-soak

long-soak: $(BUILD_DIR)/town_load
	$(BUILD_DIR)/town_load 16 216000

check: test fuzz load chaos month-soak smoke

sanitize:
	$(MAKE) clean BUILD_DIR=build/netcode-sanitize
	$(MAKE) test \
		BUILD_DIR=build/netcode-sanitize \
		CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -MMD -MP -fno-omit-frame-pointer -fsanitize=address,undefined" \
		CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -MMD -MP -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDFLAGS="-fsanitize=address,undefined"

$(BUILD_DIR)/netcode_tests: $(NET_OBJECTS) $(NETWORK_CONFIG_OBJECT) $(TEST_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/AnimalCrossingServer: $(NET_OBJECTS) $(SERVER_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/protocol_fuzz: $(NET_OBJECTS) $(FUZZ_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/town_load: $(NET_OBJECTS) $(LOAD_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/town_chaos: $(NET_OBJECTS) $(CHAOS_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/town_month_soak: $(NET_OBJECTS) $(MONTH_SOAK_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(NET_OBJECTS:.o=.d) $(NETWORK_CONFIG_OBJECT:.o=.d) $(TEST_OBJECT:.o=.d) $(SERVER_OBJECTS:.o=.d) $(FUZZ_OBJECT:.o=.d) $(LOAD_OBJECT:.o=.d) $(CHAOS_OBJECT:.o=.d) $(MONTH_SOAK_OBJECT:.o=.d)
