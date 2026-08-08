CXX ?= g++
CC ?= gcc
BUILD_DIR ?= build/netcode

CPPFLAGS := -Inet/include -Iserver/include -Ipc/include -Ithird_party/lua
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -MMD -MP
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -MMD -MP
LDFLAGS ?=
ifeq ($(OS),Windows_NT)
LDLIBS ?= -lws2_32 -lbcrypt
else
LDLIBS ?= -ldl
endif

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
	net/src/turnip.cpp \
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

# Server-only: the Lua host and the vendored interpreter. Deliberately NOT part
# of NET_SOURCES -- the client never runs mod code, and `client-link` links
# exactly NET_OBJECTS to prove the shipped client needs nothing more.
MOD_SOURCES := \
	server/src/mod_registry.cpp \
	server/src/mod_calendar.cpp \
	server/src/mod_packstore.cpp \
	server/src/mod_music.cpp \
	server/src/mod_strings.cpp \
	server/src/mod_host.cpp

LUA_SOURCES := $(wildcard third_party/lua/*.c)

NET_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(NET_SOURCES))
MOD_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(MOD_SOURCES))
LUA_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LUA_SOURCES))
TEST_OBJECT := $(BUILD_DIR)/tests/net/test_main.o
NETWORK_CONFIG_OBJECT := $(BUILD_DIR)/pc/src/pc_network_config.o
SERVER_OBJECTS := $(BUILD_DIR)/server/src/main.o
FUZZ_OBJECT := $(BUILD_DIR)/tests/fuzz/protocol_fuzz.o
# The .pcasset parser is the client's attack surface for server-delivered
# content, so it gets the same bounded-garbage treatment as the protocol
# parsers. Built from pc/ sources but free of SDL and GL by design.
PCASSET_FUZZ_OBJECTS := $(BUILD_DIR)/tests/fuzz/pcasset_fuzz.o $(BUILD_DIR)/pc/src/pc_mod_assets.o
# The client cache carries its own SHA-256 (crypto.cpp is C++ and the client
# links a strict subset), so it is checked against published vectors rather
# than assumed correct.
MOD_CACHE_OBJECTS := $(BUILD_DIR)/tests/fuzz/mod_cache_check.o $(BUILD_DIR)/pc/src/pc_mod_cache.o
# The fetch loop decides what a player waits for and what they never get, and
# its failure modes are all quiet ones -- an asset silently never requested,
# progress that cannot reach its total, a corrupted blob accepted.
MOD_FETCH_OBJECTS := $(BUILD_DIR)/tests/fuzz/mod_fetch_check.o $(BUILD_DIR)/pc/src/pc_mod_fetch.o \
                     $(BUILD_DIR)/pc/src/pc_mod_cache.o

# The registry check links the REAL furniture table (1266 entries) so it
# exercises the actual growth path and pointer swap rather than a mock. Decomp
# headers are not warning-clean, so it uses its own flags.
MOD_REGISTRY_FLAGS := -std=c11 -O0 -g -DTARGET_PC -DVERSION=0 -DF3DEX_GBI_2 -DNDEBUG \
                      -DBUGFIXES -D_LANGUAGE_C -DPC_ENHANCEMENTS \
                      -Iinclude -Isrc -I. -Ipc/include -Inet/include
MOD_MUSIC_OBJECTS := $(BUILD_DIR)/tests/fuzz/mod_music_check.o $(BUILD_DIR)/pc/src/pc_mod_music.o

# The model compiler links decomp headers for Vtx/Gfx, so it uses the registry
# check's flag set rather than the netcode one.
MOD_MODEL_SOURCES := tests/fuzz/mod_model_check.c \
                     pc/src/pc_mod_model.c \
                     pc/src/pc_mod_assets.c \
                     pc/src/pc_mod_arena.c

MOD_REGISTRY_SOURCES := tests/fuzz/mod_registry_check.c \
                        src/data/furniture/ftr_profile_table.c \
                        pc/src/pc_mod_registry.c \
                        pc/src/pc_modloader.c \
                        pc/src/pc_mod_arena.c
LOAD_OBJECT := $(BUILD_DIR)/tests/load/town_load.o
CHAOS_OBJECT := $(BUILD_DIR)/tests/load/town_chaos.o
MONTH_SOAK_OBJECT := $(BUILD_DIR)/tests/load/town_month_soak.o

.PHONY: all test server smoke fuzz load chaos month-soak soak long-soak check clean sanitize client-link mod-cache-check mod-fetch-check mod-registry-check mod-model-check mod-music-check

all: $(BUILD_DIR)/netcode_tests $(BUILD_DIR)/AnimalCrossingServer

test: $(BUILD_DIR)/netcode_tests
	$(BUILD_DIR)/netcode_tests

server: $(BUILD_DIR)/AnimalCrossingServer

smoke: $(BUILD_DIR)/AnimalCrossingServer
	$(BUILD_DIR)/AnimalCrossingServer --smoke --ticks 120 --config packaging/server.ini --data $(BUILD_DIR)/smoke-town

fuzz: $(BUILD_DIR)/protocol_fuzz $(BUILD_DIR)/pcasset_fuzz
	$(BUILD_DIR)/protocol_fuzz 50000
	$(BUILD_DIR)/pcasset_fuzz 50000

mod-cache-check: $(BUILD_DIR)/mod_cache_check
	@rm -rf $(BUILD_DIR)/mod-cache-scratch
	@mkdir -p $(BUILD_DIR)/mod-cache-scratch
	cd $(BUILD_DIR)/mod-cache-scratch && $(abspath $(BUILD_DIR))/mod_cache_check

mod-music-check: $(BUILD_DIR)/mod_music_check
	$(BUILD_DIR)/mod_music_check

$(BUILD_DIR)/mod_music_check: $(MOD_MUSIC_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $^ $(LDFLAGS) -o $@

mod-model-check: $(BUILD_DIR)/mod_model_check
	$(BUILD_DIR)/mod_model_check

$(BUILD_DIR)/mod_model_check: $(MOD_MODEL_SOURCES)
	@mkdir -p $(dir $@)
	$(CC) $(MOD_REGISTRY_FLAGS) $(MOD_MODEL_SOURCES) -o $@

mod-registry-check: $(BUILD_DIR)/mod_registry_check
	$(BUILD_DIR)/mod_registry_check

$(BUILD_DIR)/mod_registry_check: $(MOD_REGISTRY_SOURCES) tests/fuzz/gen_ftr_stubs.py
	@mkdir -p $(dir $@)
	python3 tests/fuzz/gen_ftr_stubs.py $(BUILD_DIR)/ftr_stubs.c
	$(CC) $(MOD_REGISTRY_FLAGS) $(MOD_REGISTRY_SOURCES) $(BUILD_DIR)/ftr_stubs.c -o $@

mod-fetch-check: $(BUILD_DIR)/mod_fetch_check
	@rm -rf $(BUILD_DIR)/mod-fetch-scratch
	@mkdir -p $(BUILD_DIR)/mod-fetch-scratch
	cd $(BUILD_DIR)/mod-fetch-scratch && $(abspath $(BUILD_DIR))/mod_fetch_check

load: $(BUILD_DIR)/town_load
	$(BUILD_DIR)/town_load 8 600

chaos: $(BUILD_DIR)/town_chaos
	$(BUILD_DIR)/town_chaos 8 2400

month-soak: $(BUILD_DIR)/town_month_soak
	$(BUILD_DIR)/town_month_soak

soak: month-soak

long-soak: $(BUILD_DIR)/town_load
	$(BUILD_DIR)/town_load 16 216000

# The client links a strict subset of NET_SOURCES (pc/CMakeLists.txt), so a
# call into a server-only translation unit builds clean here and fails only in
# build_pc.bat. This links exactly what the client is made of.
client-link: $(NET_OBJECTS)
	python3 scripts/check_client_link.py $(BUILD_DIR)

check: test client-link fuzz mod-cache-check mod-fetch-check mod-registry-check mod-model-check mod-music-check load chaos month-soak smoke

sanitize:
	$(MAKE) clean BUILD_DIR=build/netcode-sanitize
	$(MAKE) test \
		BUILD_DIR=build/netcode-sanitize \
		CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -MMD -MP -fno-omit-frame-pointer -fsanitize=address,undefined" \
		CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -MMD -MP -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDFLAGS="-fsanitize=address,undefined"

$(BUILD_DIR)/netcode_tests: $(NET_OBJECTS) $(MOD_OBJECTS) $(LUA_OBJECTS) $(NETWORK_CONFIG_OBJECT) $(TEST_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/AnimalCrossingServer: $(NET_OBJECTS) $(MOD_OBJECTS) $(LUA_OBJECTS) $(SERVER_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/protocol_fuzz: $(NET_OBJECTS) $(MOD_OBJECTS) $(LUA_OBJECTS) $(FUZZ_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/mod_fetch_check: $(MOD_FETCH_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/mod_cache_check: $(MOD_CACHE_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/pcasset_fuzz: $(PCASSET_FUZZ_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/town_load: $(NET_OBJECTS) $(MOD_OBJECTS) $(LUA_OBJECTS) $(LOAD_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/town_chaos: $(NET_OBJECTS) $(MOD_OBJECTS) $(LUA_OBJECTS) $(CHAOS_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/town_month_soak: $(NET_OBJECTS) $(MOD_OBJECTS) $(LUA_OBJECTS) $(MONTH_SOAK_OBJECT)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Vendored Lua gets its own flags: upstream is not clean under -Wpedantic
# -Werror, and patching it would create a merge burden on every update.
# See third_party/lua/VENDORING.md.
LUA_CFLAGS := -std=c99 -O2 -g -DLUA_USE_POSIX -MMD -MP

$(BUILD_DIR)/third_party/lua/%.o: third_party/lua/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -Ithird_party/lua -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(MOD_MUSIC_OBJECTS:.o=.d) $(MOD_FETCH_OBJECTS:.o=.d) $(MOD_CACHE_OBJECTS:.o=.d) $(PCASSET_FUZZ_OBJECTS:.o=.d) $(MOD_OBJECTS:.o=.d) $(LUA_OBJECTS:.o=.d) $(NET_OBJECTS:.o=.d) $(NETWORK_CONFIG_OBJECT:.o=.d) $(TEST_OBJECT:.o=.d) $(SERVER_OBJECTS:.o=.d) $(FUZZ_OBJECT:.o=.d) $(LOAD_OBJECT:.o=.d) $(CHAOS_OBJECT:.o=.d) $(MONTH_SOAK_OBJECT:.o=.d)
