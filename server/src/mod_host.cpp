#include "acserver/mod_host.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace acserver {
namespace {

/* Per-state allocator bookkeeping. Held in the Mod, addressed through the
 * lua_State's userdata. */
struct AllocState {
    std::size_t used = 0;
    std::size_t limit = 0;
    std::uint64_t denials = 0;
};

/* The memory ceiling, enforced where allocation happens rather than sampled
 * afterwards. Returning null makes Lua raise a catchable "not enough memory",
 * which lands in the enclosing lua_pcall like any other error. */
void* mod_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* state = static_cast<AllocState*>(ud);
    const std::size_t previous = (ptr != nullptr) ? osize : 0;
    if (nsize == 0) {
        state->used -= previous;
        std::free(ptr);
        return nullptr;
    }
    if (state->used - previous + nsize > state->limit) {
        ++state->denials;
        return nullptr;
    }
    void* fresh = std::realloc(ptr, nsize);
    if (fresh != nullptr) state->used = state->used - previous + nsize;
    return fresh;
}

/* Instruction budget. Raises inside the running chunk, so it unwinds to the
 * enclosing lua_pcall rather than killing the process. */
void budget_hook(lua_State* L, lua_Debug*) {
    lua_sethook(L, nullptr, 0, 0);   /* let the error path run without re-tripping */
    luaL_error(L, "instruction budget exhausted");
}

/* Deterministic replacement for math.random.
 *
 * Two reasons the stock one will not do: it seeds from the clock, which would
 * make a mod's behaviour unreproducible from a checkpoint, and `os` is absent
 * so a mod cannot observe real time by any other route. Randomness a mod sees
 * is therefore a pure function of server state. */
int mod_random(lua_State* L) {
    auto* seed = static_cast<std::uint64_t*>(lua_touserdata(L, lua_upvalueindex(1)));
    /* splitmix64: small, no state beyond the seed, well-distributed. */
    std::uint64_t z = (*seed += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);

    const int argc = lua_gettop(L);
    if (argc == 0) {
        lua_pushnumber(L, static_cast<lua_Number>(z >> 11) / 9007199254740992.0);
        return 1;
    }
    lua_Integer low = 1;
    lua_Integer high = luaL_checkinteger(L, 1);
    if (argc >= 2) {
        low = high;
        high = luaL_checkinteger(L, 2);
    }
    if (low > high) return luaL_error(L, "bad argument to 'random' (interval is empty)");
    const std::uint64_t span = static_cast<std::uint64_t>(high - low) + 1ull;
    lua_pushinteger(L, low + static_cast<lua_Integer>(z % span));
    return 1;
}

/* Copies one named function from the library on the stack top into a fresh
 * table, then discards the rest. This is why the whitelist is additive: a
 * function nobody listed cannot appear by accident. */
void copy_field(lua_State* L, int from, const char* name, int to) {
    lua_getfield(L, from, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_setfield(L, to, name);
}

const char* const kBaseWhitelist[] = {
    "assert", "error", "ipairs", "next", "pairs", "select",
    "tonumber", "tostring", "type", "pcall", "xpcall", "unpack",
};

const char* const kStringWhitelist[] = {
    "byte", "char", "find", "format", "gmatch", "gsub", "len",
    "lower", "match", "rep", "reverse", "sub", "upper",
};

const char* const kTableWhitelist[] = { "concat", "insert", "remove", "sort", "unpack" };

const char* const kMathWhitelist[] = {
    "abs", "ceil", "floor", "fmod", "huge", "max", "maxinteger", "min",
    "mininteger", "modf", "pi", "sqrt", "tointeger", "type",
};

} // namespace

/* --- The `calendar` table ------------------------------------------------
 *
 * Registration only, for now. A mod calls calendar.register{...} at the top
 * level of its chunk; the host validates the spec immediately so an error names
 * the mod and the field rather than surfacing later as a holiday on the wrong
 * day. Effects (announce, set_weather, grant) and the query side (today,
 * is_active) arrive with the runtime wiring.
 *
 * Everything is namespaced <mod-id>.<id> on the way in, so two mods can both
 * declare "harvest" without colliding.
 */

