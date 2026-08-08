#include "acserver/mod_registry.hpp"

#include "acnet/crypto.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace acserver {
namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool parse_quoted(const std::string& in, std::string& out, std::string& error) {
    if (in.size() < 2 || in.front() != '"' || in.back() != '"') {
        error = "expected a quoted string, got: " + in;
        return false;
    }
    out = in.substr(1, in.size() - 2);
    if (out.find('"') != std::string::npos) {
        error = "embedded quotes are not supported: " + in;
        return false;
    }
    return true;
}

bool parse_string_array(const std::string& in, std::vector<std::string>& out, std::string& error) {
    if (in.size() < 2 || in.front() != '[' || in.back() != ']') {
        error = "expected a [\"...\"] array, got: " + in;
        return false;
    }
    const std::string body = trim(in.substr(1, in.size() - 2));
    out.clear();
    if (body.empty()) return true;
    std::stringstream stream(body);
    std::string item;
    while (std::getline(stream, item, ',')) {
        std::string value;
        if (!parse_quoted(trim(item), value, error)) return false;
        out.push_back(value);
    }
    return true;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > kModMaxId) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return id.front() != '-' && id.back() != '-';
}

/* Rejects anything that could escape the mod directory. `entry` names a file
 * inside the mod, so a separator or a dot-segment is a packaging error at best
 * and a traversal attempt at worst. */
bool valid_entry(const std::string& entry) {
    if (entry.empty() || entry.size() > 64) return false;
    if (entry.find('/') != std::string::npos || entry.find('\\') != std::string::npos) return false;
    if (entry.find("..") != std::string::npos) return false;
    return entry.size() > 4 && entry.compare(entry.size() - 4, 4, ".lua") == 0;
}

} // namespace

bool parse_mod_manifest(const std::string& text, ModManifest& out, std::string& error) {
    out = ModManifest{};
    bool saw_api_version = false;
    std::istringstream stream(text);
    std::string line;
    int line_no = 0;

    while (std::getline(stream, line)) {
        ++line_no;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        line = trim(line);
        if (line.empty()) continue;

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            error = "line " + std::to_string(line_no) + ": expected key = value";
            return false;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        std::string detail;

        if (key == "id") {
            if (!parse_quoted(value, out.id, detail)) { error = "id: " + detail; return false; }
        } else if (key == "name") {
            if (!parse_quoted(value, out.name, detail)) { error = "name: " + detail; return false; }
        } else if (key == "version") {
            if (!parse_quoted(value, out.version, detail)) { error = "version: " + detail; return false; }
        } else if (key == "entry") {
            if (!parse_quoted(value, out.entry, detail)) { error = "entry: " + detail; return false; }
        } else if (key == "api_version") {
            try {
                const unsigned long parsed = std::stoul(value);
                if (parsed > 0xFFFFFFFFul) throw std::out_of_range("api_version");
                out.api_version = static_cast<std::uint32_t>(parsed);
                saw_api_version = true;
            } catch (const std::exception&) {
                error = "api_version: expected an integer, got: " + value;
                return false;
            }
        } else if (key == "requires") {
            if (!parse_string_array(value, out.requires_ids, detail)) { error = "requires: " + detail; return false; }
        } else if (key == "conflicts") {
            if (!parse_string_array(value, out.conflicts_ids, detail)) { error = "conflicts: " + detail; return false; }
        } else if (key == "authors") {
            std::vector<std::string> ignored;
            if (!parse_string_array(value, ignored, detail)) { error = "authors: " + detail; return false; }
        } else {
            error = "line " + std::to_string(line_no) + ": unknown key '" + key + "'";
            return false;
        }
    }

    if (!valid_id(out.id)) {
        error = "id must be 1-" + std::to_string(kModMaxId) +
                " characters of [a-z0-9-] and may not start or end with '-'";
        return false;
    }
    if (out.version.empty()) { error = "version is required"; return false; }
    if (!saw_api_version) { error = "api_version is required"; return false; }
    if (out.api_version != kModApiVersion) {
        error = "targets api_version " + std::to_string(out.api_version) + "; this build implements " +
                std::to_string(kModApiVersion);
        return false;
    }
    if (!valid_entry(out.entry)) {
        error = "entry must be a bare .lua filename inside the mod directory, got: " + out.entry;
        return false;
    }
    if (out.name.empty()) out.name = out.id;
    for (const std::string& dep : out.requires_ids) {
        if (!valid_id(dep)) { error = "requires: '" + dep + "' is not a valid mod id"; return false; }
    }
    for (const std::string& other : out.conflicts_ids) {
        if (!valid_id(other)) { error = "conflicts: '" + other + "' is not a valid mod id"; return false; }
    }
    return true;
}

