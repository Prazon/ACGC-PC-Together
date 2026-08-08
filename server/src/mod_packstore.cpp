#include "acserver/mod_packstore.hpp"

#include "acnet/crypto.hpp"
#include "acnet/types.hpp"

#include <algorithm>
#include <fstream>

namespace acserver {
namespace {

/* Extension to asset kind. Anything else in a content directory is ignored
 * rather than served: a stray file should not become something a client is
 * asked to download. */
bool kind_of(const std::filesystem::path& path, std::uint16_t& kind) {
    const std::string extension = path.extension().string();
    if (extension == ".pcasset") { kind = 0; return true; }
    if (extension == ".png") { kind = 1; return true; }
    if (extension == ".tex") { kind = 2; return true; }
    if (extension == ".ogg") { kind = 3; return true; }
    return false;
}

} // namespace

bool ModPackStore::build(const std::filesystem::path& mods_dir, std::string& error) {
    blobs_.clear();
    manifest_ = acnet::AssetManifest{};
    total_bytes_ = 0;

    std::error_code ec;
    if (!std::filesystem::is_directory(mods_dir, ec)) {
        /* No mods, or no content. Both are ordinary. */
        manifest_.revision = 1;
        return true;
    }

    /* Sorted so the manifest -- and therefore its digest -- is a pure function
     * of the content, not of directory iteration order. */
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(mods_dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) { error = "cannot read " + mods_dir.string() + ": " + ec.message(); return false; }
        if (!it->is_regular_file(ec) || ec) continue;
        /* Only files under a mod's content/ directory are served. Everything
         * else -- init.lua, mod.toml, overrides/ -- stays private to the town. */
        const std::string generic = it->path().generic_string();
        if (generic.find("/content/") == std::string::npos) continue;
        std::uint16_t ignored_kind = 0;
        if (!kind_of(it->path(), ignored_kind)) continue;
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    for (const std::filesystem::path& path : files) {
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) { error = "cannot size " + path.string(); return false; }
        if (size == 0) continue;
        if (size > acnet::kMaxAssetBlobBytes) {
            error = path.string() + " is larger than the " +
                    std::to_string(acnet::kMaxAssetBlobBytes) + " byte per-asset limit";
            return false;
        }
        if (total_bytes_ + size > acnet::kMaxManifestBytes) {
            error = "town content exceeds the " + std::to_string(acnet::kMaxManifestBytes) +
                    " byte total limit";
            return false;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) { error = "cannot open " + path.string(); return false; }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
            error = "short read on " + path.string();
            return false;
        }

        acnet::AssetManifestEntry entry;
        entry.hash = acnet::sha256(data.data(), data.size());
        entry.size = static_cast<std::uint32_t>(size);
        kind_of(path, entry.kind);
        entry.item_handle = 0;   /* bound by the registry once P6 lands */

        /* Identical bytes from two mods are stored and served once. */
        if (blobs_.find(entry.hash) != blobs_.end()) continue;
        if (manifest_.entries.size() >= acnet::kMaxAssetEntries) {
            error = "town content exceeds " + std::to_string(acnet::kMaxAssetEntries) + " assets";
            return false;
        }

        total_bytes_ += size;
        blobs_.emplace(entry.hash, Blob{std::move(data)});
        manifest_.entries.push_back(entry);
    }

    manifest_.revision = 1;
    /* Digest over the sorted entry hashes, so a client can compare one value
     * rather than the whole list. */
    std::vector<std::uint8_t> digest_input;
    digest_input.reserve(manifest_.entries.size() * 32);
    for (const acnet::AssetManifestEntry& entry : manifest_.entries) {
        digest_input.insert(digest_input.end(), entry.hash.begin(), entry.hash.end());
    }
    manifest_.manifest_digest =
        acnet::sha256(digest_input.empty() ? nullptr : digest_input.data(), digest_input.size());
    return true;
}

std::uint32_t ModPackStore::chunk_count(const std::array<std::uint8_t, 32>& hash) const {
    const auto found = blobs_.find(hash);
    if (found == blobs_.end()) return 0;
    const std::size_t size = found->second.data.size();
    return static_cast<std::uint32_t>((size + acnet::kAssetChunkBytes - 1) / acnet::kAssetChunkBytes);
}

bool ModPackStore::chunk(const std::array<std::uint8_t, 32>& hash, std::uint32_t index,
                         std::vector<std::uint8_t>& out) const {
    const auto found = blobs_.find(hash);
    if (found == blobs_.end()) return false;
    const std::vector<std::uint8_t>& data = found->second.data;
    const std::size_t offset = static_cast<std::size_t>(index) * acnet::kAssetChunkBytes;
    if (offset >= data.size()) return false;
    const std::size_t length = std::min(acnet::kAssetChunkBytes, data.size() - offset);
    out.assign(data.begin() + static_cast<long>(offset),
               data.begin() + static_cast<long>(offset + length));
    return true;
}

} // namespace acserver