namespace {

struct RegistrationContext {
    std::string mod_id;
    std::vector<HolidaySpec>* sink = nullptr;
    std::string error;          /* first failure; registration stops reporting after it */
};

/* Reads an optional integer field, leaving `value` alone when absent. Returns
 * false only when the field is present but not an integer -- a mod writing
 * `week = "second"` should be told, not silently given week 0. */
bool read_int_field(lua_State* L, int table, const char* key, int& value, std::string& error) {
    lua_getfield(L, table, key);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return true; }
    if (!lua_isinteger(L, -1)) {
        error = std::string("field '") + key + "' must be an integer";
        lua_pop(L, 1);
        return false;
    }
    value = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return true;
}

bool read_bool_field(lua_State* L, int table, const char* key, bool& value, std::string& error) {
    lua_getfield(L, table, key);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return true; }
    if (!lua_isboolean(L, -1)) {
        error = std::string("field '") + key + "' must be a boolean";
        lua_pop(L, 1);
        return false;
    }
    value = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return true;
}

bool read_string_field(lua_State* L, int table, const char* key, std::string& value, std::string& error) {
    lua_getfield(L, table, key);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return true; }
    if (lua_type(L, -1) != LUA_TSTRING) {
        error = std::string("field '") + key + "' must be a string";
        lua_pop(L, 1);
        return false;
    }
    value = lua_tostring(L, -1);
    lua_pop(L, 1);
    return true;
}

/* 0 = Sunday, matching lbRTC_SUNDAY and the game's own indexing. */
bool weekday_from_name(const std::string& name, int& out) {
    static const char* const kNames[7] = { "sunday", "monday", "tuesday", "wednesday",
                                           "thursday", "friday", "saturday" };
    for (int i = 0; i < 7; ++i) {
        if (name == kNames[i]) { out = i; return true; }
    }
    return false;
}

/* Query and effect bindings.
 *
 * Each closes over a WorldContext holding the mod's id and the ModWorld the
 * runtime installed. A null world means the runtime has not wired itself up
 * yet -- registration-only tests hit this -- and every binding reports that
 * rather than dereferencing it. */

struct WorldContext {
    std::string mod_id;
    ModWorld* world = nullptr;
};