namespace {

/* Hashes every regular file under `root`, path-sorted, mixing the relative path
 * in so that renaming a file changes the mod's identity. */
bool hash_directory(const std::filesystem::path& root,
                    std::array<std::uint8_t, 32>& digest,
                    std::string& error) {
    std::vector<std::filesystem::path> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) { error = "cannot read " + root.string() + ": " + ec.message(); return false; }
        if (!it->is_regular_file(ec) || ec) continue;
        const std::uintmax_t size = std::filesystem::file_size(it->path(), ec);
        if (ec) { error = "cannot size " + it->path().string(); return false; }
        total += static_cast<std::uint64_t>(size);
        if (total > kModMaxBytes) {
            error = "mod exceeds " + std::to_string(kModMaxBytes) + " bytes";
            return false;
        }
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    std::vector<std::uint8_t> blob;
    for (const std::filesystem::path& path : files) {
        const std::string relative = std::filesystem::relative(path, root).generic_string();
        blob.insert(blob.end(), relative.begin(), relative.end());
        blob.push_back(0);
        std::ifstream input(path, std::ios::binary);
        if (!input) { error = "cannot open " + path.string(); return false; }
        std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        blob.insert(blob.end(), contents.begin(), contents.end());
        blob.push_back(0);
    }
    digest = acnet::sha256(blob.empty() ? nullptr : blob.data(), blob.size());
    return true;
}

} // namespace

bool ModRegistry::scan(const std::filesystem::path& mods_dir, std::string& error) {
    order_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(mods_dir, ec)) return true;   /* no mods is normal */

    std::vector<ModManifest> found;
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator it(mods_dir, ec), end; it != end; it.increment(ec)) {
        if (ec) { error = "cannot read " + mods_dir.string() + ": " + ec.message(); return false; }
        if (it->is_directory(ec) && !ec) candidates.push_back(it->path());
    }
    std::sort(candidates.begin(), candidates.end());

    for (const std::filesystem::path& dir : candidates) {
        const std::filesystem::path manifest_path = dir / "mod.toml";
        if (!std::filesystem::exists(manifest_path, ec)) continue;
        std::ifstream input(manifest_path);
        if (!input) { error = "cannot open " + manifest_path.string(); return false; }
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

        ModManifest manifest;
        std::string detail;
        if (!parse_mod_manifest(text, manifest, detail)) {
            error = manifest_path.string() + ": " + detail;
            return false;
        }
        if (manifest.id != dir.filename().string()) {
            error = manifest_path.string() + ": id '" + manifest.id + "' does not match directory '" +
                    dir.filename().string() + "'";
            return false;
        }
        if (!std::filesystem::exists(dir / manifest.entry, ec)) {
            error = manifest_path.string() + ": entry '" + manifest.entry + "' does not exist";
            return false;
        }
        manifest.root = dir;
        if (!hash_directory(dir, manifest.content_hash, detail)) {
            error = dir.string() + ": " + detail;
            return false;
        }
        found.push_back(std::move(manifest));
        if (found.size() > kModMaxCount) {
            error = "more than " + std::to_string(kModMaxCount) + " mods installed";
            return false;
        }
    }

    std::map<std::string, const ModManifest*> by_id;
    for (const ModManifest& manifest : found) {
        if (!by_id.emplace(manifest.id, &manifest).second) {
            error = "duplicate mod id '" + manifest.id + "'";
            return false;
        }
    }
    for (const ModManifest& manifest : found) {
        for (const std::string& dep : manifest.requires_ids) {
            if (by_id.find(dep) == by_id.end()) {
                error = "mod '" + manifest.id + "' requires '" + dep + "', which is not installed";
                return false;
            }
        }
        for (const std::string& other : manifest.conflicts_ids) {
            if (by_id.find(other) != by_id.end()) {
                error = "mod '" + manifest.id + "' conflicts with installed mod '" + other + "'";
                return false;
            }
        }
    }

    /* Kahn's algorithm over `requires`, always taking the lexicographically
     * smallest ready id, so the order is a pure function of the mod set. */
    std::map<std::string, std::set<std::string>> pending;
    std::map<std::string, std::set<std::string>> dependents;
    for (const ModManifest& manifest : found) {
        pending[manifest.id] = std::set<std::string>(manifest.requires_ids.begin(), manifest.requires_ids.end());
        for (const std::string& dep : manifest.requires_ids) dependents[dep].insert(manifest.id);
    }
    std::set<std::string> ready;
    for (const auto& entry : pending) {
        if (entry.second.empty()) ready.insert(entry.first);
    }
    while (!ready.empty()) {
        const std::string id = *ready.begin();
        ready.erase(ready.begin());
        order_.push_back(*by_id[id]);
        pending.erase(id);
        for (const std::string& dependent : dependents[id]) {
            auto it = pending.find(dependent);
            if (it == pending.end()) continue;
            it->second.erase(id);
            if (it->second.empty()) ready.insert(dependent);
        }
    }
    if (order_.size() != found.size()) {
        std::string cycle;
        for (const auto& entry : pending) cycle += (cycle.empty() ? "" : ", ") + entry.first;
        error = "dependency cycle among mods: " + cycle;
        order_.clear();
        return false;
    }
    return true;
}

std::array<std::uint8_t, 32> ModRegistry::manifest_digest() const {
    std::vector<std::uint8_t> blob;
    for (const ModManifest& manifest : order_) {
        blob.insert(blob.end(), manifest.id.begin(), manifest.id.end());
        blob.push_back(0);
        blob.insert(blob.end(), manifest.version.begin(), manifest.version.end());
        blob.push_back(0);
        blob.insert(blob.end(), manifest.content_hash.begin(), manifest.content_hash.end());
    }
    return acnet::sha256(blob.empty() ? nullptr : blob.data(), blob.size());
}

} // namespace acserver
