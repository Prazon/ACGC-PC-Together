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
#include "acnet/shop.hpp"
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
    /* 1.0 ships a four-player town. The wire format and the runtime still
     * support up to kMaxPlayersPerZone (16) and config.cpp still accepts it,
     * so raising this is a server.ini edit rather than a rebuild -- but four
     * is what is tested and supported. */
    std::size_t capacity = 4;
    std::uint32_t tick_rate = 60;
    std::uint32_t snapshot_rate = 15;
    std::uint64_t connection_timeout_ms = 30000;
    std::uint64_t build_id = 0;
    // Blank runs the town open: no invite proof, no session encryption.
    std::string invite_key;
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

/* What an operator needs to address a gift: who the account is, what it already
 * holds, and how much room is left in its mailbox. */
struct RuntimeAccountSummary {
    acnet::AccountId account = 0;
    std::uint8_t resident_slot = 0xFF;
    bool resident = false;
    bool connected = false;
    std::string name;
    std::uint32_t bells = 0;
    std::uint64_t bank_balance = 0;
    std::uint64_t debt = 0;
    std::size_t pending_mail = 0;
    std::size_t carried_mail = 0;
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
    /* Operator gifts. Both commit through the same authority, journal, and
     * audit path a player transaction uses, so a gift survives a restart and is
     * attributable afterwards. */
    bool grant_bank_bells(acnet::AccountId account, std::uint64_t amount, std::string& error);
    /* Set the town's lifetime shop sales and re-derive Nook's level from them.
     * Granting bells cannot do this: sales accumulate only from committed Buy
     * and Sell transactions, so reaching Nookington's by shopping means
     * spending 240,000 bells through a five-row shelf. `visitor` sets the
     * outside-shopper flag Nookington's additionally requires. */
    bool set_shop_sales(std::uint32_t sales, bool visitor, std::string& error);
    /* Put a K.K. song in a house's stereo. `slot` is the original resident slot
     * 0-3; `song` is a bit index into the 64-bit music_box bitfield. */
    bool grant_house_song(std::uint8_t slot, std::uint8_t song, std::string& error);
    bool send_mail(acnet::AccountId recipient,
                   std::uint16_t attachment,
                   const std::string& text,
                   std::string& error);
    std::vector<RuntimeAccountSummary> account_summaries() const;
    bool import_gci(const std::filesystem::path& source, std::string& error);
    bool export_gci(const std::filesystem::path& destination, std::string& error) const;

    std::uint16_t bound_port() const { return socket_.bound_port(); }
    acnet::Tick tick() const { return movement_.current_tick(); }
    const RuntimeMetrics& metrics() const { return metrics_; }
    std::size_t connected_clients() const { return connections_.size(); }
    std::size_t connected_residents() const;
    std::size_t connected_visitors() const;
    std::size_t registered_residents() const;
    /* NPC villagers the server is simulating. Until villager replication lands
     * the runtime registers only the placeholder shopkeeper, so this reports 1
     * rather than a full town's roster. */
    /* The town's neighbours, not the two placeholder service NPCs the runtime
     * registers. Reads 0 until a client hands over a generated roster. */
    std::size_t villager_count() const {
        std::size_t count = 0;
        for (const acnet::VillagerSlot& slot : villagers_.slots) {
            if (slot.occupied) ++count;
        }
        return count;
    }
    /* The town's weekly stalk market, for the operator console. */
    const acnet::TurnipMarket& turnip_market() const { return turnips_; }
    std::uint8_t shop_tier() const { return static_cast<std::uint8_t>(shop_stock_.tier); }
    std::uint32_t shop_sales_sum() const { return shop_stock_.sales_sum; }
    /* 0 = Sunday, matching the schedule's own indexing. */
    int town_weekday() const {
        return acnet::town_date_from_seconds(clock_.state().town_unix_seconds).weekday;
    }
    std::vector<RuntimePlayerStatus> player_statuses() const;
    std::vector<RuntimeEvent> recent_events() const;
    bool town_initialized() const { return town_bootstrapped_; }
    /* Island status for the operator console. The island is only authoritative
     * once a client has reported its acre layout, which a town created before
     * island support has never done -- so "awaiting" here is a real state an
     * operator needs to be able to see, not a startup blip. */
    struct IslandStatus {
        bool terrain_ready = false;
        std::size_t tiles = 0;
        std::size_t outdoor_players = 0;
        std::size_t cabin_players = 0;
        std::size_t islander_house_players = 0;
        std::size_t cabin_furniture = 0;
        bool islander_present = false;
    };
    IslandStatus island_status() const;
    /* Read-only tile lookup for operators and tests. Mutation stays inside the
     * transaction path in WorldAuthority. */
    const acnet::TileState* tile(acnet::ZoneId zone, std::int16_t x, std::int16_t z) const {
        return world_.tile({zone, x, z});
    }
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
        acnet::CustomPattern pattern;
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