WorldContext* world_ctx(lua_State* L) {
    return static_cast<WorldContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int calendar_today(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    if (ctx->world == nullptr) return luaL_error(L, "calendar.today: the town is not available yet");
    const acnet::TownDate date = ctx->world->today();
    lua_newtable(L);
    const auto set = [&L](const char* key, int value) {
        lua_pushinteger(L, value);
        lua_setfield(L, -2, key);
    };
    set("year", date.year);
    set("month", date.month);
    set("day", date.day);
    set("hour", date.hour);
    set("weekday", date.weekday);
    return 1;
}

int calendar_weather(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    if (ctx->world == nullptr) return luaL_error(L, "calendar.weather: the town is not available yet");
    static const char* const kNames[4] = { "clear", "cloudy", "rain", "snow" };
    const int kind = ctx->world->weather();
    lua_pushstring(L, (kind >= 0 && kind < 4) ? kNames[kind] : "clear");
    return 1;
}

int calendar_is_active(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    const char* id = luaL_checkstring(L, 1);
    if (ctx->world == nullptr) { lua_pushboolean(L, 0); return 1; }
    /* A mod names its own holiday unqualified; the namespace is added here so a
     * mod cannot probe another mod's calendar. */
    lua_pushboolean(L, ctx->world->holiday_active(ctx->mod_id + "." + id) ? 1 : 0);
    return 1;
}

int calendar_players_online(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    lua_newtable(L);
    if (ctx->world == nullptr) return 1;
    const std::vector<std::uint64_t> accounts = ctx->world->players_online();
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(accounts[i]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int calendar_set_weather(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    const char* name = luaL_checkstring(L, 1);
    static const char* const kNames[4] = { "clear", "cloudy", "rain", "snow" };
    int kind = -1;
    for (int i = 0; i < 4; ++i) {
        if (std::strcmp(name, kNames[i]) == 0) { kind = i; break; }
    }
    if (kind < 0) return luaL_error(L, "calendar.set_weather: unknown weather '%s'", name);
    if (ctx->world == nullptr) return luaL_error(L, "calendar.set_weather: the town is not available yet");
    lua_pushboolean(L, ctx->world->set_weather(kind) ? 1 : 0);
    return 1;
}

int calendar_announce(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    const char* key = luaL_checkstring(L, 1);
    if (ctx->world == nullptr) return luaL_error(L, "calendar.announce: the town is not available yet");
    ctx->world->announce(ctx->mod_id, key);
    return 0;
}

int calendar_grant(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    const lua_Integer account = luaL_checkinteger(L, 1);
    const lua_Integer item = luaL_checkinteger(L, 2);
    if (account <= 0) return luaL_error(L, "calendar.grant: account must be positive");
    if (item < 0 || item > 0xFFFF) return luaL_error(L, "calendar.grant: item is out of range");
    if (ctx->world == nullptr) return luaL_error(L, "calendar.grant: the town is not available yet");
    /* The runtime commits through EconomyAuthority and journals before this
     * returns true, so a mod told the grant succeeded can rely on it surviving
     * a crash. */
    lua_pushboolean(L, ctx->world->grant_item(static_cast<std::uint64_t>(account),
                                              static_cast<std::uint16_t>(item)) ? 1 : 0);
    return 1;
}

/* store/load take numbers, strings and booleans. Everything is rendered to text
 * with a one-character type tag so load() can hand back the type it was given
 * rather than always a string. */
int calendar_store(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    const char* key = luaL_checkstring(L, 1);
    if (ctx->world == nullptr) return luaL_error(L, "calendar.store: the town is not available yet");

    std::string encoded;
    switch (lua_type(L, 2)) {
        case LUA_TNUMBER:
            encoded = "n" + std::string(lua_tostring(L, 2));
            break;
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, 2, &length);
            if (length > 512) return luaL_error(L, "calendar.store: strings are limited to 512 bytes");
            encoded = "s" + std::string(text, length);
            break;
        }
        case LUA_TBOOLEAN:
            encoded = lua_toboolean(L, 2) ? "b1" : "b0";
            break;
        default:
            return luaL_error(L, "calendar.store: value must be a number, string or boolean");
    }
    if (std::strlen(key) > 64) return luaL_error(L, "calendar.store: keys are limited to 64 bytes");
    lua_pushboolean(L, ctx->world->store(ctx->mod_id, key, encoded) ? 1 : 0);
    return 1;
}

int calendar_load(lua_State* L) {
    WorldContext* ctx = world_ctx(L);
    const char* key = luaL_checkstring(L, 1);
    std::string encoded;
    if (ctx->world == nullptr || !ctx->world->load(ctx->mod_id, key, encoded) || encoded.empty()) {
        lua_pushnil(L);
        return 1;
    }
    const std::string body = encoded.substr(1);
    switch (encoded[0]) {
        case 'n': lua_pushnumber(L, std::strtod(body.c_str(), nullptr)); break;
        case 'b': lua_pushboolean(L, body == "1" ? 1 : 0); break;
        case 's': lua_pushlstring(L, body.data(), body.size()); break;
        default:  lua_pushnil(L); break;
    }
    return 1;
}

int calendar_register(lua_State* L) {
    auto* ctx = static_cast<RegistrationContext*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (ctx->sink == nullptr) {
        return luaL_error(L, "calendar.register may only be called while the mod is loading");
    }
    luaL_checktype(L, 1, LUA_TTABLE);

    HolidaySpec spec;
    std::string field_error;
    std::string local_id;
    if (!read_string_field(L, 1, "id", local_id, field_error) ||
        !read_string_field(L, 1, "name", spec.name_key, field_error)) {
        return luaL_error(L, "%s", field_error.c_str());
    }
    if (local_id.empty()) return luaL_error(L, "calendar.register: 'id' is required");
    if (spec.name_key.empty()) return luaL_error(L, "calendar.register: 'name' is required");
    spec.id = ctx->mod_id + "." + local_id;

    /* The date table selects the recurrence by which keys it carries, so a mod
     * writes one obvious thing rather than a form tag plus fields. */
    lua_getfield(L, 1, "date");
    if (!lua_istable(L, -1)) return luaL_error(L, "calendar.register: 'date' table is required");
    const int date = lua_gettop(L);

    std::string weekday_name;
    std::string computed_name;
    int week = 0;
    bool has_day = false;
    bool has_every = false;

    if (!read_int_field(L, date, "month", spec.month, field_error) ||
        !read_int_field(L, date, "week", week, field_error) ||
        !read_int_field(L, date, "days_after", spec.days_after, field_error) ||
        !read_string_field(L, date, "computed", computed_name, field_error)) {
        return luaL_error(L, "%s", field_error.c_str());
    }

    lua_getfield(L, date, "day");
    if (!lua_isnil(L, -1)) {
        if (!lua_isinteger(L, -1)) return luaL_error(L, "calendar.register: date.day must be an integer");
        spec.day = static_cast<int>(lua_tointeger(L, -1));
        has_day = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, date, "every");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "calendar.register: date.every must be a string");
        weekday_name = lua_tostring(L, -1);
        has_every = true;
    }
    lua_pop(L, 1);

    if (!has_every && !read_string_field(L, date, "weekday", weekday_name, field_error)) {
        return luaL_error(L, "%s", field_error.c_str());
    }

    if (!computed_name.empty()) {
        spec.recurrence = Recurrence::Computed;
        if (computed_name == "vernal_equinox") spec.computed = ComputedDate::VernalEquinox;
        else if (computed_name == "autumn_equinox") spec.computed = ComputedDate::AutumnEquinox;
        else return luaL_error(L, "calendar.register: unknown computed date '%s'", computed_name.c_str());
    } else if (has_every) {
        spec.recurrence = Recurrence::EveryWeekday;
        if (!weekday_from_name(weekday_name, spec.weekday))
            return luaL_error(L, "calendar.register: unknown weekday '%s'", weekday_name.c_str());
    } else if (!weekday_name.empty()) {
        if (!weekday_from_name(weekday_name, spec.weekday))
            return luaL_error(L, "calendar.register: unknown weekday '%s'", weekday_name.c_str());
        /* week = "last" is spelled as week 6 in the stock schedule's encoding;
         * here it is the absence of a numeric week plus a `last` flag, which is
         * less clever and reads better in a mod. */
        bool last = false;
        if (!read_bool_field(L, date, "last", last, field_error))
            return luaL_error(L, "%s", field_error.c_str());
        if (last) {
            spec.recurrence = Recurrence::LastWeekday;
        } else {
            if (week < 1) return luaL_error(L, "calendar.register: date.week is required with date.weekday");
            spec.recurrence = Recurrence::NthWeekday;
            spec.week = week;
        }
    } else if (has_day) {
        spec.recurrence = Recurrence::FixedDate;
    } else {
        return luaL_error(L, "calendar.register: 'date' needs one of day, weekday, every or computed");
    }
    lua_pop(L, 1);   /* date table */

    lua_getfield(L, 1, "hours");
    if (lua_istable(L, -1)) {
        const int hours = lua_gettop(L);
        if (!read_int_field(L, hours, "from", spec.hour_from, field_error) ||
            !read_int_field(L, hours, "to", spec.hour_to, field_error)) {
            return luaL_error(L, "%s", field_error.c_str());
        }
    } else if (!lua_isnil(L, -1)) {
        return luaL_error(L, "calendar.register: 'hours' must be a table");
    }
    lua_pop(L, 1);

    if (!read_bool_field(L, 1, "marker", spec.marker, field_error))
        return luaL_error(L, "%s", field_error.c_str());

    lua_getfield(L, 1, "rumor");
    if (lua_istable(L, -1)) {
        const int rumor = lua_gettop(L);
        if (!read_int_field(L, rumor, "days_before", spec.rumor_days_before, field_error))
            return luaL_error(L, "%s", field_error.c_str());
    } else if (!lua_isnil(L, -1)) {
        return luaL_error(L, "calendar.register: 'rumor' must be a table");
    }
    lua_pop(L, 1);

    std::string invalid;
    if (!validate_holiday(spec, invalid)) return luaL_error(L, "%s", invalid.c_str());

    if (ctx->sink->size() >= kMaxModHolidays) {
        return luaL_error(L, "calendar.register: a town may declare at most %d holidays",
                          static_cast<int>(kMaxModHolidays));
    }
    for (const HolidaySpec& existing : *ctx->sink) {
        if (existing.id == spec.id) return luaL_error(L, "calendar.register: duplicate id '%s'", spec.id.c_str());
    }
    ctx->sink->push_back(std::move(spec));
    return 0;
}

