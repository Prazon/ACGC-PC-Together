#include "acserver/town_clock.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <limits>
#include <random>

namespace acserver {
namespace {

constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;

} // namespace

TownClock::TownClock(ClockConfig config, std::uint64_t random_seed) : config_(std::move(config)) {
    if (!std::isfinite(config_.scale) || config_.scale <= 0.0) config_.scale = 1.0;
    if (random_seed == 0) {
        std::random_device device;
        random_seed = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }
    random_.seed(random_seed);
    if (!config_.timezone.empty() && config_.timezone != "UTC") {
#ifdef _WIN32
        _putenv_s("TZ", config_.timezone.c_str());
        _tzset();
#else
        setenv("TZ", config_.timezone.c_str(), 1);
        tzset();
#endif
    }
}

std::int32_t TownClock::timezone_offset_minutes(std::int64_t wall_unix_seconds) const {
    if (config_.timezone.empty() || config_.timezone == "UTC") return config_.utc_offset_minutes;
    const std::time_t timestamp = static_cast<std::time_t>(wall_unix_seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &timestamp) != 0) return config_.utc_offset_minutes;
    const std::int64_t local_as_utc = static_cast<std::int64_t>(_mkgmtime64(&local));
#else
    if (localtime_r(&timestamp, &local) == nullptr) return config_.utc_offset_minutes;
    const std::int64_t local_as_utc = static_cast<std::int64_t>(timegm(&local));
#endif
    return static_cast<std::int32_t>((local_as_utc - wall_unix_seconds) / 60);
}

acnet::Revision TownClock::next_revision(acnet::Revision revision) {
    return revision == std::numeric_limits<acnet::Revision>::max() ? 1 : revision + 1;
}

bool TownClock::initialize(std::int64_t wall_unix_seconds) {
    if (wall_unix_seconds < 0) return false;
    state_.last_wall_unix_seconds = wall_unix_seconds;
    state_.town_unix_seconds = config_.starting_town_unix_seconds >= 0
        ? config_.starting_town_unix_seconds
        : wall_unix_seconds + static_cast<std::int64_t>(timezone_offset_minutes(wall_unix_seconds)) * 60;
    state_.day_number = state_.town_unix_seconds / kSecondsPerDay;
    state_.revision = 1;
    initialized_ = true;
    update_day_and_weather();
    return true;
}

bool TownClock::restore(const ClockState& state) {
    if (state.town_unix_seconds < 0 || state.last_wall_unix_seconds < 0 || state.revision == 0) return false;
    state_ = state;
    initialized_ = true;
    return true;
}

bool TownClock::add_job(const ScheduledJob& job_value, JobCallback callback) {
    if (job_value.name.empty() || job_value.interval_seconds <= 0 || job_value.next_due < 0 ||
        job_value.maximum_catchups == 0 || !callback || jobs_.find(job_value.name) != jobs_.end()) return false;
    jobs_.emplace(job_value.name, JobEntry{job_value, std::move(callback)});
    return true;
}

void TownClock::update_day_and_weather() {
    const std::int64_t day = state_.town_unix_seconds / kSecondsPerDay;
    if (day == state_.day_number && initialized_) return;
    state_.day_number = day;
    const std::uint64_t roll = random_() % 100;
    state_.weather = roll < 55 ? Weather::Clear : roll < 75 ? Weather::Cloudy : roll < 92 ? Weather::Rain
                                                                                         : Weather::Snow;
    state_.weather_intensity = static_cast<std::uint8_t>(random_() % 3);
    state_.revision = next_revision(state_.revision);
}

