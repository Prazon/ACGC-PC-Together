#include "acserver/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace acserver {
namespace {

std::string trim(std::string value) {
    const auto space = [](unsigned char character) { return std::isspace(character) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
    return value;
}

std::string remove_comment(const std::string& line) {
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (escaped) {
            escaped = false;
        } else if (character == '\\' && quoted) {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
        } else if ((character == '#' || character == ';') && !quoted) {
            return line.substr(0, index);
        }
    }
    return line;
}

bool parse_unsigned(const std::string& text, std::uint64_t maximum, std::uint64_t& output) {
    if (text.empty() || text.front() == '-') return false;
    try {
        std::size_t consumed = 0;
        const std::uint64_t value = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || value > maximum) return false;
        output = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_signed(const std::string& text, std::int64_t minimum, std::int64_t maximum, std::int64_t& output) {
    try {
        std::size_t consumed = 0;
        const std::int64_t value = std::stoll(text, &consumed, 10);
        if (consumed != text.size() || value < minimum || value > maximum) return false;
        output = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_boolean(const std::string& text, bool& output) {
    if (text == "true") { output = true; return true; }
    if (text == "false") { output = false; return true; }
    return false;
}

bool parse_string(const std::string& text, std::string& output);

bool parse_ini_string(const std::string& text, std::string& output, bool allow_empty = false) {
    if (!text.empty() && text.front() == '"') {
        if (!parse_string(text, output)) return false;
    } else {
        output = trim(text);
        if (output.find_first_of("\r\n\0") != std::string::npos) return false;
    }
    return allow_empty || !output.empty();
}

bool valid_section(const std::string& section) {
    return section == "server" || section == "town" || section == "clock" ||
           section == "storage" || section == "security";
}

std::string quote_ini(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

const char* clock_mode_name(ClockMode mode) {
    switch (mode) {
        case ClockMode::Realtime: return "realtime";
        case ClockMode::Fixed: return "fixed";
        case ClockMode::Scaled: return "scaled";
    }
    return "realtime";
}

std::string format_datetime(std::int64_t seconds) {
    const std::time_t raw = static_cast<std::time_t>(seconds);
    std::tm civil{};
#ifdef _WIN32
    if (gmtime_s(&civil, &raw) != 0) return {};
#else
    if (gmtime_r(&raw, &civil) == nullptr) return {};
#endif
    char value[32]{};
    if (std::strftime(value, sizeof(value), "%Y-%m-%d %H:%M:%S", &civil) == 0) return {};
    return value;
}

bool parse_string(const std::string& text, std::string& output) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') return false;
    output.clear();
    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
        char character = text[index];
        if (character == '\\') {
            if (++index + 1 >= text.size()) return false;
            character = text[index];
            if (character != '\\' && character != '"') return false;
        }
        if (character == '\0' || character == '\n' || character == '\r') return false;
        output.push_back(character);
    }
    return true;
}

bool parse_double(const std::string& text, double minimum, double maximum, double& output) {
    try {
        std::size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size() || value < minimum || value > maximum) return false;
        output = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_datetime(const std::string& text, std::int64_t& output) {
    std::string value;
    if (!parse_string(text, value) || value.size() != 19 ||
        value[4] != '-' || value[7] != '-' || (value[10] != ' ' && value[10] != 'T') ||
        value[13] != ':' || value[16] != ':') return false;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(value.c_str(), "%4d-%2d-%2d%*c%2d:%2d:%2d",
                    &year, &month, &day, &hour, &minute, &second) != 6 ||
        year < 2000 || year > 9999 || month < 1 || month > 12 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    static constexpr int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int maximum_day = days_per_month[month - 1] + (month == 2 && leap ? 1 : 0);
    if (day < 1 || day > maximum_day) return false;
    std::tm civil{};
    civil.tm_year = year - 1900;
    civil.tm_mon = month - 1;
    civil.tm_mday = day;
    civil.tm_hour = hour;
    civil.tm_min = minute;
    civil.tm_sec = second;
#ifdef _WIN32
    const __time64_t encoded = _mkgmtime64(&civil);
#else
    const std::time_t encoded = timegm(&civil);
#endif
    if (encoded < 0) return false;
    output = static_cast<std::int64_t>(encoded);
    return true;
}

} // namespace

bool load_town_config(const std::filesystem::path& path,
                      TownRuntimeConfig& config,
                      bool& invite_required,
                      bool allow_missing,
                      std::string& error) {
    error.clear();
    std::ifstream input(path);
    if (!input) {
        std::error_code filesystem_error;
        if (allow_missing && !std::filesystem::exists(path, filesystem_error)) return true;
        error = "cannot open town configuration: " + path.string();
        return false;
    }

    std::string line;
    std::string section;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(remove_comment(line));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.size() < 3 || line.back() != ']') {
                error = "invalid INI section at line " + std::to_string(line_number);
                return false;
            }
            section = trim(line.substr(1, line.size() - 2));
            std::transform(section.begin(), section.end(), section.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            if (!valid_section(section)) {
                error = "unknown INI section [" + section + "] at line " + std::to_string(line_number);
                return false;
            }
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            error = "expected key = value at line " + std::to_string(line_number);
            return false;
        }
        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));
        std::uint64_t unsigned_value = 0;
        std::int64_t signed_value = 0;
        bool boolean_value = false;
        std::string string_value;
        double double_value = 0.0;
        bool valid = true;

