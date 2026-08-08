#include "acnet/client.hpp"
#include "acnet/transport.hpp"
#include "acserver/town_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

struct QueuedDatagram {
    std::uint64_t due = 0;
    std::uint16_t destination = 0;
    std::vector<std::uint8_t> bytes;
};

class ChaosProxy {
public:
    explicit ChaosProxy(std::uint16_t server_port, std::uint32_t seed)
        : server_port_(server_port), random_(seed) {}

    bool open(std::string& error) { return socket_.open(0, error); }
    std::uint16_t port() const { return socket_.bound_port(); }

    bool pump(std::uint64_t now, std::string& error) {
        acnet::Datagram datagram;
        while (socket_.receive(datagram, error)) {
            const bool from_server = datagram.port == server_port_;
            if (!from_server) client_port_ = datagram.port;
            const std::uint16_t destination = from_server ? client_port_ : server_port_;
            if (destination == 0) continue;
            ++captured_;
            if (percent_(random_) < 12) {
                ++dropped_;
                continue;
            }
            queue(datagram.bytes, destination, now);
            if (percent_(random_) < 5) {
                queue(datagram.bytes, destination, now + 1 + (random_() % 40));
                ++duplicated_;
            }
        }
        if (!error.empty()) return false;
        std::stable_sort(queue_.begin(), queue_.end(), [](const QueuedDatagram& a, const QueuedDatagram& b) {
            return a.due < b.due;
        });
        std::size_t delivered = 0;
        while (delivered < queue_.size() && queue_[delivered].due <= now) {
            if (!socket_.send("127.0.0.1", queue_[delivered].destination,
                              queue_[delivered].bytes, error)) return false;
            ++delivered_;
            ++delivered;
        }
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(delivered));
        return true;
    }

    std::uint64_t captured() const { return captured_; }
    std::uint64_t dropped() const { return dropped_; }
    std::uint64_t duplicated() const { return duplicated_; }

private:
    void queue(const std::vector<std::uint8_t>& bytes, std::uint16_t destination, std::uint64_t now) {
        /* The broad random delay creates jitter and natural reordering.  A
         * small fixed floor represents propagation latency in each direction. */
        queue_.push_back({now + 20 + random_() % 181, destination, bytes});
    }

    std::uint16_t server_port_ = 0;
    std::uint16_t client_port_ = 0;
    acnet::UdpSocket socket_;
    std::mt19937 random_;
    std::uniform_int_distribution<int> percent_{0, 99};
    std::vector<QueuedDatagram> queue_;
    std::uint64_t captured_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t duplicated_ = 0;
};

struct Bot {
    std::unique_ptr<ChaosProxy> proxy;
    std::unique_ptr<acnet::ClientRuntime> client;
    acnet::Transform presentation;
    acnet::PlayerAnimation animation;
};

} // namespace

