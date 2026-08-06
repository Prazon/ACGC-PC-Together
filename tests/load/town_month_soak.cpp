#include "acnet/client.hpp"
#include "acserver/town_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kBotCount = 4;
constexpr std::int64_t kDaySeconds = 24 * 60 * 60;

struct Bot {
    std::unique_ptr<acnet::ClientRuntime> client;
    acnet::Transform presentation;
    acnet::PlayerAnimation animation;
};

bool drive_connected(acserver::TownRuntime& server,
                     std::vector<Bot>& bots,
                     std::uint64_t monotonic_start,
                     std::int64_t wall,
                     std::string& error) {
    for (std::uint64_t frame = 0; frame < 240; ++frame) {
        const std::uint64_t now = monotonic_start + frame * 17;
        for (std::size_t i = 0; i < bots.size(); ++i) {
            acnet::Transform corrected;
            bool correction = false;
            const std::int16_t x = ((frame / 90 + i) & 1U) == 0 ? 4000 : -4000;
            if (!bots[i].client->frame(now, x, 0, 0, 0, bots[i].animation, bots[i].presentation,
                                       corrected, correction, error)) return false;
            if (correction) bots[i].presentation = corrected;
        }
        if (!server.step(now, wall, error)) return false;
        for (Bot& bot : bots) if (!bot.client->poll(now, error)) return false;
        if ((frame & 31U) == 0) std::this_thread::yield();
    }
    return std::all_of(bots.begin(), bots.end(), [](const Bot& bot) {
        return bot.client->state() == acnet::ClientConnectionState::Connected &&
               bot.client->baseline() != nullptr;
    });
}

} // namespace

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("acgc-month-soak-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};

    acserver::TownRuntimeConfig config;
    config.port = 0;
    config.capacity = kBotCount;
    config.connection_timeout_ms = 60000;
    config.data_directory = root / "town";
    config.invite_key = "automated-month-soak";

    constexpr std::int64_t initial_wall = 1700000000;
    constexpr std::array<unsigned, 5> days_per_restart{{7, 6, 6, 6, 6}};
    std::int64_t wall = initial_wall;
    std::uint64_t total_daily_jobs = 0;
    std::uint64_t total_hourly_jobs = 0;
    std::uint64_t total_packets = 0;
    bool injected_torn_write = false;
    std::string error;

    for (std::size_t cycle = 0; cycle < days_per_restart.size(); ++cycle) {
        auto server = std::make_unique<acserver::TownRuntime>(config);
        if (!server->initialize(wall, error)) {
            std::cerr << "restart " << cycle << ": " << error << '\n';
            return 3;
        }

        std::vector<Bot> bots;
        for (std::size_t i = 0; i < kBotCount; ++i) {
            acnet::ClientConfig client_config;
            client_config.server_port = server->bound_port();
            client_config.account = 30000 + i;
            client_config.timeout_ms = 30000;
            client_config.invite_key = config.invite_key;
            Bot bot;
            bot.client = std::make_unique<acnet::ClientRuntime>(client_config);
            bot.presentation.position = {1980.0F, 0.0F, 780.0F};
            if (!bot.client->start(1000, error)) {
                std::cerr << "client start: " << error << '\n';
                return 4;
            }
            bots.push_back(std::move(bot));
        }

        if (!drive_connected(*server, bots, 1000, wall, error)) {
            std::cerr << "connect cycle " << cycle << ": " << error << '\n';
            return 5;
        }
        for (std::size_t i = 0; i < bots.size(); ++i) {
            const acnet::ZoneBaseline& baseline = *bots[i].client->baseline();
            const auto own = std::find_if(baseline.players.begin(), baseline.players.end(), [i](const auto& player) {
                return player.account == 30000 + i;
            });
            if (own == baseline.players.end() || own->appearance.name[0] != 'P' ||
                own->appearance.gender != static_cast<std::uint8_t>((30000 + i) & 1U)) {
                std::cerr << "appearance did not survive restart " << cycle << '\n';
                return 6;
            }
        }

        wall += static_cast<std::int64_t>(days_per_restart[cycle]) * kDaySeconds;
        const std::uint64_t jump_now = 1000 + 241 * 17;
        if (!server->step(jump_now, wall, error) || !server->step(jump_now + 17, wall, error)) {
            std::cerr << "calendar advance " << cycle << ": " << error << '\n';
            return 7;
        }
        total_daily_jobs += server->metrics().daily_jobs;
        total_hourly_jobs += server->metrics().hourly_jobs;
        total_packets += server->metrics().packets_received;

        for (Bot& bot : bots) bot.client->stop(jump_now + 34);
        if ((cycle & 1U) != 0) {
            if (!server->shutdown(error)) {
                std::cerr << "clean shutdown " << cycle << ": " << error << '\n';
                return 8;
            }
        }
        bots.clear();
        server.reset(); // Even cycles deliberately simulate an abrupt process exit.

        if (cycle == 2) {
            std::ofstream journal(config.data_directory / "journal" / "operations.log",
                                  std::ios::binary | std::ios::app);
            const std::array<char, 5> torn{{'A', 'C', 'J', 'R', '\0'}};
            journal.write(torn.data(), static_cast<std::streamsize>(torn.size()));
            if (!journal) return 9;
            injected_torn_write = true;
        }
    }

    // A final independent boot proves the last crash/clean cycle persisted a
    // loadable world and that all recurring calendar jobs can continue.
    acserver::TownRuntime final_server(config);
    if (!final_server.initialize(wall, error) || !final_server.shutdown(error)) {
        std::cerr << "final recovery: " << error << '\n';
        return 10;
    }
    if (total_daily_jobs != 31 || total_hourly_jobs < 5 * 48 || !injected_torn_write || total_packets == 0) {
        std::cerr << "unexpected coverage daily=" << total_daily_jobs << " hourly=" << total_hourly_jobs
                  << " packets=" << total_packets << '\n';
        return 11;
    }

    std::cout << "{\"month_soak\":\"pass\",\"days\":" << total_daily_jobs
              << ",\"hourly_jobs\":" << total_hourly_jobs << ",\"restarts\":"
              << days_per_restart.size() << ",\"torn_write_recovered\":true,\"bots\":" << kBotCount
              << ",\"packets_received\":" << total_packets << "}\n";
    return 0;
}
