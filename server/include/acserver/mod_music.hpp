#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace acserver {

/* Custom songs (P9).
 *
 * A house's stereo owns a 64-bit `music_box` bitfield of which K.K. songs it
 * holds (acnet::HouseState::music_box, already replicated as of protocol v22).
 * The original game defines 55 minidisks (MINIDISK_NUM in
 * include/m_name_table.h -- the source of truth; duplicated here because
 * server/ must not include game headers).
 *
 * That leaves bits 55..63 free: **nine** custom songs per town, and the cap is
 * the bitfield, not this code. Widening it is a save-format and wire-format
 * change touching HouseState, the GCI mapping and the checkpoint version, so it
 * is deliberately not attempted here -- docs/MODDING_IMPLEMENTATION.md sec 11b.2
 * recommends bundling that with EXTENDED_RESIDENTS_PLAN rather than paying for
 * it twice.
 *
 * Registration is load-time only and slots are handed out in registration
 * order, which is deterministic because mod load order is. A town that installs
 * the same mods gets the same song ids every time, which matters because the id
 * is what ends up in a saved music_box.
 */

constexpr std::uint8_t kVanillaSongCount = 55;   /* MINIDISK_NUM */
constexpr std::uint8_t kMusicBoxBits = 64;
constexpr std::uint8_t kMaxCustomSongs = kMusicBoxBits - kVanillaSongCount;   /* 9 */

struct CustomSong {
    std::string id;         /* namespaced <mod-id>.<id> */
    std::string name_key;   /* key into the mod's string table */
    std::string audio;      /* filename under the mod's content/ directory */
    std::uint8_t slot = 0;  /* bit index into music_box; always >= kVanillaSongCount */
};

class ModMusicRegistry {
public:
    /* Assigns the next free slot. Returns false when the nine are used up, or
     * when the id is already taken -- both are load-time errors naming the mod
     * rather than a song that silently does not exist. */
    bool define(const std::string& id, const std::string& name_key, const std::string& audio,
                std::string& error);

    const std::vector<CustomSong>& songs() const { return songs_; }

    /* Slot for a song id, or -1. */
    int slot_of(const std::string& id) const;

    /* Songs declared by a mod that was later quarantined are dropped, matching
     * how holidays behave: a mod that cannot run should not still own a slot. */
    void drop_by_owner(const std::string& mod_id);

    std::uint8_t remaining() const { return static_cast<std::uint8_t>(kMusicBoxBits - next_slot_); }

private:
    std::vector<CustomSong> songs_;
    /* Monotonic. Derived from songs_.size() it would go backwards after
     * drop_by_owner and hand a later song a slot a stereo already holds; the
     * two never interleave in the real flow, but making the invariant
     * structural is cheaper than relying on that staying true. */
    std::uint8_t next_slot_ = kVanillaSongCount;
};

} // namespace acserver