/* calendar.on(event, fn) installs a global the host calls by name. Keeping the
 * indirection in Lua rather than holding a registry reference per hook means
 * the host needs no per-mod callback table, and a mod can equivalently just
 * define the global itself. */
int calendar_on(lua_State* L) {
    const char* event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    static const char* const kHooks[] = { "load", "day_start", "hour", "holiday_begin", "holiday_end" };
    bool known = false;
    for (const char* candidate : kHooks) {
        if (std::strcmp(event, candidate) == 0) { known = true; break; }
    }
    if (!known) return luaL_error(L, "calendar.on: unknown event '%s'", event);
    lua_pushvalue(L, 2);
    lua_setglobal(L, (std::string("on_") + event).c_str());
    return 0;
}

void install_calendar_api(lua_State* L, RegistrationContext* ctx, WorldContext* world) {
    lua_newtable(L);
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, calendar_register, 1);
    lua_setfield(L, -2, "register");
    lua_pushcfunction(L, calendar_on);
    lua_setfield(L, -2, "on");

    static const luaL_Reg kWorldApi[] = {
        { "today", calendar_today },
        { "weather", calendar_weather },
        { "is_active", calendar_is_active },
        { "players_online", calendar_players_online },
        { "set_weather", calendar_set_weather },
        { "announce", calendar_announce },
        { "grant", calendar_grant },
        { "store", calendar_store },
        { "load", calendar_load },
    };
    for (const luaL_Reg& entry : kWorldApi) {
        lua_pushlightuserdata(L, world);
        lua_pushcclosure(L, entry.func, 1);
        lua_setfield(L, -2, entry.name);
    }
    lua_setglobal(L, "calendar");
}

} // namespace

