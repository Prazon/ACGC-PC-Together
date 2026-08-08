#include "acserver/database.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace acserver {
namespace {

struct sqlite3;
constexpr int kSqliteOk = 0;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteOpenFullMutex = 0x00010000;

std::string quote(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('\'');
    for (char character : value) {
        result.push_back(character);
        if (character == '\'') result.push_back('\'');
    }
    result.push_back('\'');
    return result;
}

const std::vector<const char*>& migrations() {
    static const std::vector<const char*> values{
        R"SQL(
CREATE TABLE IF NOT EXISTS accounts(account_id INTEGER PRIMARY KEY,created_at INTEGER NOT NULL,last_seen_at INTEGER NOT NULL,banned INTEGER NOT NULL DEFAULT 0 CHECK(banned IN (0,1)));
CREATE TABLE IF NOT EXISTS sessions(session_id TEXT PRIMARY KEY,account_id INTEGER NOT NULL REFERENCES accounts(account_id),connected INTEGER NOT NULL CHECK(connected IN (0,1)),updated_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS transactions(journal_sequence INTEGER PRIMARY KEY,account_id INTEGER NOT NULL,operation_type INTEGER NOT NULL,result_code INTEGER NOT NULL,committed_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS audit_log(id INTEGER PRIMARY KEY AUTOINCREMENT,actor_account_id INTEGER NOT NULL DEFAULT 0,action TEXT NOT NULL,details TEXT NOT NULL,created_at INTEGER NOT NULL);
)SQL",
        R"SQL(
CREATE TABLE IF NOT EXISTS mail_metadata(mail_id INTEGER PRIMARY KEY,sender_account_id INTEGER NOT NULL,recipient_account_id INTEGER NOT NULL,delivered_at INTEGER,revision INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS mail_recipient_idx ON mail_metadata(recipient_account_id,delivered_at);
)SQL",
        R"SQL(
CREATE TABLE IF NOT EXISTS housing_metadata(house_id INTEGER PRIMARY KEY,owner_account_id INTEGER NOT NULL UNIQUE,original_slot INTEGER NOT NULL CHECK(original_slot BETWEEN 0 AND 3),zone_id INTEGER NOT NULL,revision INTEGER NOT NULL);
)SQL",
        R"SQL(
CREATE TABLE IF NOT EXISTS mod_state(mod_id TEXT NOT NULL,key TEXT NOT NULL,value TEXT NOT NULL,updated_at INTEGER NOT NULL,PRIMARY KEY(mod_id,key)) WITHOUT ROWID;
)SQL",
    };
    return values;
}

} // namespace

struct DatabaseStore::Impl {
    using Open = int (*)(const char*, sqlite3**, int, const char*);
    using Close = int (*)(sqlite3*);
    using ExecCallback = int (*)(void*, int, char**, char**);
    using Exec = int (*)(sqlite3*, const char*, ExecCallback, void*, char**);
    using Free = void (*)(void*);
    using ErrorMessage = const char* (*)(sqlite3*);
    using BusyTimeout = int (*)(sqlite3*, int);

    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}
    std::filesystem::path root;
    sqlite3* database = nullptr;
#ifdef _WIN32
    HMODULE library = nullptr;
#else
    void* library = nullptr;
#endif
    Open open = nullptr;
    Close close = nullptr;
    Exec exec_function = nullptr;
    Free free_function = nullptr;
    ErrorMessage error_message = nullptr;
    BusyTimeout busy_timeout = nullptr;

    template <typename T>
    bool symbol(const char* name, T& output) {
#ifdef _WIN32
        FARPROC address = GetProcAddress(library, name);
#else
        void* address = dlsym(library, name);
#endif
        static_assert(sizeof(output) == sizeof(address), "dynamic function pointer size mismatch");
        std::memcpy(&output, &address, sizeof(output));
        return output != nullptr;
    }

    bool load(std::string& error) {
#ifdef _WIN32
        library = LoadLibraryA("sqlite3.dll");
        if (library == nullptr) library = LoadLibraryA("libsqlite3-0.dll");
#else
        library = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) library = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
