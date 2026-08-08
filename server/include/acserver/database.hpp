#pragma once

#include "acnet/types.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace acserver {

class DatabaseStore {
public:
    explicit DatabaseStore(std::filesystem::path root);
    ~DatabaseStore();
    DatabaseStore(DatabaseStore&&) noexcept;
    DatabaseStore& operator=(DatabaseStore&&) noexcept;
    DatabaseStore(const DatabaseStore&) = delete;
    DatabaseStore& operator=(const DatabaseStore&) = delete;

    bool initialize(std::string& error);
    bool record_account(acnet::AccountId account, std::int64_t unix_seconds, std::string& error);
    bool record_session(acnet::SessionId session, acnet::AccountId account, bool connected,
                        std::int64_t unix_seconds, std::string& error);
    bool record_transaction(std::uint64_t journal_sequence, acnet::AccountId account,
                            std::uint16_t operation_type, acnet::ResultCode result,
                            std::int64_t unix_seconds, std::string& error);
    bool audit(acnet::AccountId actor, const std::string& action, const std::string& details,
               std::int64_t unix_seconds, std::string& error);
    bool set_banned(acnet::AccountId account, bool banned, std::int64_t unix_seconds, std::string& error);
    bool is_banned(acnet::AccountId account, bool& banned, std::string& error) const;

    /* Per-mod key/value behind calendar.store / calendar.load. Keyed by mod id
     * so one mod cannot read or overwrite another's state. Both are best-effort
     * from a mod's point of view: a false return reaches Lua as a false result
     * rather than an error, because losing a mod's bookkeeping must not take
     * the town down. */
    bool set_mod_state(const std::string& mod_id, const std::string& key, const std::string& value,
                       std::int64_t unix_seconds);
    bool get_mod_state(const std::string& mod_id, const std::string& key, std::string& value) const;

    int schema_version(std::string& error) const;
    std::string journal_mode(std::string& error) const;
    std::filesystem::path path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acserver
