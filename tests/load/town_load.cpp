#include "acnet/client.hpp"
#include "acserver/town_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    std::size_t bot_count = 8;
    std::uint64_t ticks = 3600;
    if (argc > 1) bot_count = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (argc > 2) ticks = std::strtoull(argv[2], nullptr, 10);
    if (bot_count == 0 || bot_count > 16 || ticks == 0) return 2;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("acgc-load-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    acserver::TownRuntimeConfig server_config;
    server_config.port = 0;
    server_config.capacity = bot_count;
    server_config.connection_timeout_ms = 60000;
    server_config.data_directory = root / "town";
    server_config.invite_key = "automated-load-test";
    acserver::TownRuntime server(server_config);
    std::string error;
    constexpr std::int64_t wall = 1700000000;
    if (!server.initialize(wall, error)) { std::cerr << error << '\n'; return 3; }
    struct Bot {
        std::unique_ptr<acnet::ClientRuntime> client;
        acnet::Transform transform;
        acnet::PlayerAnimation animation;
        bool holding = false;
    };
    std::vector<Bot> bots;
    for (std::size_t i = 0; i < bot_count; ++i) {
        acnet::ClientConfig config;
        config.server_port = server.bound_port();
        config.account = 10000 + i;
        config.invite_key = server_config.invite_key;
        Bot bot;
        bot.client = std::make_unique<acnet::ClientRuntime>(config);
        bot.transform.position = {20.0F, 0.0F, 20.0F};
        if (!bot.client->start(1000, error)) { std::cerr << error << '\n'; return 4; }
        bots.push_back(std::move(bot));
    }
    std::mt19937 random(123456);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        const std::uint64_t now = 1000 + tick * 17;
        for (std::size_t i = 0; i < bots.size(); ++i) {
            const double phase = static_cast<double>((tick + i * 19) % 240) / 240.0;
            const std::int16_t x = phase < 0.5 ? 9000 : -9000;
            const std::int16_t z = ((tick / 120 + i) & 1U) == 0 ? 6000 : -6000;
            acnet::Transform corrected;
            bool correction = false;
            if (!bots[i].client->frame(now, x, z, 0, 0, bots[i].animation, bots[i].transform,
                                       corrected, correction, error)) { std::cerr << error << '\n'; return 5; }
            if (correction) bots[i].transform = corrected;
            /* Catching requires the rod in hand, so each bot holds the one it
             * spawns with before any encounter is attempted. */
            if (!bots[i].holding && bots[i].client->baseline() != nullptr) {
                acnet::EconomyRequest hold;
                hold.type = acnet::EconomyOpType::HoldItem;
                hold.idempotency = {i + 1, 0xB01DU};
                hold.expected_inventory_revision = bots[i].client->baseline()->inventory.revision;
                hold.inventory_slot = 0;
                (void)bots[i].client->request(hold, now, error);
                error.clear();
                bots[i].holding = true;
            }
            if ((tick % 600) == 300 && i == static_cast<std::size_t>(random() % bots.size())) {
                acnet::EncounterRequest request;
                request.idempotency = {tick + 1, i + 1};
                request.expected_inventory_revision =
                    bots[i].client->baseline() != nullptr ? bots[i].client->baseline()->inventory.revision : 1;
                (void)bots[i].client->request(request, now, error);
                error.clear();
            }
        }
        if (!server.step(now, wall + static_cast<std::int64_t>(tick / 60), error)) {
            std::cerr << error << '\n'; return 6;
        }
        for (Bot& bot : bots) if (!bot.client->poll(now, error)) { std::cerr << error << '\n'; return 7; }
        if ((tick & 31U) == 0) std::this_thread::yield();
    }
    for (const Bot& bot : bots) {
        if (bot.client->state() != acnet::ClientConnectionState::Connected || bot.client->baseline() == nullptr) return 8;
    }
    for (Bot& bot : bots) bot.client->stop(1000 + ticks * 17);
    for (std::uint64_t i = 0; i < 20 && server.connected_clients() != 0; ++i)
        if (!server.step(1000 + ticks * 17 + i, wall + static_cast<std::int64_t>(ticks / 60), error)) return 9;
    if (!server.shutdown(error)) { std::cerr << error << '\n'; return 10; }
    std::cout << "{\"load\":\"pass\",\"bots\":" << bot_count << ",\"ticks\":" << ticks
              << ",\"packets_received\":" << server.metrics().packets_received << "}\n";
    return 0;
}