#endif
        if (library == nullptr) { error = "SQLite 3 runtime library is not installed"; return false; }
        if (!symbol("sqlite3_open_v2", open) || !symbol("sqlite3_close", close) ||
            !symbol("sqlite3_exec", exec_function) || !symbol("sqlite3_free", free_function) ||
            !symbol("sqlite3_errmsg", error_message) || !symbol("sqlite3_busy_timeout", busy_timeout)) {
            error = "SQLite 3 runtime is missing required API symbols";
            return false;
        }
        return true;
    }

    bool execute(const std::string& sql, std::string& error) const {
        char* message = nullptr;
        const int result = exec_function(database, sql.c_str(), nullptr, nullptr, &message);
        if (result != kSqliteOk) {
            error = message != nullptr ? message : error_message(database);
            if (message != nullptr) free_function(message);
            return false;
        }
        return true;
    }

    std::string query(const std::string& sql, std::string& error) const {
        std::string value;
        const auto callback = [](void* context, int columns, char** values, char**) -> int {
            if (columns > 0 && values != nullptr && values[0] != nullptr)
                *static_cast<std::string*>(context) = values[0];
            return 0;
        };
        char* message = nullptr;
        const int result = exec_function(database, sql.c_str(), callback, &value, &message);
        if (result != kSqliteOk) {
            error = message != nullptr ? message : error_message(database);
            if (message != nullptr) free_function(message);
            return {};
        }
        return value;
    }

    void shutdown() {
        if (database != nullptr && close != nullptr) close(database);
        database = nullptr;
#ifdef _WIN32
        if (library != nullptr) FreeLibrary(library);
#else
        if (library != nullptr) dlclose(library);
#endif
        library = nullptr;
    }
};

DatabaseStore::DatabaseStore(std::filesystem::path root) : impl_(std::make_unique<Impl>(std::move(root))) {}
DatabaseStore::~DatabaseStore() { if (impl_) impl_->shutdown(); }
DatabaseStore::DatabaseStore(DatabaseStore&&) noexcept = default;
DatabaseStore& DatabaseStore::operator=(DatabaseStore&&) noexcept = default;