bool TownClock::advance(std::int64_t wall_unix_seconds, bool town_empty) {
    if (!initialized_ || wall_unix_seconds < 0) return false;
    std::int64_t wall_delta = wall_unix_seconds - state_.last_wall_unix_seconds;
    const bool backwards_wall = wall_delta < 0;
    if (wall_delta < 0 && !config_.allow_time_travel) wall_delta = 0;
    if (config_.mode == ClockMode::Fixed) wall_delta = 0;
    const double scale = config_.mode == ClockMode::Scaled ? config_.scale : 1.0;
    std::int64_t town_delta = static_cast<std::int64_t>(std::llround(static_cast<double>(wall_delta) * scale));
    if (config_.mode == ClockMode::Realtime && config_.starting_town_unix_seconds < 0) {
        const std::int64_t target = wall_unix_seconds +
            static_cast<std::int64_t>(timezone_offset_minutes(wall_unix_seconds)) * 60;
        town_delta = target - state_.town_unix_seconds;
        if (town_delta < 0 && !config_.allow_time_travel && backwards_wall) town_delta = 0;
    }
    state_.last_wall_unix_seconds = wall_unix_seconds;
    if (town_delta != 0) {
        state_.town_unix_seconds += town_delta;
        state_.revision = next_revision(state_.revision);
    }
    const std::int64_t old_day = state_.day_number;
    update_day_and_weather();
    (void)old_day;

    for (auto& item : jobs_) {
        JobEntry& entry = item.second;
        std::size_t catchups = 0;
        while (entry.job.next_due <= state_.town_unix_seconds && catchups < entry.job.maximum_catchups) {
            if (!entry.callback(entry.job, entry.job.next_due)) return false;
            entry.job.next_due += entry.job.interval_seconds;
            ++entry.job.run_count;
            ++catchups;
        }
        if (entry.job.next_due <= state_.town_unix_seconds) {
            // Coalesce excessive empty-town intervals while preserving the latest renewal.
            const std::int64_t behind = state_.town_unix_seconds - entry.job.next_due;
            const std::int64_t skipped = behind / entry.job.interval_seconds;
            entry.job.next_due += skipped * entry.job.interval_seconds;
            if (town_empty && entry.job.next_due <= state_.town_unix_seconds) {
                if (!entry.callback(entry.job, entry.job.next_due)) return false;
                entry.job.next_due += entry.job.interval_seconds;
                ++entry.job.run_count;
            }
        }
    }
    return true;
}

bool TownClock::set_time(std::int64_t town_unix_seconds,
                         std::int64_t wall_unix_seconds,
                         bool players_online) {
    if (!initialized_ || town_unix_seconds < 0 || wall_unix_seconds < 0 || players_online ||
        (!config_.allow_time_travel && town_unix_seconds < state_.town_unix_seconds)) return false;
    state_.town_unix_seconds = town_unix_seconds;
    state_.last_wall_unix_seconds = wall_unix_seconds;
    state_.revision = next_revision(state_.revision);
    update_day_and_weather();
    return true;
}

bool TownClock::set_weather(Weather weather, std::uint8_t intensity) {
    if (!initialized_ || static_cast<std::uint8_t>(weather) > static_cast<std::uint8_t>(Weather::Snow) ||
        intensity > 15) return false;
    state_.weather = weather;
    state_.weather_intensity = intensity;
    state_.revision = next_revision(state_.revision);
    return true;
}

const ScheduledJob* TownClock::job(const std::string& name) const {
    const auto found = jobs_.find(name);
    return found == jobs_.end() ? nullptr : &found->second.job;
}

std::vector<std::uint8_t> TownClock::encode_state() const {
    acnet::ByteWriter writer(128);
    writer.u64(static_cast<std::uint64_t>(state_.town_unix_seconds));
    writer.u64(static_cast<std::uint64_t>(state_.last_wall_unix_seconds));
    writer.u64(static_cast<std::uint64_t>(state_.day_number));
    writer.u8(static_cast<std::uint8_t>(state_.weather));
    writer.u8(state_.weather_intensity);
    writer.u32(state_.revision);
    return writer.ok() ? writer.data() : std::vector<std::uint8_t>{};
}

bool TownClock::decode_state(const std::vector<std::uint8_t>& bytes) {
    acnet::ByteReader reader(bytes);
    std::uint64_t town;
    std::uint64_t wall;
    std::uint64_t day;
    std::uint8_t weather;
    ClockState decoded;
    if (!reader.u64(town) || !reader.u64(wall) || !reader.u64(day) || !reader.u8(weather) ||
        !reader.u8(decoded.weather_intensity) || !reader.u32(decoded.revision) || !reader.finished() ||
        weather > static_cast<std::uint8_t>(Weather::Snow) || decoded.revision == 0 ||
        town > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        wall > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        day > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
    decoded.town_unix_seconds = static_cast<std::int64_t>(town);
    decoded.last_wall_unix_seconds = static_cast<std::int64_t>(wall);
    decoded.day_number = static_cast<std::int64_t>(day);
    decoded.weather = static_cast<Weather>(weather);
    return restore(decoded);
}

} // namespace acserver
