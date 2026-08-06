#include "acserver/town_runtime.hpp"
#include "acserver/config.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::atomic<bool> running{true};

void stop_server(int) {
    running.store(false);
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
    try {
        std::size_t consumed = 0;
        value = std::stoull(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

void usage() {
    std::cout << "AnimalCrossingServer [--port N] [--town N] [--data DIR] [--config FILE] [--ticks N] "
                 "--invite-key KEY [--smoke] [--no-sleep] [--no-dashboard] [--insecure-local] "
                 "[--ban N|--unban N|--import-gci FILE|--export-gci FILE|--checkpoint-now]\n";
}

const char* weather_name(acserver::Weather weather) {
    switch (weather) {
        case acserver::Weather::Clear: return "Clear";
        case acserver::Weather::Cloudy: return "Cloudy";
        case acserver::Weather::Rain: return "Rain";
        case acserver::Weather::Snow: return "Snow";
    }
    return "Unknown";
}

std::string format_town_time(std::int64_t seconds) {
    const std::time_t raw = static_cast<std::time_t>(seconds);
    std::tm value{};
#ifdef _WIN32
    if (gmtime_s(&value, &raw) != 0) return "invalid";
#else
    if (gmtime_r(&raw, &value) == nullptr) return "invalid";
#endif
    char output[32]{};
    return std::strftime(output, sizeof(output), "%Y-%m-%d %H:%M:%S", &value) == 0 ? "invalid" : output;
}

std::string format_event_time(std::int64_t seconds) {
    const std::time_t raw = static_cast<std::time_t>(seconds);
    std::tm value{};
#ifdef _WIN32
    if (gmtime_s(&value, &raw) != 0) return "--:--:--";
#else
    if (gmtime_r(&raw, &value) == nullptr) return "--:--:--";
#endif
    char output[16]{};
    return std::strftime(output, sizeof(output), "%H:%M:%S", &value) == 0 ? "--:--:--" : output;
}

std::string format_uptime(std::chrono::steady_clock::duration elapsed) {
    const auto total = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    const auto hours = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto seconds = total % 60;
    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':'
           << std::setw(2) << seconds;
    return output.str();
}

std::string shorten(std::string value, std::size_t maximum) {
    if (value.size() <= maximum) return value;
    if (maximum <= 3) return value.substr(0, maximum);
    value.resize(maximum - 3);
    value += "...";
    return value;
}

class OperatorConsole {
public:
    OperatorConsole() : started_(std::chrono::steady_clock::now()) {
#ifdef _WIN32
        const HANDLE standard_output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD standard_output_mode = 0;
        separate_log_stream_ = standard_output != nullptr && standard_output != INVALID_HANDLE_VALUE &&
                               GetConsoleMode(standard_output, &standard_output_mode) == FALSE;
        /* Start-Process, GUI launchers, and redirected automation may attach a
         * CUI executable to no visible console (or to an invisible pseudo
         * console). Give the operator an actual dashboard window in that case. */
        HWND console_window = GetConsoleWindow();
        if (console_window == nullptr || IsWindowVisible(console_window) == FALSE) {
            FreeConsole();
            AllocConsole();
            console_window = GetConsoleWindow();
        }
        if (console_window != nullptr) SetConsoleTitleA("Animal Crossing Dedicated Town Server");
        handle_ = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        available_ = handle_ != INVALID_HANDLE_VALUE;
#else
        available_ = isatty(STDOUT_FILENO) != 0;
        separate_log_stream_ = !available_;
#endif
    }

    ~OperatorConsole() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#endif
    }

    bool available() const { return available_; }
    bool needs_log_output() const { return !available_ || separate_log_stream_; }

    void render(const acserver::TownRuntime& runtime, const acserver::TownRuntimeConfig& config) {
        if (!available_) return;
        const auto& metrics = runtime.metrics();
        const auto& clock = runtime.clock_state();
        const auto players = runtime.player_statuses();
        const auto events = runtime.recent_events();
        std::ostringstream output;
        output << "================================================================================\n"
               << " ACGC DEDICATED TOWN SERVER     [ ONLINE ]     Uptime "
               << format_uptime(std::chrono::steady_clock::now() - started_) << "\n"
               << "================================================================================\n"
               << " Town       " << shorten(config.town_name, 8) << "  | ID " << config.town_id
               << " | Seed " << config.town_seed << " | UDP port " << runtime.bound_port() << "\n"
               << " Town time  " << format_town_time(clock.town_unix_seconds)
               << "  | Weather " << weather_name(clock.weather)
               << " (intensity " << static_cast<unsigned>(clock.weather_intensity) << ")\n"
               << " World      " << (runtime.town_initialized() ? "READY" : "AWAITING FIRST RESIDENT")
               << "  | Data " << shorten(config.data_directory.string(), 43) << "\n"
               << " Players    " << runtime.connected_clients() << '/' << config.capacity
               << " connected  | Residents " << runtime.connected_residents() << "/4 online, "
               << runtime.registered_residents() << "/4 registered  | Visitors "
               << runtime.connected_visitors() << "\n"
               << "--------------------------------------------------------------------------------\n"
               << " RESIDENT SLOTS\n";

        for (std::uint8_t slot = 0; slot < 4; ++slot) {
            const auto found = std::find_if(players.begin(), players.end(), [&](const auto& player) {
                return player.resident && player.resident_slot == slot;
            });
            output << "  [" << static_cast<unsigned>(slot) + 1U << "] ";
            if (found == players.end()) {
                output << "AVAILABLE\n";
            } else {
                output << std::left << std::setw(20) << shorten(found->name, 20) << std::right
                       << " account " << std::setw(8) << found->account << "  "
                       << (found->connected ? "ONLINE " : "offline") << "  zone " << found->zone << "\n";
            }
        }

        std::size_t shown_visitors = 0;
        for (const auto& player : players) {
            if (player.resident || !player.connected) continue;
            if (shown_visitors++ == 0) output << " CONNECTED VISITORS\n";
            output << "      " << std::left << std::setw(20) << shorten(player.name, 20) << std::right
                   << " account " << std::setw(8) << player.account << "  zone " << player.zone << "\n";
            if (shown_visitors == 4) break;
        }
        if (shown_visitors == 0) output << " CONNECTED VISITORS  none\n";

        output << "--------------------------------------------------------------------------------\n"
               << " LIVE METRICS\n"
               << "  Tick " << metrics.ticks << "  | RX " << metrics.packets_received << "  | TX "
               << metrics.packets_sent << "  | Snapshots " << metrics.snapshots_sent
               << "  | Reconnects " << metrics.reconnects << "\n"
               << "  Rejected " << metrics.rejected_packets << "  | Malformed " << metrics.malformed_packets
               << "  | Hourly jobs " << metrics.hourly_jobs << "  | Daily jobs " << metrics.daily_jobs << "\n"
               << "--------------------------------------------------------------------------------\n"
               << " RECENT ACTIVITY (verbose log)\n";
        const std::size_t begin = events.size() > 8 ? events.size() - 8 : 0;
        for (std::size_t i = begin; i < events.size(); ++i)
            output << "  [" << format_event_time(events[i].wall_unix_seconds) << "] "
                   << shorten(events[i].message, 66) << "\n";
        if (events.empty()) output << "  No activity yet.\n";
        output << "--------------------------------------------------------------------------------\n"
               << " Ctrl+C saves a checkpoint and shuts the town down safely.\n"
               << "================================================================================\n";
        write_screen(output.str());
    }

private:
    void write_screen(const std::string& contents) {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO info{};
        const COORD origin{0, 0};
        if (GetConsoleScreenBufferInfo(handle_, &info)) {
            const DWORD cells = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
            DWORD written = 0;
            FillConsoleOutputCharacterA(handle_, ' ', cells, origin, &written);
            FillConsoleOutputAttribute(handle_, info.wAttributes, cells, origin, &written);
            SetConsoleCursorPosition(handle_, origin);
        }
        DWORD written = 0;
        WriteConsoleA(handle_, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr);
#else
        std::cout << "\x1b[2J\x1b[H" << contents << std::flush;
#endif
    }

    bool available_ = false;
    bool separate_log_stream_ = false;
    std::chrono::steady_clock::time_point started_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

void print_dashboard(const acserver::TownRuntime& runtime, const acserver::TownRuntimeConfig& config) {
    const acserver::RuntimeMetrics& metrics = runtime.metrics();
    const acserver::ClockState& clock = runtime.clock_state();
    std::cout << "[TOWN] " << config.town_name
              << " | Players " << runtime.connected_clients() << '/' << config.capacity
              << " (residents " << runtime.connected_residents() << "/4, visitors "
              << runtime.connected_visitors() << ')'
              << " | Time " << format_town_time(clock.town_unix_seconds)
              << " | Weather " << weather_name(clock.weather)
              << " | World " << (runtime.town_initialized() ? "ready" : "awaiting first resident")
              << " | Tick " << metrics.ticks
              << " | RX/TX " << metrics.packets_received << '/' << metrics.packets_sent
              << " | Reconnects " << metrics.reconnects << '\n' << std::flush;
}

void print_startup_banner(const acserver::TownRuntime& runtime, const acserver::TownRuntimeConfig& config,
                          bool dashboard) {
    const acserver::ClockState& clock = runtime.clock_state();
    std::cout << "\n============================================================\n"
              << " Animal Crossing Dedicated Town Server\n"
              << "============================================================\n"
              << " Status       : ONLINE\n"
              << " Town         : " << config.town_name << " (ID " << config.town_id
              << ", seed " << config.town_seed << ")\n"
              << " Listen       : UDP 0.0.0.0:" << runtime.bound_port() << "\n"
              << " Players      : 0/" << config.capacity << " connected\n"
              << " Residents    : " << runtime.registered_residents() << "/4 registered\n"
              << " Town time    : " << format_town_time(clock.town_unix_seconds) << "\n"
              << " Weather      : " << weather_name(clock.weather) << "\n"
              << " World        : " << (runtime.town_initialized() ? "ready" : "awaiting first resident") << "\n"
              << " Data         : " << config.data_directory.string() << "\n"
              << " Security     : " << (config.invite_key.empty() ? "unauthenticated" : "invite key enabled") << "\n"
              << " Dashboard    : " << (dashboard ? "refreshes four times per second" : "disabled") << "\n"
              << " Stop safely  : press Ctrl+C\n"
              << "============================================================\n" << std::flush;
}

} // namespace

int main(int argc, char** argv) {
    /* Windows stdout can otherwise remain block-buffered when launched from a
     * batch file or redirected by an operator. Console status must be visible
     * immediately, not only when the process exits. */
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    acserver::TownRuntimeConfig config;
    std::filesystem::path config_file;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument != "--data" && argument != "--config") continue;
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        const std::string value = argv[++i];
        if (argument == "--data") config.data_directory = value;
        else config_file = value;
    }
    bool invite_required = true;
    std::string error;
    if (config_file.empty()) {
        config_file = "server.ini";
        std::error_code filesystem_error;
        if (!std::filesystem::exists(config_file, filesystem_error)) {
            const std::filesystem::path legacy_config = config.data_directory / "config.toml";
            if (std::filesystem::exists(legacy_config, filesystem_error)) {
                if (!acserver::load_town_config(legacy_config, config, invite_required, false, error) ||
                    !acserver::write_default_town_config(config_file, config, invite_required, error)) {
                    std::cerr << "Server configuration migration failed: " << error << '\n';
                    return 2;
                }
                std::cout << "Migrated legacy configuration to " << config_file.string() << '\n';
            } else if (!acserver::write_default_town_config(config_file, config, invite_required, error)) {
                std::cerr << "Server configuration creation failed: " << error << '\n';
                return 2;
            }
        }
    }
    if (!acserver::load_town_config(config_file, config, invite_required, false, error)) {
        std::cerr << "Server configuration failed: " << error << '\n';
        return 2;
    }
    if (!invite_required) config.allow_unauthenticated = true;
    if (const char* environment_key = std::getenv("ACGC_INVITE_KEY");
        environment_key != nullptr && environment_key[0] != '\0') {
        config.invite_key = environment_key;
    }

    std::uint64_t maximum_ticks = 0;
    bool smoke = false;
    bool no_sleep = false;
    bool dashboard = config.dashboard;
    bool checkpoint_now = false;
    std::uint64_t ban_account = 0;
    std::uint64_t unban_account = 0;
    std::filesystem::path import_gci;
    std::filesystem::path export_gci;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            usage();
            return 0;
        }
        if (argument == "--smoke") {
            smoke = true;
            continue;
        }
        if (argument == "--no-sleep") {
            no_sleep = true;
            continue;
        }
        if (argument == "--no-dashboard") {
            dashboard = false;
            continue;
        }
        if (argument == "--insecure-local") {
            config.allow_unauthenticated = true;
            continue;
        }
        if (argument == "--checkpoint-now") {
            checkpoint_now = true;
            continue;
        }
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        const std::string value = argv[++i];
        std::uint64_t number = 0;
        if (argument == "--data") {
            config.data_directory = value;
        } else if (argument == "--config") {
            // Loaded before command-line processing so CLI values win.
        } else if (argument == "--invite-key") {
            config.invite_key = value;
        } else if (argument == "--import-gci") {
            import_gci = value;
        } else if (argument == "--export-gci") {
            export_gci = value;
        } else if (!parse_u64(value, number)) {
            std::cerr << "Invalid number for " << argument << '\n';
            return 2;
        } else if (argument == "--port" && number <= 65535) {
            config.port = static_cast<std::uint16_t>(number);
        } else if (argument == "--town" && number != 0) {
            config.town_id = number;
        } else if (argument == "--ticks") {
            maximum_ticks = number;
        } else if (argument == "--ban" && number != 0) {
            ban_account = number;
        } else if (argument == "--unban" && number != 0) {
            unban_account = number;
        } else {
            usage();
            return 2;
        }
    }
    if (smoke) {
        config.port = 0;
        if (config.invite_key.empty()) config.allow_unauthenticated = true;
        no_sleep = true;
        if (maximum_ticks == 0) maximum_ticks = 120;
    }
    const bool one_shot_admin = checkpoint_now || ban_account != 0 || unban_account != 0 ||
                                !import_gci.empty() || !export_gci.empty();
    if (one_shot_admin && config.invite_key.empty()) config.allow_unauthenticated = true;
    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);
    OperatorConsole operator_console;
    acserver::TownRuntime runtime(config);
    if (!runtime.initialize(acserver::wall_unix_seconds(), error)) {
        std::cerr << "Server initialization failed: " << error << '\n';
        return 1;
    }
    if (!one_shot_admin) {
        if (dashboard && operator_console.available()) {
            operator_console.render(runtime, config);
        }
        if (!dashboard || operator_console.needs_log_output()) {
            print_startup_banner(runtime, config, dashboard);
            if (dashboard) print_dashboard(runtime, config);
        }
    }
    if (one_shot_admin) {
        bool ok = true;
        if (!import_gci.empty()) ok = runtime.import_gci(import_gci, error);
        if (ok && ban_account != 0) ok = runtime.set_account_banned(ban_account, true, error);
        if (ok && unban_account != 0) ok = runtime.set_account_banned(unban_account, false, error);
        if (ok && checkpoint_now) ok = runtime.checkpoint_now(error);
        if (ok && !export_gci.empty()) ok = runtime.export_gci(export_gci, error);
        std::string shutdown_error;
        if (!runtime.shutdown(shutdown_error) && error.empty()) error = shutdown_error;
        if (!ok || !error.empty()) { std::cerr << "Admin command failed: " << error << '\n'; return 1; }
        return 0;
    }
    const auto tick_duration = std::chrono::microseconds(1000000 / config.tick_rate);
    auto next_tick = std::chrono::steady_clock::now();
    auto next_operator_refresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    std::uint64_t next_log_dashboard_tick = static_cast<std::uint64_t>(config.tick_rate) * 2;
    while (running.load() && (maximum_ticks == 0 || runtime.metrics().ticks < maximum_ticks)) {
        if (!runtime.step(acserver::monotonic_milliseconds(), acserver::wall_unix_seconds(), error)) {
            std::cerr << "Server tick failed: " << error << '\n';
            runtime.shutdown(error);
            return 1;
        }
        if (dashboard && operator_console.available() &&
            std::chrono::steady_clock::now() >= next_operator_refresh) {
            operator_console.render(runtime, config);
            next_operator_refresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
        if (dashboard && operator_console.needs_log_output() &&
            runtime.metrics().ticks >= next_log_dashboard_tick) {
            print_dashboard(runtime, config);
            next_log_dashboard_tick = runtime.metrics().ticks + static_cast<std::uint64_t>(config.tick_rate) * 2;
        }
        if (!no_sleep) {
            next_tick += tick_duration;
            std::this_thread::sleep_until(next_tick);
        }
    }
    if (!runtime.shutdown(error)) {
        std::cerr << "Server shutdown failed: " << error << '\n';
        return 1;
    }
    return 0;
}