bool DatabaseStore::initialize(std::string& error) {
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(impl_->root / "snapshots", ec);
    if (ec) { error = "failed to create database directory: " + ec.message(); return false; }
    if (!impl_->load(error)) return false;
    const std::string filename = (impl_->root / "town.db").string();
    if (impl_->open(filename.c_str(), &impl_->database,
                    kSqliteOpenReadWrite | kSqliteOpenCreate | kSqliteOpenFullMutex, nullptr) != kSqliteOk) {
        error = "failed to open town.db";
        return false;
    }
    impl_->busy_timeout(impl_->database, 5000);
    if (!impl_->execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;", error)) return false;
    int version = schema_version(error);
    if (!error.empty() || version < 0 || version > static_cast<int>(migrations().size())) {
        if (error.empty()) error = "unsupported database schema version";
        return false;
    }
    if (version > 0 && version < static_cast<int>(migrations().size())) {
        const std::filesystem::path backup = impl_->root / "snapshots" /
            ("town.db.before-v" + std::to_string(migrations().size()) + ".sqlite");
        std::filesystem::remove(backup, ec);
        if (!impl_->execute("VACUUM INTO " + quote(backup.string()) + ";", error)) return false;
    }
    for (std::size_t index = static_cast<std::size_t>(version); index < migrations().size(); ++index) {
        const std::string sql = "BEGIN IMMEDIATE;" + std::string(migrations()[index]) +
                                "PRAGMA user_version=" + std::to_string(index + 1) + ";COMMIT;";
        if (!impl_->execute(sql, error)) {
            std::string ignored;
            impl_->execute("ROLLBACK;", ignored);
            return false;
        }
    }
    return true;
}

bool DatabaseStore::record_account(acnet::AccountId account, std::int64_t now, std::string& error) {
    return account != 0 && impl_->execute(
        "INSERT INTO accounts(account_id,created_at,last_seen_at) VALUES(" + std::to_string(account) + "," +
        std::to_string(now) + "," + std::to_string(now) + ") ON CONFLICT(account_id) DO UPDATE SET last_seen_at=excluded.last_seen_at;", error);
}

bool DatabaseStore::record_session(acnet::SessionId session, acnet::AccountId account, bool connected,
                                   std::int64_t now, std::string& error) {
    return session != 0 && account != 0 && impl_->execute(
        "INSERT INTO sessions(session_id,account_id,connected,updated_at) VALUES(" + quote(std::to_string(session)) +
        "," + std::to_string(account) + "," + (connected ? "1" : "0") + "," + std::to_string(now) +
        ") ON CONFLICT(session_id) DO UPDATE SET connected=excluded.connected,updated_at=excluded.updated_at;", error);
}

bool DatabaseStore::record_transaction(std::uint64_t sequence, acnet::AccountId account,
                                       std::uint16_t type, acnet::ResultCode result,
                                       std::int64_t now, std::string& error) {
    /* Say why. This returned a bare false, so an operator command that reached
     * it with no account -- a town-wide change has none -- failed with an empty
     * message and no indication of what was wrong. */
    if (sequence == 0 || account == 0) {
        error = "a transaction row needs a journal sequence and an account";
        return false;
    }
    return impl_->execute("INSERT OR REPLACE INTO transactions(journal_sequence,account_id,operation_type,result_code,committed_at) VALUES(" +
                          std::to_string(sequence) + "," + std::to_string(account) + "," + std::to_string(type) +
                          "," + std::to_string(static_cast<std::uint16_t>(result)) + "," + std::to_string(now) + ");", error);
}

bool DatabaseStore::audit(acnet::AccountId actor, const std::string& action, const std::string& details,
                          std::int64_t now, std::string& error) {
    if (action.empty() || action.size() > 128 || details.size() > 4096) return false;
    return impl_->execute("INSERT INTO audit_log(actor_account_id,action,details,created_at) VALUES(" +
                          std::to_string(actor) + "," + quote(action) + "," + quote(details) + "," +
                          std::to_string(now) + ");", error);
}

bool DatabaseStore::set_banned(acnet::AccountId account, bool banned, std::int64_t now, std::string& error) {
    if (account == 0 || !record_account(account, now, error)) return false;
    return impl_->execute("UPDATE accounts SET banned=" + std::string(banned ? "1" : "0") +
                          " WHERE account_id=" + std::to_string(account) + ";", error);
}

namespace {

/* SQLite string literal escaping. Mod ids are already restricted to [a-z0-9-],
 * but keys and values come from Lua, so they are escaped rather than trusted. */
std::string sql_quote(const std::string& value) {
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') out.push_back('\'');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

} // namespace

bool DatabaseStore::set_mod_state(const std::string& mod_id, const std::string& key,
                                  const std::string& value, std::int64_t now) {
    if (mod_id.empty() || key.empty()) return false;
    std::string error;
    return impl_->execute("INSERT INTO mod_state(mod_id,key,value,updated_at) VALUES(" +
                              sql_quote(mod_id) + "," + sql_quote(key) + "," + sql_quote(value) + "," +
                              std::to_string(now) +
                              ") ON CONFLICT(mod_id,key) DO UPDATE SET value=excluded.value,"
                              "updated_at=excluded.updated_at;",
                          error);
}

bool DatabaseStore::get_mod_state(const std::string& mod_id, const std::string& key,
                                  std::string& value) const {
    if (mod_id.empty() || key.empty()) return false;
    std::string error;
    const std::string found = impl_->query("SELECT value FROM mod_state WHERE mod_id=" + sql_quote(mod_id) +
                                               " AND key=" + sql_quote(key) + " LIMIT 1;",
                                           error);
    if (!error.empty() || found.empty()) return false;
    value = found;
    return true;
}

bool DatabaseStore::is_banned(acnet::AccountId account, bool& banned, std::string& error) const {
    banned = false;
    if (account == 0) return true;
    const std::string value = impl_->query("SELECT banned FROM accounts WHERE account_id=" +
                                           std::to_string(account) + " LIMIT 1;", error);
    if (!error.empty()) return false;
    banned = value == "1";
    return true;
}

int DatabaseStore::schema_version(std::string& error) const {
    error.clear();
    const std::string value = impl_->query("PRAGMA user_version;", error);
    if (!error.empty() || value.empty()) return -1;
    try { return std::stoi(value); } catch (...) { error = "invalid schema version"; return -1; }
}

std::string DatabaseStore::journal_mode(std::string& error) const {
    error.clear();
    std::string value = impl_->query("PRAGMA journal_mode;", error);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::filesystem::path DatabaseStore::path() const { return impl_->root / "town.db"; }

} // namespace acserver
