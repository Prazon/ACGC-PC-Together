#pragma once

#include "acnet/economy.hpp"
#include "acnet/encounter.hpp"
#include "acnet/crypto.hpp"
#include "acnet/entity_registry.hpp"
#include "acnet/fragmentation.hpp"
#include "acnet/housing.hpp"
#include "acnet/movement.hpp"
#include "acnet/messages.hpp"
#include "acnet/npc.hpp"
#include "acnet/protocol.hpp"
#include "acnet/reliability.hpp"
#include "acnet/replication.hpp"
#include "acnet/session.hpp"
#include "acnet/transport.hpp"
#include "acnet/world.hpp"
#include "acnet/zone.hpp"
#include "acserver/persistence.hpp"
#include "acserver/database.hpp"
#include "acserver/town_clock.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acserver {

struct TownRuntimeConfig {
    acnet::TownId town_id = 1;
    std::string town_name = "NetTown";
    std::uint32_t town_seed = 1;
    std::uint16_t port = 24680;
    std::filesystem::path data_directory = "towns/default";
    std::size_t capacity = 16;
    std::uint32_t tick_rate = 60;
    std::uint32_t snapshot_rate = 15;
    std::uint64_t connection_timeout_ms = 30000;
    std::uint64_t build_id = 0;
    std::string invite_key;
    bool allow_unauthenticated = false;
    bool dashboard = true;
    ClockConfig clock;
};

struct RuntimeMetrics {
    std::uint64_t ticks = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t malformed_packets = 0;
    std::uint64_t rejected_packets = 0;
    std::uint64_t snapshots_sent = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t hourly_jobs = 0;
    std::uint64_t daily_jobs = 0;
};

struct RuntimePlayerStatus {
    acnet::AccountId account = 0;
    acnet::ZoneId zone = 0;
    std::uint8_t resident_slot = 0xFF;
    bool resident = false;
    bool connected = false;
    std::string name;
};

struct RuntimeEvent {
    std::uint64_t sequence = 0;
    std::int64_t wall_unix_seconds = 0;
    std::string message;
};

class TownRuntime {
public:
    explicit TownRuntime(TownRuntimeConfig config);

    bool initialize(std::int64_t wall_unix_seconds, std::string& error);
    bool step(std::uint64_t monotonic_ms, std::int64_t wall_unix_seconds, std::string& error);
    bool shutdown(std::string& error);
    bool checkpoint_now(std::string& error);
    bool set_account_banned(acnet::AccountId account, bool banned, std::string& error);
    bool import_gci(const std::filesystem::path& source, std::string& error);
    bool export_gci(const std::filesystem::path& destination, std::string& error) const;

    std::uint16_t bound_port() const { return socket_.bound_port(); }
    acnet::Tick tick() const { return movement_.current_tick(); }
    const RuntimeMetrics& metrics() const { return metrics_; }
    std::size_t connected_clients() const { return connections_.size(); }
    std::size_t connected_residents() const;
    std::size_t connected_visitors() const;
    std::size_t registered_residents() const;
    std::vector<RuntimePlayerStatus> player_statuses() const;
    std::vector<RuntimeEvent> recent_events() const;
    bool town_initialized() const { return town_bootstrapped_; }
    const ClockState& clock_state() const { return clock_.state(); }
    std::optional<acnet::Transform> player_transform(acnet::AccountId account) const {
        const acnet::PlayerView* player = players_.by_account(account);
        return player == nullptr ? std::nullopt : std::optional<acnet::Transform>(player->transform);
    }

private:
    struct AccountState {
        acnet::EntityId entity = 0;
        acnet::PlayerKind kind = acnet::PlayerKind::Visitor;
        acnet::ZoneId zone = 1;
        acnet::Transform transform;
        acnet::PlayerAppearance appearance;
        std::uint8_t resident_slot = 0xFF;
    };