struct ModHost::Mod {
    std::string id;
    lua_State* L = nullptr;
    AllocState alloc;
    /* Both are addressed by light-userdata upvalues in the calendar bindings,
     * so they must live exactly as long as the lua_State does. Holding them by
     * value here is what guarantees that. */
    RegistrationContext registration;
    WorldContext world;
    std::uint64_t random_seed = 0;
    bool quarantined = false;
    int consecutive_errors = 0;
    std::string last_error;
};

ModHost::ModHost() = default;

ModHost::~ModHost() {
    for (auto& mod : mods_) {
        if (mod->L != nullptr) lua_close(mod->L);
    }
}

ModHost::Mod* ModHost::find(const std::string& mod_id) {
    for (auto& mod : mods_) {
        if (mod->id == mod_id) return mod.get();
    }
    return nullptr;
}

const ModHost::Mod* ModHost::find(const std::string& mod_id) const {
    for (const auto& mod : mods_) {
        if (mod->id == mod_id) return mod.get();
    }
    return nullptr;
}

namespace {

/* Builds the mod's global environment from nothing.
 *
 * The dangerous libraries are not merely hidden: liolib, loslib, loadlib and
 * ldblib are not vendored at all (third_party/lua/VENDORING.md), so `io`,
 * `os`, `require` and `debug` have no implementation to reach. What remains is
 * an explicit copy of the safe parts of base/string/table/math.
 *
 * Notably absent from the base whitelist and worth stating: `load`, `loadfile`,
 * `dofile` (would let a mod build code at runtime, defeating the point of
 * shipping auditable text), `collectgarbage` (lets a mod defeat the memory
 * ceiling's accounting), `rawset`/`rawget`/`setmetatable` (metatable games
 * around the sandbox), and `print` (mods report through the host, so their
 * output is attributable and rate-limited rather than raw on stdout). */
void install_sandbox(lua_State* L, std::uint64_t* seed_slot) {
    /* Opened one at a time rather than through luaL_openlibs, which lives in
     * the linit.c we deliberately do not vendor. Naming each library here means
     * adding one is a visible edit to this list, not a side effect of a
     * dependency update. */
    static const luaL_Reg kLibraries[] = {
        { LUA_GNAME, luaopen_base },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_MATHLIBNAME, luaopen_math },
    };
    for (const luaL_Reg& library : kLibraries) {
        luaL_requiref(L, library.name, library.func, 1);
        lua_pop(L, 1);
    }

