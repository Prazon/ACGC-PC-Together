#pragma once

#include "acserver/mod_calendar.hpp"
#include "acserver/mod_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acserver {

/* Resource ceilings, per mod, per call. Defaults are generous for the work a
 * calendar mod actually does (a few hundred table operations) and tight enough
 * that a runaway loop is caught within a tick. */
struct ModLimits {
    std::size_t memory_bytes = 4u * 1024u * 1024u;
    /* Instruction budgets. `on_load` gets more because registration runs once. */
    int instructions_load = 1000000;
    int instructions_day = 200000;
    int instructions_hour = 20000;
    /* Consecutive failures of the same hook before the mod is disabled for the
     * rest of the process lifetime. */
    int errors_to_quarantine = 3;
};

/* What a mod is allowed to see and change, as an interface rather than a
 * dependency on TownRuntime.
 *
 * Two reasons it is shaped this way. mod_host.cpp stays free of the runtime
 * header, which keeps the Lua layer buildable and testable on its own; and the
 * set of things a mod can do is enumerated in one place, so widening a mod's
 * reach is a visible edit here rather than an accident somewhere in the
 * bindings.
 *
 * Every mutating call routes through the same authority a player transaction
 * uses. A mod is trusted to *ask*, exactly like a client is -- it never commits
 * anything itself. */
class ModWorld {
public:
    virtual ~ModWorld() = default;

    /* Authoritative town time. There is no other clock a mod can read: `os` is
     * absent from the sandbox, so this is the only source of "now". */
    virtual acnet::TownDate today() const = 0;
    /* 0 clear, 1 cloudy, 2 rain, 3 snow -- acnet::Weather's ordering. */
    virtual int weather() const = 0;
    virtual std::vector<std::uint64_t> players_online() const = 0;
    virtual bool holiday_active(const std::string& holiday_id) const = 0;

    /* Effects. Each returns false if the runtime refused; a mod sees the
     * refusal rather than a silent no-op. */
    virtual bool set_weather(int kind) = 0;
    virtual bool grant_item(std::uint64_t account, std::uint16_t item) = 0;
    virtual void announce(const std::string& mod_id, const std::string& string_key) = 0;

    /* Per-mod key/value that survives a restart. Values are small scalars
     * rendered as text; anything larger belongs in the mod's own files. */
    virtual bool store(const std::string& mod_id, const std::string& key, const std::string& value) = 0;
    virtual bool load(const std::string& mod_id, const std::string& key, std::string& value) const = 0;
};

struct ModMetrics {
    std::uint64_t hooks_called = 0;
    std::uint64_t hook_errors = 0;
    std::uint64_t quarantined = 0;
    std::uint64_t memory_denials = 0;
};

/* Owns one sandboxed Lua state per mod.
 *
 * Every entry point is noexcept-in-practice: a mod that errors, allocates past
 * its ceiling, or loops forever fails *that call* and nothing else. A town must
 * keep serving with a broken mod installed -- an operator's holiday mod is
 * never worth dropping their players for.
 *
 * There is no hot reload. Mods load once, before the first client connects;
 * changing the set mid-session would desync the calendar clients already hold
 * (docs/netcode/MODDING_PLAN.md sec 4). Reload is a restart.
 */
class ModHost {
public:
    ModHost();
    ~ModHost();
    ModHost(const ModHost&) = delete;
    ModHost& operator=(const ModHost&) = delete;

    /* Creates a state per mod, installs the sandbox, and runs each entry file.
     * A mod whose chunk fails to load or errors during `on_load` is quarantined
     * and the rest still load: one bad mod must not deny the others.
     * Returns false only for an error that makes the whole set unusable. */
    /* Installed before load_all so a mod can query during registration. May be
     * null in tests that only exercise registration; the bindings then report
     * "the town is not available yet" rather than crashing. */
    void set_world(ModWorld* world) { world_ = world; }

    bool load_all(const ModRegistry& registry, const ModLimits& limits, std::string& error);

    /* Calls `on_holiday_begin` / `on_holiday_end` on the mod that owns the
     * holiday, passing the un-namespaced id. Silently does nothing if the owner
     * is quarantined or defines no such hook. */
    void call_holiday_hook(const std::string& holiday_id, bool begin);

    /* Calls a global function by name if the mod defines one. Returns false if
     * the mod is quarantined, has no such function, or the call failed --
     * callers treat all three the same way, which is why they are not
     * distinguished. */
    bool call_hook(const std::string& mod_id, const char* hook);

    bool quarantined(const std::string& mod_id) const;
    std::vector<std::string> loaded_ids() const;
    const ModMetrics& metrics() const { return metrics_; }

    /* Every holiday declared by every loaded mod, in load order, with ids
     * namespaced <mod-id>.<id>. Registration happens during load_all: a mod
     * calls calendar.register() at the top level of its chunk, so by the time
     * load_all returns the set is complete and does not change again for the
     * process lifetime. That is what lets the resolved calendar be computed
     * once per town day and replicated as plain data. */
    const std::vector<HolidaySpec>& holidays() const { return holidays_; }

    /* Holidays declared by a mod that was later quarantined are dropped: a mod
     * that cannot run its hooks should not still be shaping the calendar. */
    void drop_quarantined_holidays();

    /* Diagnostics for tests and the operator console: the last error a mod
     * produced, empty if it has not failed. */
    std::string last_error(const std::string& mod_id) const;

private:
    struct Mod;
    std::vector<std::unique_ptr<Mod>> mods_;
    std::vector<HolidaySpec> holidays_;
    ModWorld* world_ = nullptr;
    ModLimits limits_;
    ModMetrics metrics_;

    Mod* find(const std::string& mod_id);
    const Mod* find(const std::string& mod_id) const;
};

} // namespace acserver