    struct DoorTransition {
        acnet::AccountId account = 0;
        std::uint32_t door_id = 0;
        acnet::ZoneId source_zone = 0;
        acnet::ZoneId destination_zone = 0;
        acnet::Transform source_transform;
        acnet::Tick expires_tick = 0;
        bool ready = false;
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
    void publish_population_change();
    void publish_resident_change();
    bool publish_mail_change(const acnet::MailRecord& record, bool removed, std::string& error);
    void publish_presentation(const acnet::PlayerView& player);
    void refresh_equipped_item(acnet::AccountId account);
    /* Publish an NPC's new state to the zone it stands in. Silently drops an
     * unencodable NPC rather than failing the tick: presentation state is not
     * worth stopping a town over, and the next baseline carries it anyway. */
    void publish_npc_change(const acnet::NpcState& npc);
    acnet::TownOccupancy current_occupancy() const;
    acnet::ResidentRoster current_roster() const;
    bool send_deltas(Connection& connection, std::uint64_t monotonic_ms, std::string& error);
    bool refresh_interest_chunk(Connection& connection, std::uint64_t monotonic_ms, std::string& error);
    bool dispatch(Connection& connection,
                  acnet::DecodedPacket packet,
                  std::uint64_t monotonic_ms,
                  std::string& error);
    void disconnect_timed_out(std::uint64_t monotonic_ms);
    void deactivate_player(acnet::AccountId account, acnet::Tick tick);
    void record_event(std::string message);
    /* Calendar year of the town clock, which is what the new year's grab bag
     * costs. */
    std::uint16_t town_year() const;
    std::vector<std::uint8_t> encode_state() const;
    bool decode_state(const std::vector<std::uint8_t>& payload, std::string& error);
    bool commit_state(std::uint16_t record_type, std::string& error);
    bool commit_transaction(acnet::AccountId account,
                            std::uint16_t operation_type,
                            acnet::ResultCode result,
                            std::string& error);
    bool configure_zone_topology(std::string& error);
    bool configure_island_topology(std::string& error);
    bool install_island_tiles(const acnet::TownBootstrap& request, std::string& error);
    bool allow_message(Connection& connection, acnet::MessageType type, std::uint64_t monotonic_ms);
    bool allow_hello(const std::string& endpoint, std::uint64_t monotonic_ms);
    std::uint8_t house_light_mask() const;
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
    /* Tier, goods power, and the town's A/B/C rarity permutation. The rolled
     * shelf itself lives in EconomyAuthority's ShopState; this is the state the
     * daily roll needs to reproduce it. */
    acnet::ShopStockState shop_stock_;
    /* The town's weekly turnip schedule. Server-owned because every client
     * used to roll its own, and because turnips are priced from this rather
     * than from the static tables. */
    acnet::TurnipMarket turnips_;
    /* The town tune. Anyone may retune it at the town hall, so it carries a
     * revision and a stale edit is refused. */
    acnet::TownTune town_tune_;
    std::unordered_map<std::uint64_t, acnet::TownTuneResult> town_tune_idempotency_;
    /* The noticeboard. The server owns the FIFO eviction, which is the part two
     * simultaneous posters contend over. */
    std::vector<acnet::NpcState> pending_npc_republish_;
    /* The connection whose AI drives the villagers. Chosen by the server so
     * exactly one client simulates; everyone else is told and follows. */
    acnet::AccountId npc_simulation_host_ = 0;
    bool refresh_npc_simulation_host(std::string& error);
    acnet::VillagerRoster villagers_;
    /* Registers one NpcState per occupied roster slot so villagers are real
     * server entities -- which is what conversation leases address. */
    bool sync_villager_npcs(std::string& error);
    /* Publishes the roster after the server changes it. */
    void publish_villagers();
    /* The daily turnover: opens a move-in when one is due, and empties the slot
     * of anybody who announced they were leaving. */
    bool run_villager_turnover(std::string& error);
    std::unordered_map<std::uint64_t, acnet::VillagerResult> villager_idempotency_;
    acnet::NoticeBoard notices_;
    std::unordered_map<std::uint64_t, acnet::NoticePostResult> notice_idempotency_;
    /* The fruit this town grows, reported once by the bootstrapping client.
     * Zero until then, which prices every fruit as foreign. */
    std::uint16_t native_fruit_ = 0;
    acnet::EncounterAuthority encounters_;
    acnet::NpcAuthority npcs_;
    acnet::ZoneCoordinator zones_;
    acnet::HousingAuthority housing_;
    acnet::DeltaLog deltas_;
    TownClock clock_;
    PersistenceStore persistence_;
    DatabaseStore database_;
    std::unordered_map<acnet::SessionId, Connection> connections_;
    std::unordered_map<acnet::AccountId, DoorTransition> door_transitions_;
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
    std::uint8_t last_published_population_ = 0;
    std::uint8_t last_published_capacity_ = 0;
    acnet::ResidentRoster last_published_roster_;
    bool roster_published_ = false;
    std::string background_error_;
    bool initialized_ = false;
};

std::uint64_t monotonic_milliseconds();
std::int64_t wall_unix_seconds();

} // namespace acserver
