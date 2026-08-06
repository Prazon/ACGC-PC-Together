#pragma once

#include "acnet/types.hpp"

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace acserver {

enum class ClockMode : std::uint8_t {
    Realtime,
    Fixed,
    Scaled,
};

enum class Weather : std::uint8_t {
    Clear,
    Cloudy,
    Rain,
    Snow,
};

struct ClockConfig {
    std::string timezone = "UTC";
    std::int32_t utc_offset_minutes = 0;
    ClockMode mode = ClockMode::Realtime;
    double scale = 1.0;
    bool allow_time_travel = false;
    // Slaves town time to the host system clock (plus the timezone offset) on
    // every advance, overriding the mode and any starting time. Realtime mode
    // without a starting time already behaves this way; the flag additionally
    // covers seeded, scaled, fixed, and admin-adjusted clocks.
    bool sync_to_system_clock = false;
    // Local civil time encoded on a UTC-like timeline. Used only when a town
    // has no persisted clock state; -1 selects the current configured time.
    std::int64_t starting_town_unix_seconds = -1;
};

struct ClockState {
    std::int64_t town_unix_seconds = 0;
    std::int64_t last_wall_unix_seconds = 0;
    std::int64_t day_number = 0;
    Weather weather = Weather::Clear;
    std::uint8_t weather_intensity = 0;
    acnet::Revision revision = 1;
};

struct ScheduledJob {
    std::string name;
    std::int64_t next_due = 0;
    std::int64_t interval_seconds = 0;
    std::size_t maximum_catchups = 64;
    std::uint64_t run_count = 0;
};

using JobCallback = std::function<bool(const ScheduledJob&, std::int64_t due_time)>;

class TownClock {
public:
    explicit TownClock(ClockConfig config = {}, std::uint64_t random_seed = 0);

    bool initialize(std::int64_t wall_unix_seconds);
    bool restore(const ClockState& state);
    bool add_job(const ScheduledJob& job, JobCallback callback);
    bool advance(std::int64_t wall_unix_seconds, bool town_empty);
    bool set_time(std::int64_t town_unix_seconds, std::int64_t wall_unix_seconds, bool players_online);
    bool set_weather(Weather weather, std::uint8_t intensity);

    const ClockState& state() const { return state_; }
    const ScheduledJob* job(const std::string& name) const;
    const ClockConfig& config() const { return config_; }

    std::vector<std::uint8_t> encode_state() const;
    bool decode_state(const std::vector<std::uint8_t>& bytes);

private:
    struct JobEntry {
        ScheduledJob job;
        JobCallback callback;
    };

    void update_day_and_weather();
    std::int32_t timezone_offset_minutes(std::int64_t wall_unix_seconds) const;
    static acnet::Revision next_revision(acnet::Revision revision);

    ClockConfig config_;
    ClockState state_;
    std::mt19937_64 random_;
    std::unordered_map<std::string, JobEntry> jobs_;
    bool initialized_ = false;
};

} // namespace acserver