        if (key == "town_id") {
            valid = parse_unsigned(value, std::numeric_limits<std::uint64_t>::max(), unsigned_value) && unsigned_value != 0;
            if (valid) config.town_id = unsigned_value;
        } else if (key == "town_name") {
            valid = parse_ini_string(value, string_value) && string_value.size() <= 8 &&
                    std::all_of(string_value.begin(), string_value.end(), [](unsigned char character) {
                        return character >= 32 && character <= 126;
                    });
            if (valid) config.town_name = string_value;
        } else if (key == "town_seed") {
            valid = parse_unsigned(value, std::numeric_limits<std::uint32_t>::max(), unsigned_value) &&
                    unsigned_value != 0;
            if (valid) config.town_seed = static_cast<std::uint32_t>(unsigned_value);
        } else if (key == "port") {
            valid = parse_unsigned(value, 65535, unsigned_value);
            if (valid) config.port = static_cast<std::uint16_t>(unsigned_value);
        } else if (key == "capacity") {
            valid = parse_unsigned(value, 16, unsigned_value) && unsigned_value != 0;
            if (valid) config.capacity = static_cast<std::size_t>(unsigned_value);
        } else if (key == "tick_rate") {
            valid = parse_unsigned(value, 240, unsigned_value) && unsigned_value != 0;
            if (valid) config.tick_rate = static_cast<std::uint32_t>(unsigned_value);
        } else if (key == "snapshot_rate") {
            valid = parse_unsigned(value, 120, unsigned_value) && unsigned_value != 0;
            if (valid) config.snapshot_rate = static_cast<std::uint32_t>(unsigned_value);
        } else if (key == "connection_timeout_ms") {
            valid = parse_unsigned(value, 300000, unsigned_value) && unsigned_value >= 1000;
            if (valid) config.connection_timeout_ms = unsigned_value;
        } else if (key == "build_id") {
            valid = parse_unsigned(value, std::numeric_limits<std::uint64_t>::max(), unsigned_value);
            if (valid) config.build_id = unsigned_value;
        } else if (key == "data_directory") {
            valid = parse_ini_string(value, string_value) && string_value.size() <= 1024;
            if (valid) {
                std::filesystem::path directory(string_value);
                if (directory.is_relative() && !path.parent_path().empty())
                    directory = path.parent_path() / directory;
                config.data_directory = directory.lexically_normal();
            }
        } else if (key == "dashboard") {
            valid = parse_boolean(value, boolean_value);
            if (valid) config.dashboard = boolean_value;
        } else if (key == "timezone") {
            valid = parse_ini_string(value, string_value) && string_value.size() <= 128;
            if (valid) config.clock.timezone = string_value;
        } else if (key == "utc_offset_minutes") {
            valid = parse_signed(value, -24 * 60, 24 * 60, signed_value);
            if (valid) config.clock.utc_offset_minutes = static_cast<std::int32_t>(signed_value);
        } else if (key == "clock_mode") {
            valid = parse_ini_string(value, string_value);
            if (valid && string_value == "realtime") config.clock.mode = ClockMode::Realtime;
            else if (valid && string_value == "fixed") config.clock.mode = ClockMode::Fixed;
            else if (valid && string_value == "scaled") config.clock.mode = ClockMode::Scaled;
            else valid = false;
        } else if (key == "clock_scale") {
            valid = parse_double(value, 0.01, 1000.0, double_value);
            if (valid) config.clock.scale = double_value;
        } else if (key == "sync_to_system_clock") {
            valid = parse_boolean(value, boolean_value);
            if (valid) config.clock.sync_to_system_clock = boolean_value;
        } else if (key == "starting_datetime") {
            valid = parse_datetime(value, signed_value);
            if (valid) config.clock.starting_town_unix_seconds = signed_value;
        } else if (key == "allow_time_travel") {
            valid = parse_boolean(value, boolean_value);
            if (valid) config.clock.allow_time_travel = boolean_value;
        } else if (key == "invite_required") {
            valid = parse_boolean(value, boolean_value);
            if (valid) invite_required = boolean_value;
        } else if (key == "invite_key") {
            valid = parse_ini_string(value, string_value, true) && string_value.size() <= 512;
            if (valid) config.invite_key = string_value;
        } else if (key == "empty_town_simulation") {
            valid = parse_ini_string(value, string_value) && string_value == "scheduled";
        } else {
            // Unknown keys are preserved for forward compatibility.
            continue;
        }

