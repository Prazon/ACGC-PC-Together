#include "acserver/mod_music.hpp"

#include <algorithm>

namespace acserver {

bool ModMusicRegistry::define(const std::string& id, const std::string& name_key,
                              const std::string& audio, std::string& error) {
    if (id.empty()) { error = "song id is empty"; return false; }
    if (name_key.empty()) { error = "song '" + id + "' has no name key"; return false; }
    if (audio.empty()) { error = "song '" + id + "' has no audio file"; return false; }
    /* The audio field names a file inside the mod's content directory, so a
     * separator or dot-segment is a packaging error at best. */
    if (audio.find('/') != std::string::npos || audio.find('\\') != std::string::npos ||
        audio.find("..") != std::string::npos) {
        error = "song '" + id + "': audio must be a bare filename";
        return false;
    }

    for (const CustomSong& existing : songs_) {
        if (existing.id == id) { error = "duplicate song id '" + id + "'"; return false; }
    }

    if (next_slot_ >= kMusicBoxBits) {
        /* Worth being explicit about why, because nine is a surprising number
         * and the reason is not in this file. */
        error = "a town may define at most " + std::to_string(kMaxCustomSongs) +
                " custom songs: the stereo's music_box is a " + std::to_string(kMusicBoxBits) +
                "-bit field and the original game already uses " +
                std::to_string(kVanillaSongCount) + " of it";
        return false;
    }

    CustomSong song;
    song.id = id;
    song.name_key = name_key;
    song.audio = audio;
    /* Slots are handed out in registration order, which is deterministic
     * because mod load order is. The same mod set therefore yields the same
     * song ids on every start -- which matters, because the id is what a saved
     * music_box holds. */
    song.slot = next_slot_++;
    songs_.push_back(std::move(song));
    return true;
}

int ModMusicRegistry::slot_of(const std::string& id) const {
    for (const CustomSong& song : songs_) {
        if (song.id == id) return song.slot;
    }
    return -1;
}

void ModMusicRegistry::drop_by_owner(const std::string& mod_id) {
    const std::string prefix = mod_id + ".";
    /* Slots are NOT reused. next_slot_ never goes backwards, so a song defined
     * later cannot land on a bit some stereo already holds -- renumbering would
     * silently make every such stereo play something else. A gap is correct. */
    songs_.erase(std::remove_if(songs_.begin(), songs_.end(),
                                [&prefix](const CustomSong& song) {
                                    return song.id.size() >= prefix.size() &&
                                           song.id.compare(0, prefix.size(), prefix) == 0;
                                }),
                 songs_.end());
}

} // namespace acserver
