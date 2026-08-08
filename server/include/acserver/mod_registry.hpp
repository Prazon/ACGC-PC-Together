#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace acserver {

/* The framework API version this build implements. A mod declaring anything
 * else is refused at load with a clear message rather than failing later on a
 * missing function. */
constexpr std::uint32_t kModApiVersion = 1;

constexpr std::size_t kModMaxId = 32;
constexpr std::size_t kModMaxCount = 64;
/* A mod is text and data. Anything larger is a packaging mistake, and the cap
 * keeps a bad directory from stalling startup while it is hashed. */
constexpr std::uint64_t kModMaxBytes = 16u * 1024u * 1024u;

struct ModManifest {
    std::string id;          /* [a-z0-9-], unique; namespace prefix for everything the mod declares */
    std::string name;
    std::string version;
    std::string entry = "init.lua";
    std::uint32_t api_version = 0;
    std::vector<std::string> requires_ids;
    std::vector<std::string> conflicts_ids;

    std::filesystem::path root;
    /* SHA-256 over every file in the mod, path-sorted, path bytes included, so
     * a rename changes the hash. */
    std::array<std::uint8_t, 32> content_hash{};
};

class ModRegistry {
public:
    /* Scans `mods_dir` for subdirectories containing mod.toml. A missing
     * directory is success with no mods -- running without mods is the normal
     * case and must not be an error. Any *present* mod that is malformed,
     * duplicated, conflicting, or has an unsatisfied dependency fails the whole
     * scan: a town silently missing one of its mods would resolve a different
     * calendar than its operator intended. */
    bool scan(const std::filesystem::path& mods_dir, std::string& error);

    /* Dependency order, ties broken by id. Never filesystem order -- that
     * varies between machines and would make a load-order bug irreproducible. */
    const std::vector<ModManifest>& load_order() const { return order_; }

    /* SHA-256 over (id, version, content_hash) of every mod, in load order.
     * This is what ServerHello advertises so a client can tell whether it has
     * the town's content. */
    std::array<std::uint8_t, 32> manifest_digest() const;

    bool empty() const { return order_.empty(); }

private:
    std::vector<ModManifest> order_;
};

/* Exposed for testing: parses one mod.toml body. Accepts the small TOML subset
 * the manifest needs -- `key = "string"`, `key = 123`, `key = ["a", "b"]`,
 * comments, blank lines -- and rejects anything else rather than guessing. */
bool parse_mod_manifest(const std::string& text, ModManifest& out, std::string& error);

} // namespace acserver