        if (!valid) {
            error = "invalid value for " + key + " at line " + std::to_string(line_number);
            return false;
        }
    }
    if (!input.eof()) {
        error = "failed while reading town configuration";
        return false;
    }
    if (config.snapshot_rate > config.tick_rate) {
        error = "snapshot_rate cannot exceed tick_rate";
        return false;
    }
    return true;
}

bool write_default_town_config(const std::filesystem::path& path,
                               const TownRuntimeConfig& config,
                               bool invite_required,
                               std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "cannot create server configuration directory: " + filesystem_error.message();
            return false;
        }
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "cannot create server configuration: " + path.string();
        return false;
    }
    std::filesystem::path data_directory = config.data_directory;
    if (!path.parent_path().empty()) {
        const std::filesystem::path relative = data_directory.lexically_relative(path.parent_path());
        if (!relative.empty()) data_directory = relative;
    }
    output << "; Animal Crossing dedicated-town host configuration\n"
              "; Stop the server before changing this file. Command-line options override it.\n\n"
              "[server]\n"
              "port = " << config.port << "\n"
              "capacity = " << config.capacity << "\n"
              "tick_rate = " << config.tick_rate << "\n"
              "snapshot_rate = " << config.snapshot_rate << "\n"
              "connection_timeout_ms = " << config.connection_timeout_ms << "\n"
              "build_id = " << config.build_id << "\n"
              "dashboard = " << (config.dashboard ? "true" : "false") << "\n\n"
              "[town]\n"
              "town_id = " << config.town_id << "\n"
              "town_name = " << quote_ini(config.town_name) << "\n"
              "; Keep this stable: it deterministically selects the original town layout.\n"
              "town_seed = " << config.town_seed << "\n\n"
              "[clock]\n"
              "timezone = " << quote_ini(config.clock.timezone) << "\n"
              "utc_offset_minutes = " << config.clock.utc_offset_minutes << "\n"
              "clock_mode = " << clock_mode_name(config.clock.mode) << "\n"
              "clock_scale = " << std::setprecision(15) << config.clock.scale << "\n"
              "; Continuously match the host system clock. Overrides clock_mode,\n"
              "; clock_scale, starting_datetime, and any administrative time change.\n"
              "sync_to_system_clock = " << (config.clock.sync_to_system_clock ? "true" : "false") << "\n";
    if (config.clock.starting_town_unix_seconds >= 0) {
        output << "; Used only for a new town; existing towns keep persisted time.\n"
                  "starting_datetime = "
               << quote_ini(format_datetime(config.clock.starting_town_unix_seconds)) << "\n";
    } else {
        output << "; Used only for a new town. Remove the leading ';' to set it.\n"
                  "; starting_datetime = \"2026-08-06 09:00:00\"\n";
    }
    output << "allow_time_travel = " << (config.clock.allow_time_travel ? "true" : "false") << "\n"
              "empty_town_simulation = scheduled\n\n"
              "[storage]\n"
              "data_directory = " << quote_ini(data_directory.generic_string()) << "\n\n"
              "[security]\n"
              "invite_required = " << (invite_required ? "true" : "false") << "\n"
              "; Keep this private. It may instead be supplied via ACGC_INVITE_KEY.\n"
              "invite_key = \"\"\n";
    if (!output) {
        error = "failed while writing server configuration: " + path.string();
        return false;
    }
    return true;
}

} // namespace acserver