    struct Connection {
        struct RateBucket {
            double tokens = 0.0;
            std::uint64_t updated_ms = 0;
        };
        acnet::SessionId session = 0;
        acnet::AccountId account = 0;
        std::string host;
        std::uint16_t port = 0;
        std::uint64_t last_received_ms = 0;
        acnet::ReliabilityPeer reliability;
        acnet::FragmentReassembler fragments;
        std::uint32_t next_transfer_id = 1;
        acnet::Revision last_delta_revision = 0;
        std::int16_t baseline_start_x = 0;
        std::int16_t baseline_start_z = 0;
        bool has_exterior_chunk = false;
        acnet::SessionKeys session_keys;
        bool encryption_active = false;
        std::unordered_map<std::uint16_t, RateBucket> rate_buckets;
    };

    bool receive_packets(std::uint64_t monotonic_ms, std::string& error);
    bool handle_datagram(const acnet::Datagram& datagram, std::uint64_t monotonic_ms, std::string& error);
    bool handle_hello(const acnet::Datagram& datagram,
                      const acnet::DecodedPacket& packet,
                      std::uint64_t monotonic_ms,
                      std::string& error);
    bool send_payload(Connection& connection,
                      acnet::MessageType type,
                      acnet::Channel channel,
                      const std::vector<std::uint8_t>& payload,
                      std::uint64_t monotonic_ms,
                      std::string& error);
    bool send_ack(Connection& connection, acnet::Channel channel, std::string& error);
    bool send_snapshots(std::uint64_t monotonic_ms, std::string& error);
    bool send_baseline(Connection& connection, std::uint64_t monotonic_ms, std::string& error);
    bool send_deltas(Connection& connection, std::uint64_t monotonic_ms, std::string& error);
    bool refresh_interest_chunk(Connection& connection, std::uint64_t monotonic_ms, std::string& error);
    bool dispatch(Connection& connection,
                  acnet::DecodedPacket packet,
                  std::uint64_t monotonic_ms,
                  std::string& error);
    void disconnect_timed_out(std::uint64_t monotonic_ms);
    void deactivate_player(acnet::AccountId account, acnet::Tick tick);
    void record_event(std::string message);
    std::vector<std::uint8_t> encode_state() const;
    bool decode_state(const std::vector<std::uint8_t>& payload, std::string& error);
    bool commit_state(std::uint16_t record_type, std::string& error);
    bool commit_transaction(acnet::AccountId account,
                            std::uint16_t operation_type,
                            acnet::ResultCode result,
                            std::string& error);
    bool configure_zone_topology(std::string& error);
    bool allow_message(Connection& connection, acnet::MessageType type, std::uint64_t monotonic_ms);
    bool allow_hello(const std::string& endpoint, std::uint64_t monotonic_ms);
    static std::string endpoint_key(const std::string& host, std::uint16_t port);

    TownRuntimeConfig config_;
    RuntimeMetrics metrics_;
    acnet::UdpSocket socket_;
    acnet::PlayerDirectory players_;
    acnet::EntityRegistry entities_;
    acnet::SessionTable sessions_;
    acnet::MovementSimulator movement_;
    acnet::WorldAuthority world_;
    acnet::EconomyAuthority economy_;
    acnet::EncounterAuthority encounters_;
    acnet::NpcAuthority npcs_;
    acnet::ZoneCoordinator zones_;
    acnet::HousingAuthority housing_;
    acnet::DeltaLog deltas_;
    TownClock clock_;
    PersistenceStore persistence_;
    DatabaseStore database_;
    std::unordered_map<acnet::SessionId, Connection> connections_;
    std::unordered_map<std::string, acnet::SessionId> endpoints_;
    std::unordered_map<acnet::AccountId, AccountState> accounts_;
    std::unordered_map<std::string, Connection::RateBucket> hello_rates_;
    std::deque<RuntimeEvent> recent_events_;
    std::uint64_t next_event_sequence_ = 1;
    bool town_bootstrapped_ = false;
    acnet::Tick last_checkpoint_tick_ = 0;
    std::int64_t last_clock_minute_ = -1;
    Weather last_weather_ = Weather::Clear;
    std::uint8_t last_weather_intensity_ = 0;
    std::string background_error_;
    bool initialized_ = false;
};

std::uint64_t monotonic_milliseconds();
std::int64_t wall_unix_seconds();

} // namespace acserver