    lua_newtable(L);                       /* the new _G */
    const int env = lua_gettop(L);

    lua_pushglobaltable(L);
    const int old = lua_gettop(L);
    for (const char* name : kBaseWhitelist) copy_field(L, old, name, env);
    lua_pop(L, 1);

    const auto copy_library = [&](const char* library, const char* const* names, std::size_t count) {
        lua_getglobal(L, library);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
        const int source = lua_gettop(L);
        lua_newtable(L);
        const int dest = lua_gettop(L);
        for (std::size_t i = 0; i < count; ++i) copy_field(L, source, names[i], dest);
        lua_setfield(L, env, library);
        lua_pop(L, 1);
    };
    copy_library("string", kStringWhitelist, sizeof(kStringWhitelist) / sizeof(kStringWhitelist[0]));
    copy_library("table", kTableWhitelist, sizeof(kTableWhitelist) / sizeof(kTableWhitelist[0]));
    copy_library("math", kMathWhitelist, sizeof(kMathWhitelist) / sizeof(kMathWhitelist[0]));

    /* Deterministic math.random, closing over the host-owned seed. */
    lua_getfield(L, env, "math");
    if (lua_istable(L, -1)) {
        lua_pushlightuserdata(L, seed_slot);
        lua_pushcclosure(L, mod_random, 1);
        lua_setfield(L, -2, "random");
    }
    lua_pop(L, 1);

    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");            /* a mod's _G is its own table, not the real one */

    /* Replace the real globals so anything the chunk reaches for resolves here.
     * Lua 5.4 keeps _ENV for the main chunk in the registry's globals slot. */
    lua_pushvalue(L, env);
    lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pop(L, 1);
}

void log_mod_event(const std::string& id, const char* event, const std::string& detail) {
    std::ostringstream line;
    line << "{\"event\":\"mod_" << event << "\",\"mod\":\"" << id << "\"";
    if (!detail.empty()) {
        std::string escaped;
        for (const char c : detail) {
            if (c == '"' || c == '\\') escaped.push_back('\\');
            if (static_cast<unsigned char>(c) < 0x20) { escaped.push_back(' '); continue; }
            escaped.push_back(c);
        }
        line << ",\"detail\":\"" << escaped << "\"";
    }
    line << "}";
    std::cout << line.str() << std::endl;
}

} // namespace

