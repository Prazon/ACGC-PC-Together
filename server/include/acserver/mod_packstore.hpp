#pragma once

#include "acnet/messages.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace acserver {

/* Content-addressed store for the files a town serves to clients.
 *
 * Built once at startup, not per connection: the manifest is computed once and
 * serving a chunk is a bounded read from a blob already in memory. With a
 * four-player town the worst case is four clients cold-joining together, which
 * this handles without any per-client state beyond what the transport already
 * keeps.
 *
 * Blobs are keyed by the hash of their bytes, so two mods shipping the same
 * texture store and serve it once, and a client that already has it from
 * another town downloads nothing. */
class ModPackStore {
public:
    /* Scans mods/<id>/content/ under `mods_dir`. A missing directory is success
     * with an empty store -- a town serving no content is the normal case. */
    bool build(const std::filesystem::path& mods_dir, std::string& error);

    /* The manifest to advertise. Empty entries when the store is empty. */
    const acnet::AssetManifest& manifest() const { return manifest_; }

    /* Total chunks a blob occupies, or 0 if unknown. */
    std::uint32_t chunk_count(const std::array<std::uint8_t, 32>& hash) const;

    /* Fills `out` with chunk `index` of `hash`. False if the hash is unknown or
     * the index is past the end -- a client asking for either is a client the
     * server should not be doing work for. */
    bool chunk(const std::array<std::uint8_t, 32>& hash, std::uint32_t index,
               std::vector<std::uint8_t>& out) const;

    std::size_t blob_count() const { return blobs_.size(); }
    std::uint64_t total_bytes() const { return total_bytes_; }

private:
    struct Blob {
        std::vector<std::uint8_t> data;
    };
    struct HashKey {
        std::size_t operator()(const std::array<std::uint8_t, 32>& hash) const {
            std::size_t seed = 0;
            for (std::size_t i = 0; i < sizeof(std::size_t) && i < hash.size(); ++i) {
                seed = (seed << 8) | hash[i];
            }
            return seed;
        }
    };

    std::unordered_map<std::array<std::uint8_t, 32>, Blob, HashKey> blobs_;
    acnet::AssetManifest manifest_;
    std::uint64_t total_bytes_ = 0;
};

} // namespace acserver
