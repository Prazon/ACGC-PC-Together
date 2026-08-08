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
    bool load_all(const ModRegistry& registry, const ModLimits& limits, std::string& error);

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
    ModLimits limits_;
    ModMetrics metrics_;

    Mod* find(const std::string& mod_id);
    const Mod* find(const std::string& mod_id) const;
};

} // namespace acserver