bool ModHost::load_all(const ModRegistry& registry, const ModLimits& limits, std::string& error) {
    limits_ = limits;
    for (const ModManifest& manifest : registry.load_order()) {
        auto mod = std::make_unique<Mod>();
        mod->id = manifest.id;
        mod->alloc.limit = limits_.memory_bytes;
        /* Seeded from the mod's own content hash: reproducible across restarts,
         * and different per mod so two mods do not draw the same sequence. */
        for (std::size_t i = 0; i < 8; ++i) {
            mod->random_seed = (mod->random_seed << 8) | manifest.content_hash[i];
        }

        mod->L = lua_newstate(mod_alloc, &mod->alloc);
        if (mod->L == nullptr) {
            error = "mod '" + manifest.id + "': could not create a Lua state";
            return false;
        }
        install_sandbox(mod->L, &mod->random_seed);
        mod->registration.mod_id = manifest.id;
        mod->registration.sink = &holidays_;
        mod->world.mod_id = manifest.id;
        mod->world.world = world_;
        install_calendar_api(mod->L, &mod->registration, &mod->world);
        const std::size_t holidays_before = holidays_.size();

        const std::filesystem::path entry = manifest.root / manifest.entry;
        std::ifstream input(entry, std::ios::binary);
        if (!input) {
            error = "mod '" + manifest.id + "': cannot open " + entry.string();
            return false;
        }
        const std::string chunk((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

        /* "t" forbids precompiled bytecode. A mod ships auditable source, and
         * the bytecode loader is not hardened against hostile input. */
        const std::string name = "@" + manifest.id + "/" + manifest.entry;
        lua_sethook(mod->L, budget_hook, LUA_MASKCOUNT, limits_.instructions_load);
        int status = luaL_loadbufferx(mod->L, chunk.data(), chunk.size(), name.c_str(), "t");
        if (status == LUA_OK) status = lua_pcall(mod->L, 0, 0, 0);
        lua_sethook(mod->L, nullptr, 0, 0);

        if (status != LUA_OK) {
            const char* message = lua_tostring(mod->L, -1);
            mod->last_error = (message != nullptr) ? message : "unknown error";
            lua_pop(mod->L, 1);
            mod->quarantined = true;
            ++metrics_.hook_errors;
            ++metrics_.quarantined;
            /* A chunk that errored part-way may have registered holidays before
             * failing. Half a mod's calendar is worse than none of it. */
            holidays_.resize(holidays_before);
            /* Deliberately not fatal: the other mods still load, and the town
             * still starts. An operator sees this on stdout. */
            log_mod_event(mod->id, "load_failed", mod->last_error);
        } else {
            log_mod_event(mod->id, "loaded", manifest.version);
        }
        /* Registration is load-time only. Closing the sink here means a mod
         * calling calendar.register from a hook gets a clear error rather than
         * mutating a calendar that has already been resolved and replicated. */
        mod->registration.sink = nullptr;
        metrics_.memory_denials += mod->alloc.denials;
        mods_.push_back(std::move(mod));
    }
    return true;
}

bool ModHost::call_hook(const std::string& mod_id, const char* hook) {
    Mod* mod = find(mod_id);
    if (mod == nullptr || mod->quarantined || mod->L == nullptr) return false;

    lua_getglobal(mod->L, hook);
    if (!lua_isfunction(mod->L, -1)) {
        lua_pop(mod->L, 1);
        return false;
    }

    ++metrics_.hooks_called;
    const int budget = (std::strcmp(hook, "on_day_start") == 0) ? limits_.instructions_day
                                                               : limits_.instructions_hour;
    lua_sethook(mod->L, budget_hook, LUA_MASKCOUNT, budget);
    const int status = lua_pcall(mod->L, 0, 0, 0);
    lua_sethook(mod->L, nullptr, 0, 0);

    if (status != LUA_OK) {
        const char* message = lua_tostring(mod->L, -1);
        mod->last_error = (message != nullptr) ? message : "unknown error";
        lua_pop(mod->L, 1);
        ++metrics_.hook_errors;
        ++mod->consecutive_errors;
        log_mod_event(mod->id, "hook_error", std::string(hook) + ": " + mod->last_error);
        if (mod->consecutive_errors >= limits_.errors_to_quarantine) {
            mod->quarantined = true;
            ++metrics_.quarantined;
            log_mod_event(mod->id, "quarantined", "too many consecutive errors");
        }
        metrics_.memory_denials += mod->alloc.denials;
        mod->alloc.denials = 0;
        return false;
    }

    mod->consecutive_errors = 0;
    metrics_.memory_denials += mod->alloc.denials;
    mod->alloc.denials = 0;
    return true;
}

void ModHost::call_holiday_hook(const std::string& holiday_id, bool begin) {
    const std::size_t dot = holiday_id.find('.');
    if (dot == std::string::npos) return;
    const std::string owner = holiday_id.substr(0, dot);
    const std::string local = holiday_id.substr(dot + 1);

    Mod* mod = find(owner);
    if (mod == nullptr || mod->quarantined || mod->L == nullptr) return;

    lua_getglobal(mod->L, begin ? "on_holiday_begin" : "on_holiday_end");
    if (!lua_isfunction(mod->L, -1)) {
        lua_pop(mod->L, 1);
        return;
    }
    /* The mod sees its own unqualified id -- the namespace is the host's
     * bookkeeping, not something a mod author should have to write out. */
    lua_pushlstring(mod->L, local.data(), local.size());

    ++metrics_.hooks_called;
    lua_sethook(mod->L, budget_hook, LUA_MASKCOUNT, limits_.instructions_hour);
    const int status = lua_pcall(mod->L, 1, 0, 0);
    lua_sethook(mod->L, nullptr, 0, 0);

    if (status != LUA_OK) {
        const char* message = lua_tostring(mod->L, -1);
        mod->last_error = (message != nullptr) ? message : "unknown error";
        lua_pop(mod->L, 1);
        ++metrics_.hook_errors;
        ++mod->consecutive_errors;
        log_mod_event(mod->id, "hook_error",
                      std::string(begin ? "on_holiday_begin" : "on_holiday_end") + ": " + mod->last_error);
        if (mod->consecutive_errors >= limits_.errors_to_quarantine) {
            mod->quarantined = true;
            ++metrics_.quarantined;
            log_mod_event(mod->id, "quarantined", "too many consecutive errors");
        }
    } else {
        mod->consecutive_errors = 0;
    }
    metrics_.memory_denials += mod->alloc.denials;
    mod->alloc.denials = 0;
}

void ModHost::drop_quarantined_holidays() {
    std::vector<HolidaySpec> kept;
    kept.reserve(holidays_.size());
    for (const HolidaySpec& spec : holidays_) {
        const std::size_t dot = spec.id.find('.');
        const std::string owner = (dot == std::string::npos) ? spec.id : spec.id.substr(0, dot);
        if (!quarantined(owner)) kept.push_back(spec);
    }
    holidays_.swap(kept);
}

bool ModHost::quarantined(const std::string& mod_id) const {
    const Mod* mod = find(mod_id);
    return mod == nullptr || mod->quarantined;
}

std::vector<std::string> ModHost::loaded_ids() const {
    std::vector<std::string> ids;
    ids.reserve(mods_.size());
    for (const auto& mod : mods_) ids.push_back(mod->id);
    return ids;
}

std::string ModHost::last_error(const std::string& mod_id) const {
    const Mod* mod = find(mod_id);
    return mod == nullptr ? std::string() : mod->last_error;
}

} // namespace acserver