int main(int argc, char** argv) {
    std::size_t bot_count = 8;
    std::uint64_t ticks = 2400;
    if (argc > 1) bot_count = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (argc > 2) ticks = std::strtoull(argv[2], nullptr, 10);
    if (bot_count < 2 || bot_count > 16 || ticks < 1200) return 2;

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("acgc-chaos-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};

    acserver::TownRuntimeConfig server_config;
    server_config.port = 0;
    server_config.capacity = bot_count;
    server_config.connection_timeout_ms = 30000;
    server_config.data_directory = root / "town";
    server_config.invite_key = "automated-chaos-test";
    acserver::TownRuntime server(server_config);
    std::string error;
    constexpr std::int64_t wall = 1700000000;
    if (!server.initialize(wall, error)) { std::cerr << error << '\n'; return 3; }

    std::vector<Bot> bots;
    for (std::size_t i = 0; i < bot_count; ++i) {
        Bot bot;
        bot.proxy = std::make_unique<ChaosProxy>(server.bound_port(), 0xAC6C0000U + static_cast<std::uint32_t>(i));
        if (!bot.proxy->open(error)) { std::cerr << error << '\n'; return 4; }
        acnet::ClientConfig config;
        config.server_port = bot.proxy->port();
        config.account = 20000 + i;
        config.timeout_ms = 10000;
        config.invite_key = server_config.invite_key;
        bot.client = std::make_unique<acnet::ClientRuntime>(config);
        bot.presentation.position = {1980.0F, 0.0F, 780.0F};
        if (!bot.client->start(1000, error)) { std::cerr << error << '\n'; return 5; }
        bots.push_back(std::move(bot));
    }

    bool world_requested = false;
    bool world_completed = false;
    bool reconnected = false;
    bool reconnect_stopped = false;
    acnet::Revision dropped_tile_revision = 0;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        const std::uint64_t now = 1000 + tick * 17;
        for (Bot& bot : bots) if (!bot.proxy->pump(now, error)) { std::cerr << error << '\n'; return 6; }

        for (std::size_t i = 0; i < bots.size(); ++i) {
            if (bots[i].client->state() == acnet::ClientConnectionState::Offline) continue;
            const std::int16_t x = ((tick / 180 + i) & 1U) == 0 ? 7000 : -7000;
            const std::int16_t z = ((tick / 240 + i) & 1U) == 0 ? 5000 : -5000;
            acnet::Transform corrected;
            bool correction = false;
            if (!bots[i].client->frame(now, x, z, 0, 0, bots[i].animation, {}, bots[i].presentation,
                                       corrected, correction, error)) {
                std::cerr << error << '\n'; return 7;
            }
            if (correction) bots[i].presentation = corrected;
        }
        for (Bot& bot : bots) if (!bot.proxy->pump(now, error)) { std::cerr << error << '\n'; return 8; }
        if (!server.step(now, wall + static_cast<std::int64_t>(tick / 60), error)) {
            std::cerr << error << '\n'; return 9;
        }
        for (Bot& bot : bots) {
            if (!bot.proxy->pump(now, error) ||
                (bot.client->state() != acnet::ClientConnectionState::Offline && !bot.client->poll(now, error))) {
                std::cerr << error << '\n'; return 10;
            }
        }

        if (!world_requested && tick > 700 && bots[0].client->baseline() != nullptr) {
            const acnet::ZoneBaseline& baseline = *bots[0].client->baseline();
            const auto tile = std::find_if(baseline.tiles.begin(), baseline.tiles.end(), [](const auto& entry) {
                return entry.first.x == 49 && entry.first.z == 19;
            });
            if (tile != baseline.tiles.end()) {
                acnet::WorldOperation drop;
                drop.type = acnet::WorldOpType::DropItem;
                drop.idempotency = {0xCA05, 1};
                drop.tile = tile->first;
                drop.expected_tile_revision = tile->second.revision;
                drop.expected_inventory_revision = baseline.inventory.revision;
                drop.inventory_slot = 0;
                drop.expected_item = 0x2203;
                if (!bots[0].client->request(drop, now, error)) { std::cerr << error << '\n'; return 11; }
                world_requested = true;
            }
        }
        if (const auto result = bots[0].client->take_world_result(); result.has_value()) {
            if (result->code != acnet::ResultCode::Ok) return 12;
            dropped_tile_revision = result->tile_revision;
            world_completed = true;
        }

        /* Reuse the same ClientRuntime so its signed reconnect credential is
         * exercised; the proxy also changes the client's observed UDP port. */
        if (!reconnect_stopped && tick == 1500 &&
            bots.back().client->state() == acnet::ClientConnectionState::Connected) {
            bots.back().client->stop(now);
            reconnect_stopped = true;
        }
        if (reconnect_stopped && !reconnected && tick == 1540) {
            if (!bots.back().client->start(now + 1, error)) { std::cerr << error << '\n'; return 13; }
            reconnected = true;
        }
    }

    std::uint64_t captured = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duplicated = 0;
    for (std::size_t i = 0; i < bots.size(); ++i) {
        const Bot& bot = bots[i];
        if (bot.client->state() != acnet::ClientConnectionState::Connected || bot.client->baseline() == nullptr) {
            std::cerr << "bot " << i << " state=" << static_cast<unsigned>(bot.client->state())
                      << " baseline=" << (bot.client->baseline() != nullptr) << " error="
                      << bot.client->last_error() << '\n';
            return 14;
        }
        captured += bot.proxy->captured();
        dropped += bot.proxy->dropped();
        duplicated += bot.proxy->duplicated();
    }
    if (!world_requested || !world_completed || dropped_tile_revision < 2 || !reconnected ||
        dropped == 0 || duplicated == 0) return 15;
    for (Bot& bot : bots) bot.client->stop(1000 + ticks * 17);
    if (!server.shutdown(error)) { std::cerr << error << '\n'; return 16; }
    std::cout << "{\"chaos\":\"pass\",\"bots\":" << bot_count << ",\"ticks\":" << ticks
              << ",\"captured\":" << captured << ",\"dropped\":" << dropped
              << ",\"duplicated\":" << duplicated << "}\n";
    return 0;
}
