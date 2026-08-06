#include "acnet/entity_registry.hpp"
#include "acnet/client.hpp"
#include "acnet/crypto.hpp"
#include "acnet/economy.hpp"
#include "acnet/encounter.hpp"
#include "acnet/fragmentation.hpp"
#include "acnet/housing.hpp"
#include "acnet/interpolation.hpp"
#include "acnet/movement.hpp"
#include "acnet/messages.hpp"
#include "acnet/npc.hpp"
#include "acnet/player_query.hpp"
#include "acnet/protocol.hpp"
#include "acnet/reliability.hpp"
#include "acnet/replication.hpp"
#include "acnet/session.hpp"
#include "acnet/transport.hpp"
#include "acnet/world.hpp"
#include "acnet/zone.hpp"
#include "acserver/persistence.hpp"
#include "acserver/config.hpp"
#include "acserver/database.hpp"
#include "acserver/gci.hpp"
#include "acserver/town_clock.hpp"
#include "acserver/town_runtime.hpp"
#include "pc_network_config.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <deque>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define CHECK(expression)                                                                                 \
    do {                                                                                                  \
        if (!(expression)) throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +    \
                                             ": CHECK(" #expression ") failed");                         \
    } while (false)

bool same_float(float a, float b) {
    return std::fabs(a - b) < 0.0001F;
}

std::string hexadecimal(const std::uint8_t* bytes, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) output << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return output.str();
}

void cryptography_authenticates_and_rejects_tampering() {
    const std::string abc = "abc";
    const auto digest = acnet::sha256(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size());
    CHECK(hexadecimal(digest.data(), digest.size()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string key_text = "key";
    const std::string message = "The quick brown fox jumps over the lazy dog";
    const std::vector<std::uint8_t> key(key_text.begin(), key_text.end());
    const auto mac = acnet::hmac_sha256(key, reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
    CHECK(hexadecimal(mac.data(), mac.size()) ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    acnet::CryptoKey packet_key{};
    for (std::size_t i = 0; i < packet_key.size(); ++i) packet_key[i] = static_cast<std::uint8_t>(i + 1);
    acnet::PacketHeader header;
    header.message_type = acnet::MessageType::WorldRequest;
    header.channel = acnet::Channel::Transactions;
    header.flags = acnet::PacketReliable | acnet::PacketEncrypted;
    header.session = 0x1122334455667788ULL;
    header.sequence = 9;
    const std::vector<std::uint8_t> plaintext{1, 2, 3, 4, 5, 6, 7};
    std::vector<std::uint8_t> sealed;
    CHECK(acnet::seal_payload(header, packet_key, plaintext, sealed));
    CHECK(sealed != plaintext);
    std::vector<std::uint8_t> opened;
    CHECK(acnet::open_payload(header, packet_key, sealed, opened));
    CHECK(opened == plaintext);
    sealed[2] ^= 0x80;
    CHECK(!acnet::open_payload(header, packet_key, sealed, opened));
    std::array<std::uint8_t, 32> random{};
    CHECK(acnet::secure_random(random.data(), random.size()));
    CHECK(std::any_of(random.begin(), random.end(), [](std::uint8_t byte) { return byte != 0; }));
}

void fragmentation_reassembles_large_payloads() {
    std::vector<std::uint8_t> original(20000);
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = static_cast<std::uint8_t>((i * 37) & 0xFF);
    std::vector<std::vector<std::uint8_t>> encoded;
    CHECK(acnet::split_fragments(original, 77, encoded));
    CHECK(encoded.size() > 10);
    acnet::FragmentReassembler reassembler;
    std::optional<std::vector<std::uint8_t>> complete;
    std::uint64_t now = 100;
    for (auto it = encoded.rbegin(); it != encoded.rend(); ++it) {
        acnet::Fragment fragment;
        CHECK(acnet::decode_fragment(*it, fragment));
        const auto piece = reassembler.accept(fragment, now++);
        if (piece.has_value()) complete = piece;
        if (fragment.index == 3) CHECK(!reassembler.accept(fragment, now++).has_value());
    }
    CHECK(complete.has_value());
    CHECK(*complete == original);
    CHECK(reassembler.pending() == 0);
    encoded.front().pop_back();
    acnet::Fragment invalid;
    CHECK(acnet::decode_fragment(encoded.front(), invalid));
    CHECK(!reassembler.accept(invalid, now).has_value());
    reassembler.expire(now + 20000);
    CHECK(reassembler.pending() == 0);
}

void transaction_messages_round_trip() {
    acnet::WorldOperation world;
    world.type = acnet::WorldOpType::Bury;
    world.account = 44;
    world.idempotency = {10, 11};
    world.tile = {3, -4, 5};
    world.expected_tile_revision = 8;
    world.expected_inventory_revision = 9;
    world.inventory_slot = 2;
    world.expected_item = 0x1234;
    std::vector<std::uint8_t> bytes;
    CHECK(acnet::encode(world, bytes));
    acnet::WorldOperation decoded_world;
    CHECK(acnet::decode(bytes, decoded_world));
    CHECK(decoded_world.type == world.type);
    CHECK(decoded_world.tile.x == -4);
    CHECK(decoded_world.expected_item == 0x1234);

    acnet::EconomyRequest economy;
    economy.type = acnet::EconomyOpType::AttachMail;
    economy.account = 45;
    economy.idempotency = {12, 13};
    economy.expected_inventory_revision = 2;
    economy.expected_aux_revision = 3;
    economy.recipient = 99;
    economy.amount = 500;
    CHECK(acnet::encode(economy, bytes));
    acnet::EconomyRequest decoded_economy;
    CHECK(acnet::decode(bytes, decoded_economy));
    CHECK(decoded_economy.recipient == 99);
    CHECK(decoded_economy.amount == 500);

    acnet::HouseUpdate house;
    house.account = 46;
    house.idempotency = {14, 15};
    house.house_id = 10002;
    house.expected_house_revision = 7;
    house.upgrade_level = 3;
    house.main_light_on = true;
    house.music_tracks[0] = 91;
    house.furniture_switches[1] = 0x1122334455667788ULL;
    house.furniture[{4, 5, 2, 1}] = {0x3001, 2};
    CHECK(acnet::encode(house, bytes));
    acnet::HouseUpdate decoded_house;
    CHECK(acnet::decode(bytes, decoded_house));
    CHECK(decoded_house.house_id == house.house_id);
    CHECK(decoded_house.upgrade_level == 3);
    CHECK(decoded_house.main_light_on);
    CHECK(decoded_house.music_tracks[0] == 91);
    CHECK(decoded_house.furniture_switches[1] == 0x1122334455667788ULL);
    CHECK(decoded_house.furniture.at({4, 5, 2, 1}).item == 0x3001);

    acnet::ZoneReadyRequest ready;
    ready.account = 47;
    ready.token = {0x1234, 0x5678};
    ready.destination_transform.position = {160.0F, 0.0F, 300.0F};
    ready.destination_transform.velocity = {0.0F, 0.0F, -12.5F};
    ready.destination_transform.yaw = -32768;
    CHECK(acnet::encode(ready, bytes));
    acnet::ZoneReadyRequest decoded_ready;
    CHECK(acnet::decode(bytes, decoded_ready));
    CHECK(decoded_ready.account == ready.account);
    CHECK(decoded_ready.token == ready.token);
    CHECK(same_float(decoded_ready.destination_transform.position.x, 160.0F));
    CHECK(same_float(decoded_ready.destination_transform.position.z, 300.0F));
    CHECK(same_float(decoded_ready.destination_transform.velocity.z, -12.5F));
    CHECK(decoded_ready.destination_transform.yaw == -32768);

    std::vector<acnet::ReplicationDelta> deltas(2);
    deltas[0].revision = 4;
    deltas[0].kind = acnet::ResourceKind::Tile;
    deltas[0].zone = 1;
    deltas[0].has_position = true;
    deltas[0].position = {20.0F, 0.0F, -20.0F};
    deltas[0].payload = {1, 2, 3};
    deltas[1].revision = 5;
    deltas[1].kind = acnet::ResourceKind::Clock;
    CHECK(acnet::encode_deltas(deltas, bytes));
    std::vector<acnet::ReplicationDelta> decoded_deltas;
    CHECK(acnet::decode_deltas(bytes, decoded_deltas));
    CHECK(decoded_deltas.size() == 2);
    CHECK(decoded_deltas[0].payload == deltas[0].payload);
    bytes.push_back(0);
    CHECK(!acnet::decode_deltas(bytes, decoded_deltas));
}

void encounters_are_server_authoritative_and_idempotent() {
    acnet::PlayerDirectory players;
    acnet::PlayerView player;
    player.account = 5;
    player.entity = 9;
    player.zone = 1;
    CHECK(players.upsert(player));
    acnet::WorldAuthority world(&players);
    acnet::InventoryState inventory;
    inventory.slots[0].item = 0x2203;
    CHECK(world.register_inventory(player.account, inventory));
    acnet::EncounterAuthority encounters(&players, &world, 12345);
    acnet::EncounterRequest request;
    request.account = player.account;
    request.idempotency = {1, 2};
    request.kind = acnet::EncounterKind::Fish;
    request.expected_inventory_revision = inventory.revision;
    request.tool_slot = 0;
    const acnet::EncounterResult first = encounters.resolve(request, 100, 1700000000, 0);
    CHECK(first.code == acnet::ResultCode::Ok);
    CHECK(first.next_allowed_tick == 190);
    if (first.caught) CHECK(first.item >= 0x2300 && first.item <= 0x2305);
    const acnet::EncounterResult replay = encounters.resolve(request, 101, 1700000001, 3);
    CHECK(replay.replayed);
    CHECK(replay.code == first.code);
    CHECK(replay.item == first.item);
    CHECK(replay.caught == first.caught);
}

void runtime_replays_uncheckpointed_world_journal() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-runtime-replay-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    constexpr std::int64_t wall = 1700000000;
    acserver::TownRuntimeConfig config;
    config.port = 0;
    config.data_directory = root / "town";
    config.connection_timeout_ms = 60000;
    config.invite_key = "journal-test-key";
    std::string error;

    {
        acserver::TownRuntime server(config);
        CHECK(server.initialize(wall, error));
        acnet::ClientConfig client_config;
        client_config.server_port = server.bound_port();
        client_config.account = 808;
        client_config.invite_key = config.invite_key;
        acnet::ClientRuntime client(client_config);
        CHECK(client.start(1000, error));
        for (std::uint64_t i = 0; i < 100 && client.baseline() == nullptr; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(1000 + i, wall, error));
            CHECK(client.poll(1000 + i, error));
        }
        CHECK(client.baseline() != nullptr);
        acnet::WorldOperation drop;
        drop.type = acnet::WorldOpType::DropItem;
        drop.idempotency = {500, 501};
        /* Resident slot zero now starts clear of the train tracks at
         * (2200, 1000), which maps to foreground tile (55, 25). */
        drop.tile = {1, 55, 25};
        drop.expected_tile_revision = 1;
        drop.expected_inventory_revision = 1;
        drop.inventory_slot = 0;
        drop.expected_item = 0x2203;
        CHECK(client.request(drop, 1200, error));
        std::optional<acnet::WorldResult> result;
        for (std::uint64_t i = 0; i < 100 && !result.has_value(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(1200 + i, wall, error));
            CHECK(client.poll(1200 + i, error));
            result = client.take_world_result();
        }
        CHECK(result.has_value());
        CHECK(result->code == acnet::ResultCode::Ok);
        CHECK(result->transferred_item == 0x2203);
        // Intentionally skip TownRuntime::shutdown to model a crash after the durable acknowledgement.
    }

    acserver::TownRuntime recovered(config);
    CHECK(recovered.initialize(wall + 5, error));
    acnet::ClientConfig recovered_config;
    recovered_config.server_port = recovered.bound_port();
    recovered_config.account = 808;
    recovered_config.invite_key = config.invite_key;
    acnet::ClientRuntime recovered_client(recovered_config);
    CHECK(recovered_client.start(3000, error));
    for (std::uint64_t i = 0; i < 100 && recovered_client.baseline() == nullptr; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(recovered.step(3000 + i, wall + 5, error));
        CHECK(recovered_client.poll(3000 + i, error));
    }
    CHECK(recovered_client.baseline() != nullptr);
    bool found = false;
    for (const auto& tile : recovered_client.baseline()->tiles) {
        if (tile.first.x == 55 && tile.first.z == 25) {
            found = true;
            CHECK(tile.second.item == 0x2203);
            CHECK(tile.second.revision == 2);
        }
    }
    CHECK(found);
    recovered_client.stop(4000);
    CHECK(recovered.shutdown(error));
}

void sqlite_metadata_uses_wal_and_migrations() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-database-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::string error;
    {
        acserver::DatabaseStore database(root);
        CHECK(database.initialize(error));
        CHECK(database.schema_version(error) == 3);
        CHECK(database.journal_mode(error) == "wal");
        CHECK(database.record_account(42, 1700000000, error));
        CHECK(database.record_session(91, 42, true, 1700000001, error));
        CHECK(database.record_transaction(1, 42, 20, acnet::ResultCode::Ok, 1700000002, error));
        CHECK(database.audit(42, "test_action", "bounded details", 1700000003, error));
        CHECK(std::filesystem::exists(database.path()));
        CHECK(!std::filesystem::exists(root / "config.toml"));
    }
    acserver::DatabaseStore reopened(root);
    CHECK(reopened.initialize(error));
    CHECK(reopened.schema_version(error) == 3);
    CHECK(reopened.journal_mode(error) == "wal");
}

void named_timezone_applies_dst_transitions() {
    acserver::ClockConfig config;
    config.timezone = "America/Winnipeg";
    acserver::TownClock winter(config, 10);
    CHECK(winter.initialize(1705320000));
    CHECK(winter.state().town_unix_seconds - 1705320000 == -6 * 3600);
    acserver::TownClock summer(config, 10);
    CHECK(summer.initialize(1721044800));
    CHECK(summer.state().town_unix_seconds - 1721044800 == -5 * 3600);
}

void system_clock_sync_overrides_seeded_and_scaled_time() {
    constexpr std::int64_t start = 1735732800; // 2025-01-01 12:00:00 UTC
    constexpr std::int64_t hour = 3600;
    acserver::ClockConfig seeded;
    seeded.mode = acserver::ClockMode::Scaled;
    seeded.scale = 4.0;
    seeded.starting_town_unix_seconds = 1893553445;

    // Without the flag a seeded, scaled town keeps its own timeline.
    acserver::TownClock unsynced(seeded, 7);
    CHECK(unsynced.initialize(start));
    CHECK(unsynced.state().town_unix_seconds == seeded.starting_town_unix_seconds);
    CHECK(unsynced.advance(start + hour, true));
    CHECK(unsynced.state().town_unix_seconds == seeded.starting_town_unix_seconds + 4 * hour);

    // With the flag the host clock wins over the start, the mode, and the scale.
    acserver::ClockConfig synced = seeded;
    synced.sync_to_system_clock = true;
    acserver::TownClock clock(synced, 7);
    CHECK(clock.initialize(start));
    CHECK(clock.state().town_unix_seconds == start);
    CHECK(clock.advance(start + hour, true));
    CHECK(clock.state().town_unix_seconds == start + hour);

    // A persisted town resumes on the host clock rather than its stored drift,
    // including the backwards correction after an administrative time change.
    const auto encoded = clock.encode_state();
    CHECK(!encoded.empty());
    acserver::TownClock resumed(synced, 7);
    CHECK(resumed.decode_state(encoded));
    CHECK(resumed.advance(start + 5 * hour, true));
    CHECK(resumed.state().town_unix_seconds == start + 5 * hour);
    CHECK(resumed.set_time(start + 90 * hour, start + 5 * hour, false));
    CHECK(resumed.advance(start + 6 * hour, true));
    CHECK(resumed.state().town_unix_seconds == start + 6 * hour);

    // A backwards host clock still needs allow_time_travel.
    CHECK(resumed.advance(start + 2 * hour, true));
    CHECK(resumed.state().town_unix_seconds == start + 6 * hour);

    // The configured timezone offset still applies to the synced time.
    acserver::ClockConfig zoned;
    zoned.sync_to_system_clock = true;
    zoned.timezone = "America/Winnipeg";
    acserver::TownClock local(zoned, 7);
    CHECK(local.initialize(start));
    CHECK(local.advance(start + hour, true));
    CHECK(local.state().town_unix_seconds == start + hour - 6 * hour);
}

void town_configuration_is_loaded_and_validated() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-config-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::filesystem::create_directories(root);
    const std::filesystem::path path = root / "server.ini";
    {
        std::ofstream output(path);
        output << "[server]\nport = 25001\ncapacity = 12\n"
                  "tick_rate = 60\nsnapshot_rate = 20\nconnection_timeout_ms = 45000\n"
                  "dashboard = false\n\n[town]\ntown_id = 91\ntown_name = Winnipeg\ntown_seed = 42\n"
                  "\n[clock]\ntimezone = America/Winnipeg\nutc_offset_minutes = -360\n"
                  "clock_mode = scaled\nclock_scale = 4.0\nsync_to_system_clock = true\n"
                  "starting_datetime = \"2030-01-02 03:04:05\"\n"
                  "allow_time_travel = true\nempty_town_simulation = scheduled\n"
                  "\n[storage]\ndata_directory = town-data\n"
                  "\n[security]\ninvite_required = true\ninvite_key = \"test secret;not a comment\"\n";
    }
    acserver::TownRuntimeConfig config;
    bool invite_required = true;
    std::string error;
    CHECK(acserver::load_town_config(path, config, invite_required, false, error));
    CHECK(config.town_id == 91);
    CHECK(config.town_name == "Winnipeg");
    CHECK(config.town_seed == 42);
    CHECK(config.port == 25001);
    CHECK(config.capacity == 12);
    CHECK(config.snapshot_rate == 20);
    CHECK(config.connection_timeout_ms == 45000);
    CHECK(!config.dashboard);
    CHECK(config.clock.timezone == "America/Winnipeg");
    CHECK(config.clock.mode == acserver::ClockMode::Scaled);
    CHECK(config.clock.scale == 4.0);
    CHECK(config.clock.sync_to_system_clock);
    CHECK(config.clock.starting_town_unix_seconds == 1893553445);
    CHECK(config.clock.allow_time_travel);
    CHECK(invite_required);
    CHECK(config.invite_key == "test secret;not a comment");
    CHECK(config.data_directory == (root / "town-data").lexically_normal());

    const std::filesystem::path generated = root / "generated" / "server.ini";
    CHECK(acserver::write_default_town_config(generated, config, invite_required, error));
    acserver::TownRuntimeConfig generated_config;
    bool generated_invite_required = false;
    CHECK(acserver::load_town_config(generated, generated_config, generated_invite_required, false, error));
    CHECK(generated_config.town_name == config.town_name);
    CHECK(generated_config.clock.sync_to_system_clock);
    CHECK(generated_config.clock.starting_town_unix_seconds == config.clock.starting_town_unix_seconds);
    CHECK(generated_config.invite_key.empty());
    CHECK(generated_invite_required);

    {
        std::ofstream output(path, std::ios::trunc);
        output << "capacity = 17\n";
    }
    CHECK(!acserver::load_town_config(path, config, invite_required, false, error));
    CHECK(error.find("capacity") != std::string::npos);
}

void client_network_ini_is_loaded_and_validated() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-network-ini-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::filesystem::create_directories(root);
    const std::filesystem::path path = root / "network.ini";
    {
        std::ofstream output(path);
        output << "[connection]\n"
                  "enabled = true\n"
                  "server = [::1]:25000\n"
                  "town_id = 91\n"
                  "account_id = 1002\n"
                  "invite_key = \"secret;value\"\n";
    }
    pc_network_config_t config;
    pc_network_config_defaults(&config);
    int found = 0;
    char error[256]{};
    CHECK(pc_network_config_load(path.string().c_str(), &config, &found, error, sizeof(error)));
    CHECK(found);
    CHECK(config.enabled);
    CHECK(std::string(config.host) == "::1");
    CHECK(config.port == 25000);
    CHECK(config.town_id == 91);
    CHECK(config.account_id == 1002);
    CHECK(std::string(config.invite_key) == "secret;value");

    const std::filesystem::path generated = root / "generated.ini";
    CHECK(pc_network_config_write_default(generated.string().c_str(), error, sizeof(error)));
    pc_network_config_defaults(&config);
    CHECK(pc_network_config_load(generated.string().c_str(), &config, &found, error, sizeof(error)));
    CHECK(found);
    CHECK(!config.enabled);
    CHECK(config.account_id == 1001);

    {
        std::ofstream output(path, std::ios::trunc);
        output << "[connection]\naccount_id = 0\n";
    }
    CHECK(!pc_network_config_load(path.string().c_str(), &config, &found, error, sizeof(error)));
    CHECK(std::string(error).find("line 2") != std::string::npos);
}

void protocol_hello_round_trip() {
    acnet::ClientHello original;
    original.minimum_version = 1;
    original.maximum_version = acnet::kProtocolVersion;
    original.build_id = 0x123456789ABCDEF0ULL;
    original.feature_flags = 0xA5A5;
    original.town = 91;
    original.account = 42;
    original.client_nonce = 0xCAFEBABE;
    original.reconnect_token_size = 7;
    for (std::size_t i = 0; i < original.reconnect_token_size; ++i) {
        original.reconnect_token[i] = static_cast<std::uint8_t>(i + 10);
    }
    original.invite_proof[0] = 0xEF;

    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode(original, payload));
    acnet::ClientHello decoded;
    CHECK(acnet::decode(payload, decoded));
    CHECK(decoded.maximum_version == acnet::kProtocolVersion);
    CHECK(decoded.build_id == original.build_id);
    CHECK(decoded.town == 91);
    CHECK(decoded.account == 42);
    CHECK(decoded.reconnect_token_size == 7);
    CHECK(decoded.reconnect_token[6] == 16);
    CHECK(decoded.invite_proof[0] == 0xEF);
}

void town_bootstrap_messages_are_bounded_and_round_trip() {
    acnet::TownBootstrap original;
    original.town_seed = 42;
    original.land_id = 0x302A;
    original.town_name = {{'W', 'i', 'n', 'n', 'i', 'p', 'e', 'g'}};
    original.appearance.name = {{'R', 'e', 's', 'i', 'd', 'e', 'n', 't'}};
    original.appearance.gender = 1;
    original.appearance.face = 6;
    original.appearance.clothing = 0x2401;
    original.appearance.equipped_item = 0x2001;
    original.tiles.resize(acnet::kTownBootstrapTileCount);
    for (std::size_t i = 0; i < original.tiles.size(); ++i) {
        original.tiles[i].item = static_cast<std::uint16_t>(i & 0x7FFFU);
        original.tiles[i].buried = (i % 17U) == 0;
    }

    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode(original, payload));
    acnet::TownBootstrap decoded;
    CHECK(acnet::decode(payload, decoded));
    CHECK(decoded.town_seed == original.town_seed);
    CHECK(decoded.land_id == original.land_id);
    CHECK(decoded.town_name == original.town_name);
    CHECK(decoded.appearance.name == original.appearance.name);
    CHECK(decoded.appearance.clothing == original.appearance.clothing);
    CHECK(decoded.tiles.size() == acnet::kTownBootstrapTileCount);
    CHECK(decoded.tiles[17].buried);
    CHECK(decoded.tiles[1234].item == original.tiles[1234].item);

    original.tiles.pop_back();
    CHECK(!acnet::encode(original, payload));
    original.tiles.resize(acnet::kTownBootstrapTileCount);
    original.town_seed = 0;
    CHECK(!acnet::encode(original, payload));

    acnet::TownBootstrapResult result;
    result.code = acnet::ResultCode::Ok;
    result.revision = 91;
    result.initialized = true;
    CHECK(acnet::encode(result, payload));
    acnet::TownBootstrapResult decoded_result;
    CHECK(acnet::decode(payload, decoded_result));
    CHECK(decoded_result.code == acnet::ResultCode::Ok);
    CHECK(decoded_result.revision == 91);
    CHECK(decoded_result.initialized);
}

void packet_round_trip_and_corruption() {
    acnet::InputCommand input;
    input.sequence = 99;
    input.estimated_server_tick = 1200;
    input.stick_x = -123;
    input.stick_y = 234;
    input.buttons = 7;
    input.action = 3;
    input.client_transform.position = {1.25F, -2.5F, 9.0F};
    input.client_transform.velocity = {-4.0F, 0.5F, 8.0F};
    input.client_transform.yaw = -1234;
    input.client_transform.action = 9;
    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode(input, payload));

    acnet::PacketHeader header;
    header.message_type = acnet::MessageType::InputCommand;
    header.channel = acnet::Channel::Snapshots;
    header.session = 77;
    header.sequence = 15;
    header.acknowledged_sequence = 14;
    header.acknowledged_bits = 0x55;
    std::vector<std::uint8_t> packet;
    std::string error;
    CHECK(acnet::encode_packet(header, payload, packet, error));

    acnet::DecodedPacket decoded_packet;
    CHECK(acnet::decode_packet(packet.data(), packet.size(), decoded_packet, error));
    CHECK(decoded_packet.header.session == 77);
    CHECK(decoded_packet.header.sequence == 15);
    acnet::InputCommand decoded;
    CHECK(acnet::decode(decoded_packet.payload, decoded));
    CHECK(decoded.sequence == 99);
    CHECK(decoded.stick_x == -123);
    CHECK(same_float(decoded.client_transform.position.y, -2.5F));
    CHECK(same_float(decoded.client_transform.velocity.z, 8.0F));
    CHECK(decoded.client_transform.yaw == -1234);

    packet[20] ^= 0x40;
    CHECK(!acnet::decode_packet(packet.data(), packet.size(), decoded_packet, error));
    CHECK(error == "checksum mismatch");
}

void protocol_rejects_truncated_and_nonfinite() {
    acnet::ClientHello hello;
    hello.town = 1;
    hello.account = 2;
    std::vector<std::uint8_t> bytes;
    CHECK(acnet::encode(hello, bytes));
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + size);
        acnet::ClientHello output;
        CHECK(!acnet::decode(truncated, output));
    }

    acnet::InputCommand input;
    input.client_transform.position.x = std::numeric_limits<float>::infinity();
    CHECK(!acnet::encode(input, bytes));

    acnet::ZoneReadyRequest ready;
    ready.token = {1, 2};
    ready.destination_transform.position.z = std::numeric_limits<float>::quiet_NaN();
    CHECK(!acnet::encode(ready, bytes));

    acnet::TransformSnapshot snapshot;
    snapshot.server_tick = 1;
    for (std::size_t i = 0; i <= acnet::kMaxPlayersPerZone; ++i) {
        acnet::PlayerSnapshot player;
        player.entity = i + 1;
        player.account = i + 10;
        player.zone = 1;
        snapshot.players.push_back(player);
    }
    CHECK(!acnet::encode(snapshot, bytes));
}

void snapshot_round_trip() {
    acnet::TransformSnapshot original;
    original.server_tick = 500;
    original.baseline_revision = 17;
    original.house_light_mask = 0x5;
    for (std::uint64_t i = 0; i < 8; ++i) {
        acnet::PlayerSnapshot player;
        player.entity = 0x100000001ULL + i;
        player.account = 100 + i;
        player.zone = 1;
        player.acknowledged_input = static_cast<std::uint32_t>(50 + i);
        player.transform.position = {static_cast<float>(i), 2.0F, static_cast<float>(i * 3)};
        player.transform.velocity = {0.1F, 0.0F, -0.2F};
        player.transform.yaw = static_cast<std::int16_t>(i * 100);
        if (i == 7) {
            player.transition_phase = acnet::DoorTransitionPhase::Arriving;
            player.transition_door = 202;
            player.transition_expires_tick = 590;
        }
        original.players.push_back(player);
    }
    std::vector<std::uint8_t> bytes;
    CHECK(acnet::encode(original, bytes));
    acnet::TransformSnapshot decoded;
    CHECK(acnet::decode(bytes, decoded));
    CHECK(decoded.players.size() == 8);
    CHECK(decoded.players[7].account == 107);
    CHECK(decoded.house_light_mask == 0x5);
    CHECK(decoded.players[7].transition_phase == acnet::DoorTransitionPhase::Arriving);
    CHECK(decoded.players[7].transition_door == 202);
    CHECK(same_float(decoded.players[7].transform.position.z, 21.0F));
}

void entity_ids_are_stable_and_not_reused() {
    acnet::EntityRegistry entities(19);
    const acnet::EntityId first = entities.add(0x1000, 2, 1, 3);
    const acnet::EntityId second = entities.add(0x2000, 3, 1, 1);
    CHECK(first == (static_cast<std::uint64_t>(19) << 32 | 1));
    CHECK(second != first);
    CHECK(entities.by_key(0x1000)->id == first);
    CHECK(entities.add(0x1000, 4, 1, 0) == 0);
    CHECK(entities.change_zone(first, 10));
    CHECK(entities.by_id(first)->zone == 10);
    CHECK(entities.remove_by_key(0x1000));
    CHECK(entities.by_id(first) == nullptr);
    const acnet::EntityId third = entities.add(0x3000, 2, 1, 0);
    CHECK(third != first);
    CHECK((third & 0xFFFFFFFFULL) == 3);
}

void sessions_negotiate_capacity_and_reconnect() {
    acnet::SessionConfig config;
    config.capacity = 2;
    config.minimum_protocol = 1;
    config.maximum_protocol = 2;
    config.required_build_id = 55;
    config.reconnect_window_ms = 1000;
    acnet::SessionTable sessions(config, 12345);

    acnet::ClientHello first;
    first.minimum_version = 1;
    first.maximum_version = 4;
    first.build_id = 55;
    first.town = 5;
    first.account = 100;
    first.client_nonce = 9;
    acnet::ServerHello accepted = sessions.accept(first, 0x100000001ULL, 10, 1000);
    CHECK(accepted.result == acnet::ResultCode::Ok);
    CHECK(accepted.negotiated_version == 2);
    CHECK(accepted.session != 0);
    CHECK(accepted.reconnect_token_size == acnet::kReconnectTokenBytes);
    CHECK(sessions.active_count() == 1);

    CHECK(sessions.disconnect(accepted.session, 1500));
    CHECK(sessions.active_count() == 0);
    first.reconnect_token = accepted.reconnect_token;
    first.reconnect_token_size = accepted.reconnect_token_size;
    acnet::ServerHello reconnected = sessions.accept(first, 0x999, 11, 2000);
    CHECK(reconnected.result == acnet::ResultCode::Ok);
    CHECK(reconnected.session == accepted.session);
    CHECK(reconnected.player_entity == 0x100000001ULL);

    acnet::ClientHello duplicate = first;
    duplicate.reconnect_token_size = 0;
    CHECK(sessions.accept(duplicate, 0x200, 12, 2100).result == acnet::ResultCode::Conflict);

    CHECK(sessions.disconnect(accepted.session, 3000));
    CHECK(sessions.expire(4001) == 1);
    CHECK(sessions.find(accepted.session) == nullptr);
}

void interpolation_orders_and_extrapolates() {
    acnet::TransformHistory history(3);
    acnet::Transform a;
    a.position = {0.0F, 0.0F, 0.0F};
    a.velocity = {1.0F, 0.0F, 0.0F};
    acnet::Transform b = a;
    b.position.x = 10.0F;
    CHECK(history.push(10, a));
    CHECK(history.push(20, b));
    CHECK(!history.push(19, b));
    const auto midpoint = history.sample(15.0);
    CHECK(midpoint.has_value());
    CHECK(same_float(midpoint->position.x, 5.0F));
    const auto extrapolated = history.sample(25.0, 2.0);
    CHECK(extrapolated.has_value());
    CHECK(same_float(extrapolated->position.x, 12.0F));
}

void selective_reliability_tracks_ack_windows() {
    acnet::ReliabilityPeer sender;
    acnet::ReliabilityPeer receiver;
    std::vector<std::uint8_t> dummy{1, 2, 3};
    const acnet::PacketHeader first =
        sender.make_header(acnet::MessageType::Ping, acnet::Channel::Control, 7);
    const acnet::PacketHeader second =
        sender.make_header(acnet::MessageType::Ping, acnet::Channel::Control, 7);
    const acnet::PacketHeader third =
        sender.make_header(acnet::MessageType::Ping, acnet::Channel::Control, 7);
    sender.track_sent(first, dummy, 100);
    sender.track_sent(second, dummy, 100);
    sender.track_sent(third, dummy, 100);
    CHECK(sender.pending_count() == 3);
    CHECK(receiver.receive(first) == acnet::ReceiveDisposition::New);
    CHECK(receiver.receive(third) == acnet::ReceiveDisposition::New);
    CHECK(receiver.receive(second) == acnet::ReceiveDisposition::New);
    CHECK(receiver.receive(second) == acnet::ReceiveDisposition::Duplicate);

    const acnet::PacketHeader acknowledgement =
        receiver.make_header(acnet::MessageType::Pong, acnet::Channel::Control, 7);
    CHECK(acknowledgement.acknowledged_sequence == third.sequence);
    CHECK((acknowledgement.acknowledged_bits & 3U) == 3U);
    CHECK(sender.receive(acknowledgement) == acnet::ReceiveDisposition::New);
    CHECK(sender.pending_count() == 0);

    const acnet::PacketHeader pending =
        sender.make_header(acnet::MessageType::Ping, acnet::Channel::Transactions, 7);
    sender.track_sent(pending, dummy, 1000);
    CHECK(sender.retransmissions(1050, 100, 2).empty());
    CHECK(sender.retransmissions(1100, 100, 2).size() == 1);
    CHECK(sender.retransmissions(1200, 100, 2).size() == 1);
    CHECK(sender.retransmissions(1300, 100, 2).empty());
    CHECK(sender.pending_count() == 0);
    CHECK(sender.dropped_after_retries() == 1);

    const acnet::PacketHeader snapshot =
        sender.make_header(acnet::MessageType::TransformSnapshot, acnet::Channel::Snapshots, 7);
    sender.track_sent(snapshot, dummy, 2000);
    CHECK(sender.pending_count() == 0);
}

void udp_eight_client_handshake_smoke() {
    std::string error;
    acnet::UdpSocket server;
    CHECK(server.open(0, error));
    CHECK(server.bound_port() != 0);

    constexpr std::size_t client_count = 8;
    std::array<acnet::UdpSocket, client_count> clients;
    std::array<bool, client_count> answered{};
    for (std::size_t i = 0; i < client_count; ++i) {
        CHECK(clients[i].open(0, error));
        acnet::ClientHello hello;
        hello.town = 1;
        hello.account = 1000 + i;
        hello.client_nonce = i + 10;
        std::vector<std::uint8_t> payload;
        CHECK(acnet::encode(hello, payload));
        acnet::PacketHeader header;
        header.message_type = acnet::MessageType::ClientHello;
        header.channel = acnet::Channel::Control;
        header.flags = acnet::PacketReliable;
        header.sequence = 1;
        std::vector<std::uint8_t> packet;
        CHECK(acnet::encode_packet(header, payload, packet, error));
        CHECK(clients[i].send("127.0.0.1", server.bound_port(), packet, error));
    }

    acnet::SessionConfig config;
    config.capacity = client_count;
    acnet::SessionTable sessions(config, 777);
    std::size_t accepted_count = 0;
    std::size_t answered_count = 0;
    for (int attempt = 0; attempt < 2000 && answered_count < client_count; ++attempt) {
        acnet::Datagram incoming;
        while (server.receive(incoming, error)) {
            acnet::DecodedPacket packet;
            CHECK(acnet::decode_packet(incoming.bytes.data(), incoming.bytes.size(), packet, error));
            CHECK(packet.header.message_type == acnet::MessageType::ClientHello);
            acnet::ClientHello hello;
            CHECK(acnet::decode(packet.payload, hello));
            const acnet::EntityId entity = 0x100000001ULL + accepted_count;
            const acnet::ServerHello response = sessions.accept(hello, entity, 50, 1000);
            CHECK(response.result == acnet::ResultCode::Ok);
            std::vector<std::uint8_t> payload;
            CHECK(acnet::encode(response, payload));
            acnet::PacketHeader header;
            header.message_type = acnet::MessageType::ServerHello;
            header.channel = acnet::Channel::Control;
            header.flags = acnet::PacketReliable;
            header.session = response.session;
            header.sequence = 1;
            header.acknowledged_sequence = packet.header.sequence;
            std::vector<std::uint8_t> bytes;
            CHECK(acnet::encode_packet(header, payload, bytes, error));
            CHECK(server.send(incoming.host, incoming.port, bytes, error));
            ++accepted_count;
        }
        CHECK(error.empty());

        for (std::size_t i = 0; i < client_count; ++i) {
            if (answered[i]) continue;
            acnet::Datagram response;
            if (!clients[i].receive(response, error)) {
                CHECK(error.empty());
                continue;
            }
            acnet::DecodedPacket packet;
            CHECK(acnet::decode_packet(response.bytes.data(), response.bytes.size(), packet, error));
            acnet::ServerHello hello;
            CHECK(acnet::decode(packet.payload, hello));
            CHECK(hello.result == acnet::ResultCode::Ok);
            CHECK(hello.session != 0);
            answered[i] = true;
            ++answered_count;
        }
        if (answered_count < client_count) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(accepted_count == client_count);
    CHECK(answered_count == client_count);
    CHECK(sessions.active_count() == client_count);
}

void multiplayer_player_queries_are_scoped() {
    acnet::PlayerDirectory players;
    for (std::uint64_t i = 0; i < 5; ++i) {
        acnet::PlayerView player;
        player.account = 10 + i;
        player.entity = 100 + i;
        player.zone = i == 4 ? 10 : 1;
        player.kind = i < 2 ? acnet::PlayerKind::Resident : acnet::PlayerKind::Visitor;
        player.transform.position = {static_cast<float>(i * 10), 0.0F, 0.0F};
        CHECK(players.upsert(player));
    }
    CHECK(players.set_local(12));
    CHECK(players.local()->account == 12);
    CHECK(players.by_entity(101)->account == 11);
    const acnet::PlayerView* nearest = players.nearest({12.0F, 0.0F, 0.0F}, 1, 100.0F);
    CHECK(nearest != nullptr);
    CHECK(nearest->account == 11);
    const auto radius = players.query_radius({15.0F, 0.0F, 0.0F}, 1, 16.0F, 8);
    CHECK(radius.size() == 4);
    CHECK(radius.front()->account == 11 || radius.front()->account == 12);
    CHECK(players.query_zone(10, 8).size() == 1);
    CHECK(players.remove(12));
    CHECK(players.local() == nullptr);
}

void movement_is_client_authoritative_under_latency() {
    acnet::MovementConfig config;
    config.maximum_speed = 300.0F;
    config.acceleration = 1200.0F;
    config.friction = 1600.0F;
    bool collision_validator_called = false;
    acnet::MovementSimulator server(config, [&](acnet::ZoneId, const acnet::Vec3&, const acnet::Vec3&) {
        collision_validator_called = true;
        return false;
    });
    acnet::Transform initial;
    CHECK(server.add_player(1, 0x100000001ULL, 1, initial));

    struct DelayedInput { int delivery_tick; acnet::InputCommand command; };
    std::deque<DelayedInput> outbound;
    constexpr int one_way_latency_ticks = 12; // 200 ms at 60 Hz

    for (int tick = 0; tick < 900; ++tick) {
        if (tick < 600) {
            acnet::InputCommand command;
            command.sequence = static_cast<std::uint32_t>(tick + 1);
            command.estimated_server_tick = server.current_tick();
            command.stick_x = tick < 480 ? 26000 : 0;
            command.client_transform.position = {
                static_cast<float>(tick) * 1.5F,
                37.0F,
                -static_cast<float>(tick) * 0.75F,
            };
            command.client_transform.velocity = {90.0F, -3.0F, -45.0F};
            command.client_transform.yaw = static_cast<std::int16_t>(-8192);
            command.client_transform.action = 7;
            outbound.push_back({tick + one_way_latency_ticks, command});
        }
        while (!outbound.empty() && outbound.front().delivery_tick <= tick) {
            CHECK(server.submit(1, outbound.front().command) == acnet::ResultCode::Ok);
            outbound.pop_front();
        }
        server.tick();
    }
    const acnet::MovementPlayer* authority = server.player(1);
    CHECK(authority != nullptr);
    CHECK(authority->last_processed_sequence == 600);
    CHECK(same_float(authority->transform.position.x, 599.0F * 1.5F));
    CHECK(same_float(authority->transform.position.y, 37.0F));
    CHECK(same_float(authority->transform.position.z, -599.0F * 0.75F));
    CHECK(same_float(authority->transform.velocity.y, -3.0F));
    CHECK(authority->transform.yaw == -8192);
    CHECK(authority->transform.action == 7);
    CHECK(!collision_validator_called);

    acnet::InputCommand duplicate;
    duplicate.sequence = 600;
    CHECK(server.submit(1, duplicate) == acnet::ResultCode::Conflict);
}

void world_transactions_are_atomic_idempotent_and_conserved() {
    acnet::PlayerDirectory players;
    for (std::uint64_t i = 1; i <= 2; ++i) {
        acnet::PlayerView player;
        player.account = i;
        player.entity = 100 + i;
        player.zone = 1;
        player.transform.position = {20.0F, 0.0F, 20.0F};
        CHECK(players.upsert(player));
    }
    acnet::WorldAuthority world(&players);
    CHECK(world.register_inventory(1));
    CHECK(world.register_inventory(2));
    const acnet::TileAddress address{1, 0, 0};
    acnet::TileState ground;
    ground.item = 500;
    CHECK(world.set_tile(address, ground));
    const acnet::TileAddress unrelated{1, 1, 0};
    CHECK(world.set_tile(unrelated, acnet::TileState{}));
    CHECK(world.total_item_units() == 1);

    acnet::WorldOperation pickup;
    pickup.type = acnet::WorldOpType::PickupItem;
    pickup.account = 1;
    pickup.idempotency = {1, 1};
    pickup.tile = address;
    pickup.expected_tile_revision = 1;
    pickup.expected_inventory_revision = 1;
    pickup.expected_item = 500;
    const acnet::WorldResult accepted = world.apply(pickup);
    CHECK(accepted.code == acnet::ResultCode::Ok);
    CHECK(accepted.tile_revision == 2);
    CHECK(accepted.inventory_revision == 2);
    CHECK(accepted.transferred_item == 500);
    CHECK(world.tile(address)->item == 0);
    CHECK(world.inventory(1)->slots[accepted.inventory_slot].item == 500);
    CHECK(world.total_item_units() == 1);
    CHECK(world.tile(unrelated)->revision == 1);

    const acnet::WorldResult replay = world.apply(pickup);
    CHECK(replay.code == acnet::ResultCode::Ok);
    CHECK(replay.replayed);
    CHECK(world.total_item_units() == 1);

    acnet::WorldOperation contending = pickup;
    contending.account = 2;
    contending.idempotency = {2, 1};
    const acnet::WorldResult rejected = world.apply(contending);
    CHECK(rejected.code == acnet::ResultCode::StaleRevision);
    CHECK(rejected.tile_revision == 2);
    CHECK(world.total_item_units() == 1);

    acnet::WorldOperation drop;
    drop.type = acnet::WorldOpType::DropItem;
    drop.account = 1;
    drop.idempotency = {1, 2};
    drop.tile = address;
    drop.expected_tile_revision = 2;
    drop.expected_inventory_revision = 2;
    drop.inventory_slot = accepted.inventory_slot;
    drop.expected_item = 500;
    CHECK(world.apply(drop).code == acnet::ResultCode::Ok);
    CHECK(world.tile(address)->item == 500);
    CHECK(world.inventory(1)->slots[accepted.inventory_slot].item == 0);
    CHECK(world.total_item_units() == 1);

    bool allow_commit = false;
    world.set_commit_hook([&](const acnet::WorldOperation&, const acnet::WorldResult&,
                              const acnet::TileState&, const acnet::InventoryState&) {
        return allow_commit;
    });
    acnet::WorldOperation failed_pickup = pickup;
    failed_pickup.idempotency = {1, 3};
    failed_pickup.expected_tile_revision = world.tile(address)->revision;
    failed_pickup.expected_inventory_revision = world.inventory(1)->revision;
    CHECK(world.apply(failed_pickup).code == acnet::ResultCode::InternalError);
    CHECK(world.tile(address)->item == 500);
    CHECK(world.total_item_units() == 1);
    allow_commit = true;
    CHECK(world.apply(failed_pickup).code == acnet::ResultCode::Ok);
    CHECK(world.total_item_units() == 1);

    acnet::InventoryState tools = *world.inventory(1);
    tools.slots[1].item = 0x2202;
    tools.slots[2].item = 0x2900;
    CHECK(world.set_inventory(1, tools));
    acnet::WorldOperation dig;
    dig.type = acnet::WorldOpType::Dig;
    dig.account = 1;
    dig.idempotency = {1, 4};
    dig.tile = unrelated;
    dig.expected_tile_revision = 1;
    dig.expected_inventory_revision = tools.revision;
    dig.tool_slot = 1;
    CHECK(world.apply(dig).code == acnet::ResultCode::Ok);
    CHECK(world.tile(unrelated)->terrain == acnet::TerrainState::Hole);
    acnet::WorldOperation fill = dig;
    fill.type = acnet::WorldOpType::FillHole;
    fill.idempotency = {1, 5};
    fill.expected_tile_revision = world.tile(unrelated)->revision;
    CHECK(world.apply(fill).code == acnet::ResultCode::Ok);
    CHECK(world.tile(unrelated)->terrain == acnet::TerrainState::Normal);
    acnet::WorldOperation plant = fill;
    plant.type = acnet::WorldOpType::Plant;
    plant.idempotency = {1, 6};
    plant.expected_tile_revision = world.tile(unrelated)->revision;
    plant.inventory_slot = 2;
    plant.tool_slot = 0xFF;
    plant.expected_item = 0x2900;
    CHECK(world.apply(plant).code == acnet::ResultCode::Ok);
    CHECK(world.tile(unrelated)->terrain == acnet::TerrainState::Planted);
    CHECK(world.tile(unrelated)->item == 0x0800);
}

void economy_and_trade_prevent_value_duplication() {
    acnet::PlayerDirectory players;
    acnet::WorldAuthority world(&players);
    acnet::InventoryState first_inventory;
    first_inventory.bells = 1000;
    first_inventory.slots[0].item = 100;
    first_inventory.slots[1].item = 200;
    acnet::InventoryState second_inventory;
    second_inventory.bells = 100;
    second_inventory.slots[0].item = 300;
    CHECK(world.register_inventory(1, first_inventory));
    CHECK(world.register_inventory(2, second_inventory));

    acnet::EconomyAuthority economy(&world);
    acnet::AccountLedger first_ledger;
    first_ledger.bank_balance = 200;
    first_ledger.debt = 500;
    CHECK(economy.register_account(1, first_ledger));
    CHECK(economy.register_account(2));
    acnet::ShopState shop;
    shop.stock.push_back({400, 250, 1});
    economy.set_shop(shop);
    economy.set_sell_price(100, 50);
    const std::uint64_t initial_items = economy.total_item_units();
    CHECK(initial_items == 4); // three pockets plus one shop item
    const std::uint64_t initial_bells = economy.total_bells();
    CHECK(initial_bells == 1300);

    acnet::EconomyRequest buy;
    buy.type = acnet::EconomyOpType::Buy;
    buy.account = 1;
    buy.idempotency = {10, 1};
    buy.expected_inventory_revision = 1;
    buy.expected_aux_revision = 1;
    const acnet::EconomyResult bought = economy.apply(buy);
    CHECK(bought.code == acnet::ResultCode::Ok);
    CHECK(bought.item == 400);
    CHECK(bought.bells == 750);
    CHECK(economy.shop().stock[0].quantity == 0);
    CHECK(economy.total_item_units() == initial_items);
    const acnet::EconomyResult buy_replay = economy.apply(buy);
    CHECK(buy_replay.replayed);
    CHECK(buy_replay.bells == 750);

    acnet::EconomyRequest sell;
    sell.type = acnet::EconomyOpType::Sell;
    sell.account = 1;
    sell.idempotency = {10, 2};
    sell.expected_inventory_revision = bought.inventory_revision;
    sell.inventory_slot = 0;
    sell.expected_item = 100;
    const acnet::EconomyResult sold = economy.apply(sell);
    CHECK(sold.code == acnet::ResultCode::Ok);
    CHECK(sold.bells == 800);
    CHECK(economy.total_item_units() == initial_items - 1); // sale is an explicit item sink

    acnet::EconomyRequest deposit;
    deposit.type = acnet::EconomyOpType::Deposit;
    deposit.account = 1;
    deposit.idempotency = {10, 3};
    deposit.expected_inventory_revision = sold.inventory_revision;
    deposit.expected_aux_revision = 1;
    deposit.amount = 300;
    const acnet::EconomyResult deposited = economy.apply(deposit);
    CHECK(deposited.code == acnet::ResultCode::Ok);
    CHECK(deposited.bells == 500);
    CHECK(deposited.balance == 500);
    CHECK(economy.total_bells() == initial_bells - 250 + 50); // purchase sink, sale source

    acnet::EconomyRequest donate;
    donate.type = acnet::EconomyOpType::Donate;
    donate.account = 1;
    donate.idempotency = {10, 4};
    donate.expected_inventory_revision = deposited.inventory_revision;
    donate.expected_aux_revision = 1;
    donate.inventory_slot = 1;
    donate.expected_item = 200;
    const acnet::EconomyResult donated = economy.apply(donate);
    CHECK(donated.code == acnet::ResultCode::Ok);
    CHECK(economy.museum().donated_items.count(200) == 1);
    CHECK(economy.total_item_units() == initial_items - 1);

    acnet::EconomyRequest attach;
    attach.type = acnet::EconomyOpType::AttachMail;
    attach.account = 1;
    attach.idempotency = {10, 5};
    attach.expected_inventory_revision = donated.inventory_revision;
    attach.inventory_slot = bought.inventory_slot;
    attach.expected_item = 400;
    attach.recipient = 2;
    const acnet::EconomyResult attached = economy.apply(attach);
    CHECK(attached.code == acnet::ResultCode::Ok);
    CHECK(attached.mail_id != 0);
    CHECK(economy.mail(attached.mail_id)->attachment == 400);
    CHECK(economy.total_item_units() == initial_items - 1);

    acnet::InventoryState trade_first = *world.inventory(1);
    acnet::InventoryState trade_second = *world.inventory(2);
    trade_first.revision = 20;
    trade_second.revision = 30;
    trade_first.slots[0].item = 111;
    trade_second.slots[0].item = 222;
    CHECK(world.set_inventory(1, trade_first));
    CHECK(world.set_inventory(2, trade_second));
    const std::uint64_t before_trade = economy.total_item_units();
    CHECK(economy.create_trade(900, 1, 2).code == acnet::ResultCode::Ok);
    const acnet::TradeResult first_offer = economy.update_trade_offer(900, 1, 1, {0});
    CHECK(first_offer.code == acnet::ResultCode::Ok);
    const acnet::TradeResult second_offer = economy.update_trade_offer(900, 2, first_offer.trade_revision, {0});
    CHECK(second_offer.code == acnet::ResultCode::Ok);
    CHECK(!economy.confirm_trade(900, 1, second_offer.trade_revision).finalized);
    const acnet::TradeResult finalized = economy.confirm_trade(900, 2, second_offer.trade_revision);
    CHECK(finalized.code == acnet::ResultCode::Ok);
    CHECK(finalized.finalized);
    CHECK(world.inventory(1)->slots[0].item == 222);
    CHECK(world.inventory(2)->slots[0].item == 111);
    CHECK(economy.total_item_units() == before_trade);
    CHECK(economy.confirm_trade(900, 2, finalized.trade_revision).finalized);
    CHECK(economy.total_item_units() == before_trade);

    CHECK(economy.create_trade(901, 1, 2).code == acnet::ResultCode::Ok);
    const auto offer_a = economy.update_trade_offer(901, 1, 1, {0});
    const auto offer_b = economy.update_trade_offer(901, 2, offer_a.trade_revision, {0});
    acnet::InventoryState externally_changed = *world.inventory(1);
    ++externally_changed.revision;
    CHECK(world.set_inventory(1, externally_changed));
    CHECK(economy.confirm_trade(901, 1, offer_b.trade_revision).code == acnet::ResultCode::StaleRevision);
    CHECK(economy.cancel_trade(901));
}

void npc_leases_scope_conversations_and_disconnects() {
    acnet::PlayerDirectory players;
    acnet::PlayerView first;
    first.account = 1;
    first.entity = 101;
    first.zone = 1;
    first.transform.position = {10.0F, 0.0F, 10.0F};
    CHECK(players.upsert(first));
    acnet::PlayerView second = first;
    second.account = 2;
    second.entity = 102;
    second.transform.position = {20.0F, 0.0F, 10.0F};
    CHECK(players.upsert(second));

    acnet::NpcConfig config;
    config.interaction_radius = 60.0F;
    config.lease_duration_ticks = 30;
    acnet::NpcAuthority npcs(&players, config);
    acnet::NpcState npc_a;
    npc_a.entity = 1001;
    npc_a.zone = 1;
    npc_a.transform.position = {15.0F, 0.0F, 10.0F};
    CHECK(npcs.add_npc(npc_a));
    acnet::NpcState npc_b = npc_a;
    npc_b.entity = 1002;
    npc_b.transform.position.x = 25.0F;
    CHECK(npcs.add_npc(npc_b));

    const acnet::ConversationResult lease_a = npcs.request_conversation(1, 1001, 10);
    CHECK(lease_a.code == acnet::ResultCode::Ok);
    CHECK(lease_a.lease_id != 0);
    CHECK(npcs.request_conversation(2, 1001, 11).code == acnet::ResultCode::Conflict);
    const acnet::ConversationResult lease_b = npcs.request_conversation(2, 1002, 11);
    CHECK(lease_b.code == acnet::ResultCode::Ok); // separate villagers can converse concurrently
    CHECK(npcs.advance_conversation(2, 1001, lease_a.lease_id, 0, 12).code ==
          acnet::ResultCode::Unauthorized);

    npcs.set_dialogue_resolver([](acnet::EntityId, acnet::AccountId, std::uint32_t node,
                                  std::uint16_t choice) -> std::optional<std::uint32_t> {
        if (choice > 2) return std::nullopt;
        return choice == 2 ? 0U : node + 1;
    });
    const auto advanced = npcs.advance_conversation(1, 1001, lease_a.lease_id, 0, 12);
    CHECK(advanced.code == acnet::ResultCode::Ok);
    CHECK(advanced.dialogue_node == 2);
    CHECK(!advanced.completed);
    const auto completed = npcs.advance_conversation(1, 1001, lease_a.lease_id, 2, 13);
    CHECK(completed.code == acnet::ResultCode::Ok);
    CHECK(completed.completed);
    CHECK(!npcs.conversation(1001)->active);

    CHECK(npcs.release_player(2) == 1);
    CHECK(!npcs.conversation(1002)->active);
    CHECK(npcs.request_conversation(1, 1002, 20).code == acnet::ResultCode::Ok);
    CHECK(npcs.expire(51) == 1);

    CHECK(npcs.acquire_event(7, 1, 100, 20));
    CHECK(!npcs.acquire_event(8, 2, 101, 20));
    CHECK(npcs.acquire_event(8, 2, 121, 20));
    CHECK(npcs.release_event(8, 2));
    CHECK(npcs.nearest_player(1001, 100.0F)->account == 1);
    CHECK(npcs.zone_snapshot(1).size() == 2);
}

void zone_handoffs_and_four_resident_housing_are_safe() {
    acnet::PlayerDirectory players;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        acnet::PlayerView player;
        player.account = i;
        player.entity = 100 + i;
        player.zone = 1;
        player.transform.position = {0.0F, 0.0F, 0.0F};
        CHECK(players.upsert(player));
    }
    acnet::ZoneConfig zone_config;
    zone_config.transfer_timeout_ticks = 30;
    zone_config.sleep_after_ticks = 10;
    acnet::ZoneCoordinator zones(&players, zone_config, 123);
    acnet::ZoneState exterior;
    exterior.id = 1;
    exterior.kind = acnet::ZoneKind::Exterior;
    exterior.capacity = 8;
    CHECK(zones.add_zone(exterior));
    acnet::ZoneState shop;
    shop.id = 10;
    shop.kind = acnet::ZoneKind::PublicInterior;
    shop.capacity = 1;
    CHECK(zones.add_zone(shop));
    CHECK(zones.join(1, 1, {100.0F, 0.0F, 100.0F}, 1));
    CHECK(zones.join(2, 1, {100.0F, 0.0F, 100.0F}, 1));
    acnet::DoorDefinition enter;
    enter.id = 1;
    enter.source_zone = 1;
    enter.destination_zone = 10;
    enter.source_position = {100.0F, 0.0F, 100.0F};
    enter.destination_position = {20.0F, 0.0F, 20.0F};
    CHECK(zones.add_door(enter));
    acnet::DoorDefinition leave = enter;
    leave.id = 2;
    leave.source_zone = 10;
    leave.destination_zone = 1;
    leave.source_position = enter.destination_position;
    leave.destination_position = enter.source_position;
    CHECK(zones.add_door(leave));

    /* Door animations and collision are owned by the original client. A
     * stale/fallback server door coordinate must not disconnect a valid scene
     * handoff from the correct source zone. */
    players.by_account(1)->transform.position = {9000.0F, 0.0F, -9000.0F};
    const acnet::TransferOffer first_offer = zones.request_transfer(1, 1, 2);
    CHECK(first_offer.code == acnet::ResultCode::Ok);
    CHECK(first_offer.token.valid());
    CHECK(zones.request_transfer(2, 1, 2).code == acnet::ResultCode::Capacity);
    acnet::TransferToken wrong = first_offer.token;
    ++wrong.low;
    CHECK(zones.acknowledge_ready(1, wrong, 3) == acnet::ResultCode::Unauthorized);
    CHECK(players.by_account(1)->zone == 1);
    CHECK(zones.acknowledge_ready(1, first_offer.token, 3) == acnet::ResultCode::Ok);
    CHECK(players.by_account(1)->zone == 10);
    CHECK(zones.zone(10)->occupants.count(1) == 1);

    const auto return_offer = zones.request_transfer(1, 2, 4);
    CHECK(return_offer.code == acnet::ResultCode::Ok);
    CHECK(zones.acknowledge_ready(1, return_offer.token, 5) == acnet::ResultCode::Ok);
    CHECK(players.by_account(1)->zone == 1);
    const auto expiring = zones.request_transfer(2, 1, 6);
    CHECK(expiring.code == acnet::ResultCode::Ok);
    CHECK(zones.expire(37) == 1);
    CHECK(players.by_account(2)->zone == 1);
    zones.update_sleep_states(50);
    CHECK(zones.zone(10)->runtime == acnet::ZoneRuntimeState::Sleeping);

    acnet::WorldAuthority world(&players);
    for (std::uint64_t account = 1; account <= 5; ++account) {
        acnet::InventoryState inventory;
        if (account == 1) {
            inventory.slots[0].item = 0x1100;
            inventory.slots[1].item = 0x1200;
        }
        CHECK(world.register_inventory(account, inventory));
    }
    acnet::HousingAuthority housing(&world);
    CHECK(housing.register_resident(0, 1, 10000));
    CHECK(housing.register_resident(1, 2, 10001));
    CHECK(housing.register_resident(2, 3, 10002));
    CHECK(housing.register_resident(3, 4, 10003));
    CHECK(!housing.register_resident(4, 5, 10004));
    CHECK(housing.resident_count() == acnet::kOriginalResidentSlots);

    acnet::HouseUpdate initial_house;
    initial_house.account = 1;
    initial_house.idempotency = {50, 10};
    initial_house.house_id = 10000;
    initial_house.expected_house_revision = 1;
    initial_house.upgrade_level = 2;
    const auto initialized = housing.replace_contents(initial_house);
    CHECK(initialized.code == acnet::ResultCode::Ok);

    acnet::FurnitureOperation place;
    place.type = acnet::FurnitureOpType::Place;
    place.account = 1;
    place.idempotency = {50, 1};
    place.house_id = 10000;
    place.address = {2, 3, 0};
    place.expected_house_revision = initialized.house_revision;
    place.expected_inventory_revision = 1;
    place.inventory_slot = 0;
    place.expected_item = 0x1100;
    const auto placed = housing.apply(place);
    CHECK(placed.code == acnet::ResultCode::Ok);
    CHECK(housing.house(10000)->furniture.at(place.address).item == 0x1100);
    CHECK(world.inventory(1)->slots[0].item == 0);
    CHECK(world.total_item_units() + housing.total_furniture_units() == 2);

    CHECK(housing.apply(place).replayed);

    acnet::FurnitureOperation remove = place;
    remove.type = acnet::FurnitureOpType::Remove;
    remove.idempotency = {50, 2};
    remove.expected_house_revision = placed.house_revision;
    remove.expected_inventory_revision = placed.inventory_revision;
    const auto removed = housing.apply(remove);
    CHECK(removed.code == acnet::ResultCode::Ok);
    CHECK(housing.house(10000)->furniture.empty());
    CHECK(world.inventory(1)->slots[removed.inventory_slot].item == 0x1100);
    CHECK(world.total_item_units() + housing.total_furniture_units() == 2);

    acnet::HouseUpdate house_update;
    house_update.account = 1;
    house_update.idempotency = {50, 4};
    house_update.house_id = 10000;
    house_update.expected_house_revision = housing.house(10000)->revision;
    house_update.upgrade_level = 2;
    house_update.main_light_on = true;
    house_update.music_tracks[0] = 87;
    house_update.furniture_switches[0] = 3;
    house_update.furniture[{7, 8, 1, 0}] = {0x1200, 2};
    const auto updated = housing.replace_contents(house_update);
    CHECK(updated.code == acnet::ResultCode::Ok);
    CHECK(housing.house(10000)->initialized);
    CHECK(housing.house(10000)->main_light_on);
    CHECK(housing.house(10000)->music_tracks[0] == 87);
    CHECK(housing.house(10000)->furniture_switches[0] == 3);
    CHECK(housing.house(10000)->furniture.at({7, 8, 1, 0}).item == 0x1200);
    CHECK(housing.replace_contents(house_update).replayed);
    CHECK(world.total_item_units() + housing.total_furniture_units() == 2);

    acnet::HouseUpdate rearranged = house_update;
    rearranged.idempotency = {50, 5};
    rearranged.expected_house_revision = updated.house_revision;
    rearranged.furniture.clear();
    rearranged.furniture[{8, 7, 1, 0}] = {0x1200, 1};
    const acnet::Revision inventory_before_rearrange = world.inventory(1)->revision;
    const auto rearranged_result = housing.replace_contents(rearranged);
    CHECK(rearranged_result.code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->revision == inventory_before_rearrange);
    CHECK(world.inventory(1)->slots[removed.inventory_slot].item == 0x1100);

    acnet::HouseUpdate add_from_inventory = rearranged;
    add_from_inventory.idempotency = {50, 6};
    add_from_inventory.expected_house_revision = rearranged_result.house_revision;
    add_from_inventory.furniture[{4, 5, 0, 0}] = {0x1100, 0};
    const auto added = housing.replace_contents(add_from_inventory);
    CHECK(added.code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->slots[removed.inventory_slot].item == 0);
    CHECK(world.total_item_units() + housing.total_furniture_units() == 2);

    acnet::HouseUpdate forged = add_from_inventory;
    forged.idempotency = {50, 7};
    forged.expected_house_revision = added.house_revision;
    forged.furniture[{6, 6, 0, 0}] = {0x1300, 0};
    CHECK(housing.replace_contents(forged).code == acnet::ResultCode::InvalidState);
    CHECK(housing.house(10000)->revision == added.house_revision);
    CHECK(world.total_item_units() + housing.total_furniture_units() == 2);

    acnet::HouseUpdate remove_to_inventory = add_from_inventory;
    remove_to_inventory.idempotency = {50, 8};
    remove_to_inventory.expected_house_revision = added.house_revision;
    remove_to_inventory.furniture.erase({8, 7, 1, 0});
    const auto returned = housing.replace_contents(remove_to_inventory);
    CHECK(returned.code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->slots[0].item == 0x1200);
    CHECK(world.total_item_units() + housing.total_furniture_units() == 2);

    acnet::HouseUpdate free_upgrade = remove_to_inventory;
    free_upgrade.idempotency = {50, 9};
    free_upgrade.expected_house_revision = returned.house_revision;
    free_upgrade.upgrade_level = 3;
    CHECK(housing.replace_contents(free_upgrade).code == acnet::ResultCode::InvalidState);

    acnet::FurnitureOperation visitor = place;
    visitor.account = 5;
    visitor.idempotency = {50, 3};
    visitor.expected_house_revision = housing.house(10000)->revision;
    visitor.expected_inventory_revision = world.inventory(5)->revision;
    CHECK(housing.apply(visitor).code == acnet::ResultCode::Unauthorized);
}

void persistence_recovers_checkpoints_journal_and_gci() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-netcode-persistence-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};

    acserver::PersistenceConfig config;
    config.retained_checkpoints = 2;
    acserver::PersistenceStore store(root / "town", config);
    std::string error;
    CHECK(store.initialize(error));
    CHECK(!store.previous_shutdown_was_clean());
    CHECK(store.append({1, 1, {1, 2, 3}}, error));
    CHECK(store.append({2, 2, {4, 5}}, error));
    CHECK(!store.append({4, 2, {9}}, error));
    CHECK(store.write_checkpoint(1, {10, 11, 12}, error));
    CHECK(store.write_checkpoint(2, {20, 21}, error));
    CHECK(store.append({3, 3, {6, 7, 8, 9}}, error));
    CHECK(store.write_checkpoint(3, {30, 31, 32}, error));

    std::size_t checkpoint_count = 0;
    for (const auto& ignored : std::filesystem::directory_iterator(root / "town" / "snapshots")) {
        (void)ignored;
        ++checkpoint_count;
    }
    CHECK(checkpoint_count == 2);
    auto latest = store.load_latest_checkpoint(error);
    CHECK(latest.has_value());
    CHECK(latest->sequence == 3);
    CHECK(latest->payload == std::vector<std::uint8_t>({30, 31, 32}));

    std::vector<std::uint64_t> replayed;
    acserver::ReplayReport report;
    CHECK(store.replay(1, [&](const acserver::JournalRecord& record) {
        replayed.push_back(record.sequence);
        return true;
    }, report, error));
    CHECK(replayed == std::vector<std::uint64_t>({2, 3}));
    CHECK(report.ignored_before_checkpoint == 1);
    CHECK(!report.truncated_tail);

    const std::filesystem::path import = root / "input.gci";
    std::vector<std::uint8_t> gci_bytes(0x40 + 0x2000, 0x5A);
    gci_bytes[0] = 'G'; gci_bytes[1] = 'A'; gci_bytes[2] = 'F'; gci_bytes[3] = 'E';
    gci_bytes[4] = '0'; gci_bytes[5] = '1';
    gci_bytes[0x38] = 0; gci_bytes[0x39] = 1;
    {
        std::ofstream output(import, std::ios::binary);
        output.write(reinterpret_cast<const char*>(gci_bytes.data()),
                     static_cast<std::streamsize>(gci_bytes.size()));
    }
    CHECK(store.import_gci(import, error));
    const std::filesystem::path exported = root / "exported.gci";
    CHECK(store.export_gci(exported, error));
    std::vector<std::uint8_t> exported_bytes(gci_bytes.size());
    {
        std::ifstream input(exported, std::ios::binary);
        input.read(reinterpret_cast<char*>(exported_bytes.data()),
                   static_cast<std::streamsize>(exported_bytes.size()));
    }
    CHECK(exported_bytes == gci_bytes);
    CHECK(store.mark_clean_shutdown(error));

    acserver::PersistenceStore restarted(root / "town", config);
    CHECK(restarted.initialize(error));
    CHECK(restarted.previous_shutdown_was_clean());
    CHECK(restarted.last_sequence() == 3);

    // A torn final journal write preserves every complete record.
    {
        std::ofstream journal(restarted.journal_path(), std::ios::binary | std::ios::app);
        const std::array<char, 5> torn{{'A', 'C', 'J', 'R', '\0'}};
        journal.write(torn.data(), static_cast<std::streamsize>(torn.size()));
    }
    replayed.clear();
    CHECK(restarted.replay(0, [&](const acserver::JournalRecord& record) {
        replayed.push_back(record.sequence);
        return true;
    }, report, error));
    CHECK(report.truncated_tail);
    CHECK(replayed == std::vector<std::uint64_t>({1, 2, 3}));

    // Startup removes the torn bytes before accepting new records, so a
    // subsequent restart cannot encounter valid data after corrupt garbage.
    acserver::PersistenceStore recovered(root / "town", config);
    CHECK(recovered.initialize(error));
    CHECK(recovered.last_sequence() == 3);
    CHECK(recovered.append({4, 4, {10, 20, 30}}, error));
    acserver::PersistenceStore recovered_again(root / "town", config);
    CHECK(recovered_again.initialize(error));
    replayed.clear();
    CHECK(recovered_again.replay(0, [&](const acserver::JournalRecord& record) {
        replayed.push_back(record.sequence);
        return true;
    }, report, error));
    CHECK(!report.truncated_tail);
    CHECK(replayed == std::vector<std::uint64_t>({1, 2, 3, 4}));

    // Corrupting the newest checkpoint falls back to the previous valid one.
    latest = restarted.load_latest_checkpoint(error);
    CHECK(latest.has_value() && latest->sequence == 3);
    {
        std::fstream checkpoint(latest->path, std::ios::binary | std::ios::in | std::ios::out);
        checkpoint.seekp(25);
        char byte = 0x7F;
        checkpoint.write(&byte, 1);
    }
    latest = restarted.load_latest_checkpoint(error);
    CHECK(latest.has_value());
    CHECK(latest->sequence == 2);
}

void gci_semantic_conversion_preserves_native_save() {
    constexpr std::size_t main = 0x40 + 0x26000;
    constexpr std::size_t backup = 0x40 + 0x4C000;
    std::vector<std::uint8_t> bytes(0x40 + 0x72000, 0);
    bytes[0] = 'G'; bytes[1] = 'A'; bytes[2] = 'F'; bytes[3] = 'E';
    bytes[4] = '0'; bytes[5] = '1';
    bytes[0x38] = 0; bytes[0x39] = 57;
    bytes[main + 0x10000] = 0xA5;
    bytes[main + 0x20 + 0x1086] = 1;
    bytes[main + 0x20 + 0x68] = 0x12;
    bytes[main + 0x20 + 0x69] = 0x34;
    bytes[main + 0x20 + 0x8F] = 99;
    bytes[main + 0x137A8] = 0x45;
    bytes[main + 0x137A9] = 0x67;
    bytes[main + 0x20F1C] = 0x00;
    bytes[main + 0x20F1D] = 0x01;
    bytes[main + 0x20F19] = 0x21;

    acserver::GciTownState state;
    std::string error;
    CHECK(acserver::decode_gci_town(bytes, state, error));
    CHECK(state.tiles.size() == 5U * 6U * 16U * 16U);
    CHECK(state.tiles.front().first.x == 16 && state.tiles.front().first.z == 16);
    CHECK(state.tiles.front().second.item == 0x4567);
    CHECK(state.tiles.front().second.buried);
    CHECK(state.residents[0].exists);
    CHECK(state.residents[0].inventory.slots[0].item == 0x1234);
    CHECK(state.residents[0].inventory.bells == 99);
    CHECK(state.weather == 1 && state.weather_intensity == 2);

    state.tiles.front().second.item = 0x7788;
    state.tiles.front().second.buried = false;
    state.residents[0].inventory.slots[0] = {0x2345, 2};
    state.residents[0].inventory.bells = 123456;
    state.residents[0].ledger.debt = 54321;
    state.residents[0].ledger.bank_balance = 7654321;
    state.weather = 3;
    state.weather_intensity = 4;
    CHECK(acserver::encode_gci_town(bytes, state, error));
    CHECK(bytes[main + 0x10000] == 0xA5);
    CHECK(bytes[main + 0x137A8] == 0x77 && bytes[main + 0x137A9] == 0x88);
    CHECK(bytes[main + 0x20F1C] == 0 && bytes[main + 0x20F1D] == 0);
    CHECK(bytes[main + 0x20 + 0x68] == 0x23 && bytes[main + 0x20 + 0x69] == 0x45);
    CHECK(bytes[main + 0x20F19] == 0x43);
    CHECK(std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(main),
                     bytes.begin() + static_cast<std::ptrdiff_t>(main + 0x26000),
                     bytes.begin() + static_cast<std::ptrdiff_t>(backup)));
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset < 0x242A0; offset += 2) {
        sum += (static_cast<std::uint16_t>(bytes[main + offset]) << 8) | bytes[main + offset + 1];
    }
    CHECK((sum & 0xFFFFU) == 0);

    acserver::GciTownState round_trip;
    CHECK(acserver::decode_gci_town(bytes, round_trip, error));
    CHECK(round_trip.tiles.front().second.item == 0x7788);
    CHECK(round_trip.residents[0].inventory.bells == 123456);
    CHECK(round_trip.residents[0].ledger.bank_balance == 7654321);
}

void clock_jobs_and_replication_survive_empty_time() {
    constexpr std::int64_t day = 24 * 60 * 60;
    constexpr std::int64_t start = 1700000000;
    acserver::ClockConfig config;
    config.timezone = "America/Winnipeg";
    config.utc_offset_minutes = -360;
    config.mode = acserver::ClockMode::Realtime;
    acserver::TownClock clock(config, 42);
    CHECK(clock.initialize(start));
    const std::int64_t first_town_time = clock.state().town_unix_seconds;
    std::vector<std::int64_t> daily_runs;
    acserver::ScheduledJob daily;
    daily.name = "daily-renewal";
    daily.next_due = first_town_time + day;
    daily.interval_seconds = day;
    daily.maximum_catchups = 64;
    CHECK(clock.add_job(daily, [&](const acserver::ScheduledJob&, std::int64_t due) {
        daily_runs.push_back(due);
        return true;
    }));
    CHECK(clock.advance(start + 31 * day, true));
    CHECK(daily_runs.size() == 31);
    CHECK(clock.job("daily-renewal")->run_count == 31);
    const auto encoded_clock = clock.encode_state();
    CHECK(!encoded_clock.empty());
    acserver::TownClock restored(config, 99);
    CHECK(restored.decode_state(encoded_clock));
    CHECK(restored.state().town_unix_seconds == clock.state().town_unix_seconds);
    const auto before_backwards_wall = restored.state().town_unix_seconds;
    CHECK(restored.advance(start, true));
    CHECK(restored.state().town_unix_seconds == before_backwards_wall);
    CHECK(!restored.set_time(before_backwards_wall + day, start + 32 * day, true));

    acnet::PlayerDirectory players;
    acnet::PlayerView player;
    player.account = 1;
    player.entity = 101;
    player.zone = 1;
    player.transform.position = {20.0F, 0.0F, 20.0F};
    CHECK(players.upsert(player));
    acnet::WorldAuthority world(&players);
    CHECK(world.register_inventory(1));
    acnet::TileState tile;
    tile.item = 777;
    CHECK(world.set_tile({1, 0, 0}, tile));
    acnet::NpcAuthority npcs(&players);
    acnet::NpcState npc;
    npc.entity = 501;
    npc.zone = 1;
    npc.transform.position = {100.0F, 0.0F, 100.0F};
    CHECK(npcs.add_npc(npc));
    auto baseline = acnet::build_baseline(
        1, 600, 1, clock.state().town_unix_seconds, static_cast<std::uint8_t>(clock.state().weather),
        clock.state().weather_intensity, world, players, npcs);
    baseline.town_population = 3;
    baseline.town_capacity = 16;
    std::vector<std::uint8_t> baseline_bytes;
    CHECK(acnet::encode_baseline(baseline, baseline_bytes));
    acnet::ZoneBaseline decoded;
    CHECK(acnet::decode_baseline(baseline_bytes, decoded));
    CHECK(decoded.zone == 1);
    CHECK(decoded.tiles.size() == 1);
    CHECK(decoded.tiles[0].second.item == 777);
    CHECK(decoded.players.size() == 1);
    CHECK(decoded.npcs.size() == 1);
    /* Town-wide occupancy travels with the baseline and is independent of the
     * interest set above (one visible player, three in town). */
    CHECK(decoded.town_population == 3);
    CHECK(decoded.town_capacity == 16);
    baseline_bytes.pop_back();
    CHECK(!acnet::decode_baseline(baseline_bytes, decoded));

    /* A population that exceeds capacity, or a zero capacity, is nonsense the
     * codec must refuse in both directions rather than replicate. */
    acnet::ZoneBaseline invalid = baseline;
    invalid.town_population = 17;
    invalid.town_capacity = 16;
    std::vector<std::uint8_t> invalid_bytes;
    CHECK(!acnet::encode_baseline(invalid, invalid_bytes));
    invalid.town_population = 0;
    invalid.town_capacity = 0;
    CHECK(!acnet::encode_baseline(invalid, invalid_bytes));

    acnet::TownOccupancy occupancy;
    occupancy.population = 4;
    occupancy.capacity = 16;
    std::vector<std::uint8_t> occupancy_bytes;
    CHECK(acnet::encode_town_delta(occupancy, occupancy_bytes));
    CHECK(occupancy_bytes.size() == 2);
    acnet::TownOccupancy decoded_occupancy;
    CHECK(acnet::decode_town_delta(occupancy_bytes, decoded_occupancy));
    CHECK(decoded_occupancy.population == 4);
    CHECK(decoded_occupancy.capacity == 16);
    CHECK(!acnet::decode_town_delta({5, 4}, decoded_occupancy));  /* population > capacity */
    CHECK(!acnet::decode_town_delta({1, 0}, decoded_occupancy));  /* zero capacity */
    CHECK(!acnet::decode_town_delta({1}, decoded_occupancy));     /* truncated */

    acnet::ReplicationDelta town_delta;
    town_delta.kind = acnet::ResourceKind::Town;
    town_delta.zone = 0;
    town_delta.reliable = true;
    CHECK(acnet::encode_town_delta(occupancy, town_delta.payload));
    std::vector<acnet::ReplicationDelta> town_batch{town_delta};
    std::vector<std::uint8_t> town_batch_bytes;
    CHECK(acnet::encode_deltas(town_batch, town_batch_bytes));
    std::vector<acnet::ReplicationDelta> town_batch_decoded;
    CHECK(acnet::decode_deltas(town_batch_bytes, town_batch_decoded));
    CHECK(town_batch_decoded.size() == 1);
    CHECK(town_batch_decoded[0].kind == acnet::ResourceKind::Town);

    /* Town occupancy is town-wide, so it must reach a viewer whose zone does
     * not match the delta's - the check zone-scoped kinds fail. */
    acnet::DeltaLog town_log(4);
    town_log.append(town_delta);
    acnet::InterestContext other_zone;
    other_zone.account = 4242;
    other_zone.zone = 9;
    const auto town_visible = town_log.since(0, other_zone, 20);
    CHECK(town_visible.deltas.size() == 1);
    CHECK(town_visible.deltas[0].kind == acnet::ResourceKind::Town);

    acnet::DeltaLog deltas(4);
    acnet::ReplicationDelta clock_delta;
    clock_delta.kind = acnet::ResourceKind::Clock;
    clock_delta.reliable = true;
    deltas.append(clock_delta);
    acnet::ReplicationDelta near_player;
    near_player.kind = acnet::ResourceKind::Player;
    near_player.zone = 1;
    near_player.entity = 10;
    near_player.reliable = false;
    near_player.has_position = true;
    near_player.position = {100.0F, 0.0F, 0.0F};
    deltas.append(near_player);
    acnet::ReplicationDelta far_player = near_player;
    far_player.entity = 11;
    far_player.position = {5000.0F, 0.0F, 0.0F};
    deltas.append(far_player);
    acnet::ReplicationDelta targeted;
    targeted.kind = acnet::ResourceKind::House;
    targeted.zone = 10;
    targeted.target_account = 2;
    deltas.append(targeted);

    acnet::InterestContext interest;
    interest.account = 1;
    interest.zone = 1;
    interest.position = {};
    interest.exterior = true;
    interest.radius = 500.0F;
    const auto relevant = deltas.since(0, interest, 20);
    CHECK(!relevant.requires_baseline);
    CHECK(relevant.deltas.size() == 2); // global clock plus nearby transform
    CHECK(relevant.deltas[0].kind == acnet::ResourceKind::Clock);
    CHECK(relevant.deltas[1].entity == 10);

    for (int i = 0; i < 3; ++i) deltas.append(clock_delta);
    CHECK(deltas.size() == 4);
    CHECK(deltas.since(1, interest, 20).requires_baseline);
}

void real_runtime_serves_eight_moving_bots() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-netcode-runtime-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};

    acserver::TownRuntimeConfig config;
    config.port = 0;
    config.data_directory = root / "town";
    config.capacity = 8;
    config.connection_timeout_ms = 60000;
    config.allow_unauthenticated = true;
    acserver::TownRuntime runtime(config);
    std::string error;
    constexpr std::int64_t wall_start = 1700000000;
    CHECK(runtime.initialize(wall_start, error));

    struct Bot {
        acnet::UdpSocket socket;
        acnet::ReliabilityPeer reliability;
        acnet::SessionId session = 0;
        acnet::AccountId account = 0;
        bool saw_snapshot = false;
        float last_x = 0.0F;
        std::size_t last_player_count = 0;
    };
    constexpr std::size_t bot_count = 8;
    std::array<Bot, bot_count> bots;
    for (std::size_t i = 0; i < bot_count; ++i) {
        Bot& bot = bots[i];
        bot.account = 1000 + i;
        CHECK(bot.socket.open(0, error));
        acnet::ClientHello hello;
        hello.town = config.town_id;
        hello.account = bot.account;
        hello.client_nonce = 9000 + i;
        std::vector<std::uint8_t> payload;
        CHECK(acnet::encode(hello, payload));
        const acnet::PacketHeader header =
            bot.reliability.make_header(acnet::MessageType::ClientHello, acnet::Channel::Control, 0);
        std::vector<std::uint8_t> bytes;
        CHECK(acnet::encode_packet(header, payload, bytes, error));
        CHECK(bot.socket.send("127.0.0.1", runtime.bound_port(), bytes, error));
    }

    const std::uint64_t monotonic_start = acserver::monotonic_milliseconds();
    for (int frame = 0; frame < 30; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(runtime.step(monotonic_start + static_cast<std::uint64_t>(frame * 17), wall_start, error));
        for (Bot& bot : bots) {
            acnet::Datagram datagram;
            while (bot.socket.receive(datagram, error)) {
                acnet::DecodedPacket packet;
                CHECK(acnet::decode_packet(datagram.bytes.data(), datagram.bytes.size(), packet, error));
                bot.reliability.receive(packet.header);
                if (packet.header.message_type == acnet::MessageType::ServerHello) {
                    acnet::ServerHello hello;
                    CHECK(acnet::decode(packet.payload, hello));
                    CHECK(hello.result == acnet::ResultCode::Ok);
                    bot.session = hello.session;
                }
            }
            CHECK(error.empty());
        }
    }
    CHECK(runtime.connected_clients() == bot_count);

    for (Bot& bot : bots) {
        CHECK(bot.session != 0);
        acnet::InputCommand input;
        input.sequence = 1;
        input.stick_x = 28000;
        input.client_transform.position = {
            100.0F + static_cast<float>(bot.account - 1000), 0.0F, 200.0F};
        input.client_transform.velocity.x = 25.0F;
        std::vector<std::uint8_t> payload;
        CHECK(acnet::encode(input, payload));
        const acnet::PacketHeader header = bot.reliability.make_header(
            acnet::MessageType::InputCommand, acnet::Channel::Snapshots, bot.session);
        std::vector<std::uint8_t> bytes;
        CHECK(acnet::encode_packet(header, payload, bytes, error));
        CHECK(bot.socket.send("127.0.0.1", runtime.bound_port(), bytes, error));

        const acnet::PacketHeader ping =
            bot.reliability.make_header(acnet::MessageType::Ping, acnet::Channel::Control, bot.session);
        payload.clear();
        CHECK(acnet::encode_packet(ping, payload, bytes, error));
        CHECK(bot.socket.send("127.0.0.1", runtime.bound_port(), bytes, error));
    }

    for (int frame = 30; frame < 180; ++frame) {
        std::this_thread::yield();
        if (!runtime.step(monotonic_start + static_cast<std::uint64_t>(frame * 17),
                          wall_start + frame / 60, error)) {
            throw TestFailure("eight-bot runtime step failed: " + error);
        }
        for (Bot& bot : bots) {
            acnet::Datagram datagram;
            while (bot.socket.receive(datagram, error)) {
                acnet::DecodedPacket packet;
                CHECK(acnet::decode_packet(datagram.bytes.data(), datagram.bytes.size(), packet, error));
                const auto disposition = bot.reliability.receive(packet.header);
                if (disposition != acnet::ReceiveDisposition::New) continue;
                if (packet.header.message_type == acnet::MessageType::TransformSnapshot) {
                    acnet::TransformSnapshot snapshot;
                    CHECK(acnet::decode(packet.payload, snapshot));
                    bot.last_player_count = snapshot.players.size();
                    for (const acnet::PlayerSnapshot& player : snapshot.players) {
                        if (player.account == bot.account) {
                            bot.saw_snapshot = true;
                            bot.last_x = player.transform.position.x;
                            CHECK(player.transform.position.y == 0.0F);
                        }
                    }
                }
            }
            CHECK(error.empty());
        }
    }
    for (const Bot& bot : bots) {
        CHECK(bot.saw_snapshot);
        CHECK(bot.last_player_count == bot_count);
        CHECK(bot.last_x > 20.0F);
    }
    CHECK(runtime.metrics().packets_received >= bot_count * 3);
    CHECK(runtime.metrics().snapshots_sent > bot_count);
    CHECK(runtime.shutdown(error));

    acserver::TownRuntime restarted(config);
    CHECK(restarted.initialize(wall_start + 10, error));
    CHECK(restarted.shutdown(error));
}

void production_clients_connect_move_and_render_each_other() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-client-runtime-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};

    acserver::TownRuntimeConfig server_config;
    server_config.port = 0;
    server_config.data_directory = root / "town";
    server_config.connection_timeout_ms = 60000;
    server_config.invite_key = "production-loopback-key";
    acserver::TownRuntime server(server_config);
    std::string error;
    constexpr std::int64_t wall = 1700000000;
    CHECK(server.initialize(wall, error));

    acnet::ClientConfig first_config;
    first_config.server_port = server.bound_port();
    first_config.town = server_config.town_id;
    first_config.account = 71;
    first_config.invite_key = server_config.invite_key;
    acnet::ClientConfig second_config = first_config;
    second_config.account = 72;
    acnet::ClientRuntime first(first_config);
    acnet::ClientRuntime second(second_config);
    constexpr std::uint64_t start = 10000;
    CHECK(first.start(start, error));
    CHECK(second.start(start, error));

    acnet::Transform first_local;
    first_local.position = {2200.0F, 37.0F, 1000.0F};
    first_local.velocity.y = -3.0F;
    acnet::Transform second_local = first_local;
    second_local.position.x = 2240.0F;
    second_local.position.y = 53.0F;
    second_local.velocity.y = 4.0F;
    for (std::uint64_t frame = 0; frame < 240; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::uint64_t now = start + frame * 17;
        acnet::Transform corrected;
        bool has_correction = false;
        first_local.position.x += 0.75F;
        first_local.velocity.x = 45.0F;
        first_local.yaw = 8192;
        second_local.position.x -= 0.5F;
        second_local.velocity.x = -30.0F;
        second_local.yaw = -8192;
        CHECK(first.frame(now, 26000, 0, 0, 0, first_local, corrected, has_correction, error));
        CHECK(!has_correction);
        if (has_correction) first_local = corrected;
        CHECK(second.frame(now, -26000, 0, 0, 0, second_local, corrected, has_correction, error));
        CHECK(!has_correction);
        if (has_correction) second_local = corrected;
        CHECK(server.step(now, wall + static_cast<std::int64_t>(frame / 60), error));
        CHECK(first.poll(now, error));
        CHECK(second.poll(now, error));
    }
    for (std::uint64_t frame = 240; frame < 300; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const std::uint64_t now = start + frame * 17;
        acnet::Transform corrected;
        bool has_correction = false;
        first_local.velocity.x = 0.0F;
        second_local.velocity.x = 0.0F;
        CHECK(first.frame(now, 0, 0, 0, 0, first_local, corrected, has_correction, error));
        CHECK(!has_correction);
        if (has_correction) first_local = corrected;
        CHECK(second.frame(now, 0, 0, 0, 0, second_local, corrected, has_correction, error));
        CHECK(!has_correction);
        if (has_correction) second_local = corrected;
        CHECK(server.step(now, wall + static_cast<std::int64_t>(frame / 60), error));
        CHECK(first.poll(now, error));
        CHECK(second.poll(now, error));
    }

    CHECK(first.state() == acnet::ClientConnectionState::Connected);
    CHECK(first_local.position.y == 37.0F);
    CHECK(first_local.velocity.y == -3.0F);
    CHECK(second_local.position.y == 53.0F);
    CHECK(second_local.velocity.y == 4.0F);
    if (second.state() != acnet::ClientConnectionState::Connected) {
        throw TestFailure("second production client state=" +
                          std::to_string(static_cast<int>(second.state())) +
                          " rejection=" + std::to_string(static_cast<int>(second.rejection_reason())) +
                          " error=" + second.last_error());
    }
    CHECK(first.local_entity() != 0);
    CHECK(second.local_entity() != 0);
    std::vector<acnet::RemotePresentation> first_remotes;
    std::vector<acnet::RemotePresentation> second_remotes;
    std::uint64_t settle_now = start + 300 * 17;
    bool movement_converged = false;
    for (std::uint64_t i = 0; i < 200; ++i) {
        first_remotes = first.remote_players();
        second_remotes = second.remote_players();
        movement_converged = first_remotes.size() == 1 && second_remotes.size() == 1 &&
            std::fabs(first_remotes[0].transform.position.x - second_local.position.x) < 0.01F &&
            std::fabs(second_remotes[0].transform.position.x - first_local.position.x) < 0.01F &&
            first_remotes[0].transform.yaw == second_local.yaw &&
            second_remotes[0].transform.yaw == first_local.yaw;
        if (movement_converged) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        acnet::Transform corrected;
        bool has_correction = false;
        CHECK(first.frame(++settle_now, 0, 0, 0, 0, first_local, corrected, has_correction, error));
        CHECK(!has_correction);
        CHECK(second.frame(settle_now, 0, 0, 0, 0, second_local, corrected, has_correction, error));
        CHECK(!has_correction);
        CHECK(server.step(settle_now, wall + 5, error));
        CHECK(first.poll(settle_now, error));
        CHECK(second.poll(settle_now, error));
    }
    CHECK(movement_converged);
    CHECK(first_remotes.size() == 1);
    CHECK(second_remotes.size() == 1);
    CHECK(first_remotes[0].account == second_config.account);
    CHECK(second_remotes[0].account == first_config.account);
    CHECK(std::fabs(first_remotes[0].transform.position.x - second_local.position.x) < 0.01F);
    CHECK(std::fabs(second_remotes[0].transform.position.x - first_local.position.x) < 0.01F);
    CHECK(first_remotes[0].transform.yaw == second_local.yaw);
    CHECK(second_remotes[0].transform.yaw == first_local.yaw);
    CHECK(first.packets_received() > 10);
    CHECK(second.packets_received() > 10);
    CHECK(first.baseline() != nullptr);
    CHECK(second.baseline() != nullptr);
    const std::int64_t estimated_town_time = first.estimated_town_time(start + 10000);
    CHECK(estimated_town_time >= first.baseline()->town_unix_seconds);
    CHECK(first.estimated_town_time(start + 12000) == estimated_town_time + 2);
    CHECK(first.baseline()->tiles.size() == 256);
    CHECK(first.baseline()->zone == 1);

    std::uint64_t transaction_now = settle_now;
    const auto transfer_both = [&](std::uint32_t door_id, acnet::ZoneId destination) {
        acnet::ZoneTransferRequest request;
        request.door_id = door_id;
        CHECK(first.request(request, ++transaction_now, error));
        CHECK(second.request(request, ++transaction_now, error));
        std::optional<acnet::TransferOffer> first_offer;
        std::optional<acnet::TransferOffer> second_offer;
        for (std::uint64_t i = 0; i < 200 && (!first_offer.has_value() || !second_offer.has_value()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(++transaction_now, wall + 5, error));
            CHECK(first.poll(transaction_now, error));
            CHECK(second.poll(transaction_now, error));
            if (!first_offer.has_value()) first_offer = first.take_transfer_offer();
            if (!second_offer.has_value()) second_offer = second.take_transfer_offer();
        }
        CHECK(first_offer.has_value());
        CHECK(second_offer.has_value());
        CHECK(first_offer->code == acnet::ResultCode::Ok);
        CHECK(second_offer->code == acnet::ResultCode::Ok);
        CHECK(first_offer->destination_zone == destination);
        CHECK(second_offer->destination_zone == destination);

        bool saw_leaving = false;
        for (std::uint64_t i = 0; i < 200 && !saw_leaving; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(++transaction_now, wall + 5, error));
            CHECK(first.poll(transaction_now, error));
            CHECK(second.poll(transaction_now, error));
            const auto first_zone_remotes = first.remote_players();
            const auto second_zone_remotes = second.remote_players();
            saw_leaving = first_zone_remotes.size() == 1 && second_zone_remotes.size() == 1 &&
                          first_zone_remotes[0].transition_phase == acnet::DoorTransitionPhase::Leaving &&
                          second_zone_remotes[0].transition_phase == acnet::DoorTransitionPhase::Leaving &&
                          first_zone_remotes[0].transition_door == door_id &&
                          second_zone_remotes[0].transition_door == door_id;
        }
        CHECK(saw_leaving);

        const acnet::Vec3 first_arrival = destination >= 100
                                            ? acnet::Vec3{120.0F, 0.0F, 220.0F}
                                            : acnet::Vec3{2200.0F, 0.0F, 1000.0F};
        const acnet::Vec3 second_arrival = destination >= 100
                                             ? acnet::Vec3{122.0F, 0.0F, 220.0F}
                                             : acnet::Vec3{2240.0F, 0.0F, 1000.0F};
        acnet::ZoneReadyRequest ready;
        ready.token = first_offer->token;
        ready.destination_transform.position = first_arrival;
        CHECK(first.ready(ready, ++transaction_now, error));

        bool saw_source_ghost = false;
        for (std::uint64_t i = 0; i < 300 && !saw_source_ghost; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(++transaction_now, wall + 5, error));
            CHECK(first.poll(transaction_now, error));
            CHECK(second.poll(transaction_now, error));
            const auto source_remotes = second.remote_players();
            saw_source_ghost = first.baseline() != nullptr && first.baseline()->zone == destination &&
                               source_remotes.size() == 1 &&
                               source_remotes[0].account == first_config.account &&
                               source_remotes[0].transition_phase == acnet::DoorTransitionPhase::Leaving &&
                               source_remotes[0].transition_door == door_id;
        }
        CHECK(saw_source_ghost);

        ready.token = second_offer->token;
        ready.destination_transform.position = second_arrival;
        CHECK(second.ready(ready, ++transaction_now, error));

        bool shared_zone_visible = false;
        bool saw_arriving = false;
        bool saw_exact_arrivals = false;
        for (std::uint64_t i = 0; i < 300 &&
             (!shared_zone_visible || !saw_arriving || !saw_exact_arrivals); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(++transaction_now, wall + 5, error));
            CHECK(first.poll(transaction_now, error));
            CHECK(second.poll(transaction_now, error));
            if (first.baseline() == nullptr || second.baseline() == nullptr ||
                first.baseline()->zone != destination || second.baseline()->zone != destination) continue;
            const auto first_zone_remotes = first.remote_players();
            const auto second_zone_remotes = second.remote_players();
            shared_zone_visible = first_zone_remotes.size() == 1 && second_zone_remotes.size() == 1 &&
                                  first_zone_remotes[0].zone == destination &&
                                  second_zone_remotes[0].zone == destination;
            saw_arriving = shared_zone_visible &&
                           first_zone_remotes[0].transition_phase == acnet::DoorTransitionPhase::Arriving &&
                           first_zone_remotes[0].transition_door == door_id;
            saw_exact_arrivals = shared_zone_visible &&
                                 std::fabs(first_zone_remotes[0].transform.position.x - second_arrival.x) < 0.01F &&
                                 std::fabs(first_zone_remotes[0].transform.position.z - second_arrival.z) < 0.01F &&
                                 std::fabs(second_zone_remotes[0].transform.position.x - first_arrival.x) < 0.01F &&
                                 std::fabs(second_zone_remotes[0].transform.position.z - first_arrival.z) < 0.01F;
        }
        CHECK(shared_zone_visible);
        CHECK(saw_arriving);
        CHECK(saw_exact_arrivals);
        (void)first.take_transfer_offer();  // Successful ZoneReady acknowledgement.
        (void)second.take_transfer_offer();
    };

    const std::int64_t before_house_time = first.estimated_town_time(transaction_now);
    transfer_both(100, 100);  // Both clients visit resident slot zero's house.
    CHECK(first.state() == acnet::ClientConnectionState::Connected);
    CHECK(second.state() == acnet::ClientConnectionState::Connected);
    CHECK(first.estimated_town_time(transaction_now) >= before_house_time);
    CHECK(first.house_light_mask() == 0);
    CHECK(second.house_light_mask() == 0);
    CHECK(first.baseline() != nullptr);
    CHECK(second.baseline() != nullptr);
    CHECK(first.baseline()->has_house);
    CHECK(second.baseline()->has_house);
    CHECK(first.baseline()->house.house_id == 10000);
    CHECK(first.baseline()->house.owner == first_config.account);

    acnet::HouseUpdate house_update;
    house_update.idempotency = {704, 705};
    house_update.house_id = first.baseline()->house.house_id;
    house_update.expected_house_revision = first.baseline()->house.revision;
    house_update.upgrade_level = 2;
    house_update.main_light_on = true;
    house_update.music_tracks[0] = 37;
    house_update.furniture_switches[0] = 5;
    house_update.furniture[{3, 4, 0, 0}] = {0x1100, 1};
    CHECK(first.request(house_update, ++transaction_now, error));
    std::optional<acnet::HouseUpdateResult> house_result;
    bool house_converged = false;
    for (std::uint64_t i = 0; i < 800 && !house_converged; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(server.step(++transaction_now, wall + 5, error));
        CHECK(first.poll(transaction_now, error));
        CHECK(second.poll(transaction_now, error));
        if (!house_result.has_value()) house_result = first.take_house_update_result();
        house_converged = house_result.has_value() && first.baseline() != nullptr &&
                          second.baseline() != nullptr && first.baseline()->has_house &&
                          second.baseline()->has_house &&
                          first.baseline()->house.revision == house_result->house_revision &&
                          second.baseline()->house.revision == house_result->house_revision &&
                          first.house_light_mask() == 1 && second.house_light_mask() == 1;
    }
    CHECK(house_result.has_value());
    CHECK(house_result->code == acnet::ResultCode::Ok);
    CHECK(house_converged);
    CHECK(first.baseline()->house.initialized);
    CHECK(second.baseline()->house.initialized);
    CHECK(second.baseline()->house.main_light_on);
    CHECK(second.baseline()->house.music_tracks[0] == 37);
    CHECK(second.baseline()->house.furniture_switches[0] == 5);
    CHECK(second.baseline()->house.furniture.at({3, 4, 0, 0}).item == 0x1100);
    CHECK(second.baseline()->house.furniture.at({3, 4, 0, 0}).condition == 1);

    const std::int64_t before_exit_time = first.estimated_town_time(transaction_now);
    transfer_both(200, 1);
    CHECK(first.state() == acnet::ClientConnectionState::Connected);
    CHECK(second.state() == acnet::ClientConnectionState::Connected);
    CHECK(first.estimated_town_time(transaction_now) >= before_exit_time);
    CHECK(first.house_light_mask() == 1);
    CHECK(second.house_light_mask() == 1);

    const auto transaction_origin = server.player_transform(first_config.account);
    CHECK(transaction_origin.has_value());
    const std::int16_t transaction_x = static_cast<std::int16_t>(transaction_origin->position.x / 40.0F);
    const std::int16_t transaction_z = static_cast<std::int16_t>(transaction_origin->position.z / 40.0F);

    acnet::EncounterRequest encounter;
    encounter.account = 999999; // The server must replace this with the authenticated account.
    encounter.idempotency = {700, 701};
    encounter.expected_inventory_revision = 1;
    encounter.tool_slot = 0;
    CHECK(first.request(encounter, transaction_now, error));
    std::optional<acnet::EncounterResult> encounter_result;
    for (std::uint64_t i = 0; i < 30 && !encounter_result.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++transaction_now;
        CHECK(server.step(transaction_now, wall + 5, error));
        CHECK(first.poll(transaction_now, error));
        encounter_result = first.take_encounter_result();
    }
    CHECK(encounter_result.has_value());
    CHECK(encounter_result->code == acnet::ResultCode::Ok);
    CHECK(encounter_result->idempotency.high == 700);

    const auto find_tile = [](const acnet::ClientRuntime& client, std::int16_t x, std::int16_t z) {
        const auto& tiles = client.baseline()->tiles;
        return std::find_if(tiles.begin(), tiles.end(), [&](const auto& tile) {
            return tile.first.zone == 1 && tile.first.x == x && tile.first.z == z;
        });
    };
    auto target = find_tile(first, transaction_x, transaction_z);
    CHECK(target != first.baseline()->tiles.end());
    CHECK(target->second.item == 0);
    acnet::WorldOperation drop;
    drop.type = acnet::WorldOpType::DropItem;
    drop.account = 999999;
    drop.idempotency = {702, 703};
    drop.tile = {1, transaction_x, transaction_z};
    drop.expected_tile_revision = target->second.revision;
    drop.expected_inventory_revision = first.baseline()->inventory.revision;
    drop.inventory_slot = 0;
    drop.expected_item = 0x2203;
    CHECK(first.request(drop, ++transaction_now, error));
    std::optional<acnet::WorldResult> world_result;
    for (std::uint64_t i = 0; i < 400; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++transaction_now;
        CHECK(server.step(transaction_now, wall + 5, error));
        CHECK(first.poll(transaction_now, error));
        CHECK(second.poll(transaction_now, error));
        if (!world_result.has_value()) world_result = first.take_world_result();
        target = find_tile(first, transaction_x, transaction_z);
        const auto observed = find_tile(second, transaction_x, transaction_z);
        if (world_result.has_value() && target != first.baseline()->tiles.end() &&
            observed != second.baseline()->tiles.end() && target->second.item == 0x2203 &&
            observed->second.item == 0x2203) break;
    }
    if (!world_result.has_value()) {
        throw TestFailure("world result timed out: client_state=" +
                          std::to_string(static_cast<int>(first.state())) +
                          " received=" + std::to_string(first.packets_received()) +
                          " sent=" + std::to_string(first.packets_sent()) +
                          " server_received=" + std::to_string(server.metrics().packets_received) +
                          " server_rejected=" + std::to_string(server.metrics().rejected_packets) +
                          " server_malformed=" + std::to_string(server.metrics().malformed_packets) +
                          " error=" + first.last_error());
    }
    if (world_result->code != acnet::ResultCode::Ok) {
        const auto authoritative = server.player_transform(first_config.account);
        throw TestFailure("world result rejected code=" +
                          std::to_string(static_cast<unsigned>(world_result->code)) +
                          " tile_revision=" + std::to_string(world_result->tile_revision) +
                          " inventory_revision=" + std::to_string(world_result->inventory_revision) +
                          " target=" + std::to_string(transaction_x * 40 + 20) + "," +
                          std::to_string(transaction_z * 40 + 20) +
                          " remote=" + std::to_string(second_remotes[0].transform.position.x) + "," +
                          std::to_string(second_remotes[0].transform.position.z) + " authoritative=" +
                          (authoritative.has_value()
                               ? std::to_string(authoritative->position.x) + "," +
                                     std::to_string(authoritative->position.z)
                               : std::string("missing")));
    }
    CHECK(first.baseline()->inventory.slots[0].item == 0);
    CHECK(find_tile(first, transaction_x, transaction_z)->second.item == 0x2203);
    CHECK(find_tile(second, transaction_x, transaction_z)->second.item == 0x2203);

    first.stop(start + 7000);
    second.stop(start + 7000);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    for (std::uint64_t i = 0; i < 10 && server.connected_clients() != 0; ++i) {
        CHECK(server.step(start + 7001 + i, wall + 5, error));
    }
    if (server.connected_clients() != 0) {
        throw TestFailure("disconnect left clients=" + std::to_string(server.connected_clients()) +
                          " packets_received=" + std::to_string(server.metrics().packets_received) +
                          " rejected=" + std::to_string(server.metrics().rejected_packets) +
                          " first_sent=" + std::to_string(first.packets_sent()) +
                          " second_sent=" + std::to_string(second.packets_sent()) +
                          " first_error=" + first.last_error() +
                          " second_error=" + second.last_error());
    }
    CHECK(server.shutdown(error));
}

void canonical_town_bootstrap_survives_clients_and_restart() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-bootstrap-runtime-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};

    acserver::TownRuntimeConfig server_config;
    server_config.port = 0;
    server_config.data_directory = root / "town";
    server_config.connection_timeout_ms = 60000;
    server_config.allow_unauthenticated = true;
    server_config.town_name = "BootTown";
    server_config.town_seed = 42;
    constexpr std::int64_t wall = 1700000000;
    std::string error;

    const auto make_bootstrap = [&](std::uint16_t fill_item, std::uint8_t face) {
        acnet::TownBootstrap bootstrap;
        bootstrap.town_seed = server_config.town_seed;
        bootstrap.land_id = 0x302A;
        bootstrap.town_name = {{'B', 'o', 'o', 't', 'T', 'o', 'w', 'n'}};
        bootstrap.appearance.name = {{'P', 'l', 'a', 'y', 'e', 'r', ' ', ' '}};
        bootstrap.appearance.face = face;
        bootstrap.appearance.clothing = 0x2401;
        bootstrap.tiles.resize(acnet::kTownBootstrapTileCount);
        for (auto& tile : bootstrap.tiles) tile.item = fill_item;
        return bootstrap;
    };

    acserver::TownRuntime first_server(server_config);
    CHECK(first_server.initialize(wall, error));
    acnet::ClientConfig first_config;
    first_config.server_port = first_server.bound_port();
    first_config.account = 8101;
    acnet::ClientConfig second_config = first_config;
    second_config.account = 8102;
    acnet::ClientRuntime first(first_config);
    acnet::ClientRuntime second(second_config);
    CHECK(first.start(1000, error));
    CHECK(second.start(1000, error));
    std::uint64_t now = 1000;
    for (std::uint64_t i = 0; i < 300 &&
         (first.state() != acnet::ClientConnectionState::Connected ||
          second.state() != acnet::ClientConnectionState::Connected); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++now;
        CHECK(first_server.step(now, wall, error));
        CHECK(first.poll(now, error));
        CHECK(second.poll(now, error));
    }
    CHECK(first.state() == acnet::ClientConnectionState::Connected);
    CHECK(second.state() == acnet::ClientConnectionState::Connected);
    CHECK(first.resident_slot() == 0);
    CHECK(second.resident_slot() == 1);
    CHECK(first.town_seed() == 42);
    CHECK(first.town_land_id() == 0x302A);
    CHECK(!first.town_initialized());
    CHECK(!second.town_initialized());

    auto canonical = make_bootstrap(0, 2);
    /* x=49,z=19 maps to block (2,0), unit (1,3). */
    const std::size_t canonical_index = ((0U * 5U + 2U) * 16U + 3U) * 16U + 1U;
    canonical.tiles[canonical_index].item = 0x1234;
    canonical.tiles[canonical_index].buried = true;
    CHECK(first.submit_town_bootstrap(std::move(canonical), ++now, error));
    std::optional<acnet::TownBootstrapResult> first_result;
    for (std::uint64_t i = 0; i < 800 && !first_result.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++now;
        CHECK(first_server.step(now, wall, error));
        CHECK(first.poll(now, error));
        CHECK(second.poll(now, error));
        first_result = first.take_town_bootstrap_result();
    }
    CHECK(first_result.has_value());
    CHECK(first_result->code == acnet::ResultCode::Ok);
    CHECK(first_result->initialized);
    CHECK(first.town_initialized());

    auto attempted_overwrite = make_bootstrap(0x7777, 5);
    CHECK(second.submit_town_bootstrap(std::move(attempted_overwrite), ++now, error));
    std::optional<acnet::TownBootstrapResult> second_result;
    for (std::uint64_t i = 0; i < 800 && !second_result.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++now;
        CHECK(first_server.step(now, wall, error));
        CHECK(first.poll(now, error));
        CHECK(second.poll(now, error));
        second_result = second.take_town_bootstrap_result();
    }
    CHECK(second_result.has_value());
    CHECK(second_result->code == acnet::ResultCode::Ok);
    CHECK(second.town_initialized());
    CHECK(second.baseline() != nullptr);
    const auto canonical_tile = std::find_if(second.baseline()->tiles.begin(), second.baseline()->tiles.end(),
        [](const auto& entry) { return entry.first.zone == 1 && entry.first.x == 49 && entry.first.z == 19; });
    CHECK(canonical_tile != second.baseline()->tiles.end());
    CHECK(canonical_tile->second.item == 0x1234);
    CHECK(canonical_tile->second.buried);

    first.stop(++now);
    second.stop(++now);
    for (std::uint64_t i = 0; i < 20 && first_server.connected_clients() != 0; ++i)
        CHECK(first_server.step(++now, wall, error));
    CHECK(first_server.shutdown(error));

    /* Restart from the current state format. Older v1-v3 checkpoints remain
     * accepted by the decoder, but changing only a v4 header to claim v2 is
     * not a valid legacy fixture because v4 adds bounded house-state fields. */

    acserver::TownRuntime restarted(server_config);
    CHECK(restarted.initialize(wall + 30, error));
    acnet::ClientConfig returning_config = first_config;
    returning_config.server_port = restarted.bound_port();
    acnet::ClientRuntime returning(returning_config);
    CHECK(returning.start(++now, error));
    for (std::uint64_t i = 0; i < 500 && returning.baseline() == nullptr; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++now;
        CHECK(restarted.step(now, wall + 30, error));
        CHECK(returning.poll(now, error));
    }
    CHECK(returning.state() == acnet::ClientConnectionState::Connected);
    CHECK(returning.town_initialized());
    CHECK(returning.resident_slot() == 0);
    CHECK(returning.baseline() != nullptr);
    const auto persisted_tile = std::find_if(returning.baseline()->tiles.begin(), returning.baseline()->tiles.end(),
        [](const auto& entry) { return entry.first.zone == 1 && entry.first.x == 49 && entry.first.z == 19; });
    CHECK(persisted_tile != returning.baseline()->tiles.end());
    CHECK(persisted_tile->second.item == 0x1234);
    CHECK(persisted_tile->second.buried);
    returning.stop(++now);
    CHECK(restarted.shutdown(error));
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"protocol hello round trip", protocol_hello_round_trip},
        {"bounded town bootstrap codecs", town_bootstrap_messages_are_bounded_and_round_trip},
        {"authenticated encryption", cryptography_authenticates_and_rejects_tampering},
        {"large payload fragmentation", fragmentation_reassembles_large_payloads},
        {"transaction message codecs", transaction_messages_round_trip},
        {"server-authoritative encounters", encounters_are_server_authoritative_and_idempotent},
        {"runtime journal replay", runtime_replays_uncheckpointed_world_journal},
        {"SQLite WAL metadata", sqlite_metadata_uses_wal_and_migrations},
        {"IANA timezone DST", named_timezone_applies_dst_transitions},
        {"system clock sync", system_clock_sync_overrides_seeded_and_scaled_time},
        {"town configuration", town_configuration_is_loaded_and_validated},
        {"client network INI", client_network_ini_is_loaded_and_validated},
        {"packet round trip and corruption", packet_round_trip_and_corruption},
        {"protocol rejects truncation/nonfinite", protocol_rejects_truncated_and_nonfinite},
        {"snapshot round trip", snapshot_round_trip},
        {"stable entity IDs", entity_ids_are_stable_and_not_reused},
        {"sessions and reconnect", sessions_negotiate_capacity_and_reconnect},
        {"snapshot interpolation", interpolation_orders_and_extrapolates},
        {"selective reliability", selective_reliability_tracks_ack_windows},
        {"UDP eight-client handshake", udp_eight_client_handshake_smoke},
        {"multiplayer player queries", multiplayer_player_queries_are_scoped},
        {"client-authoritative movement at 200ms", movement_is_client_authoritative_under_latency},
        {"atomic world transactions", world_transactions_are_atomic_idempotent_and_conserved},
        {"economy and escrow trade", economy_and_trade_prevent_value_duplication},
        {"NPC conversation leases", npc_leases_scope_conversations_and_disconnects},
        {"zone handoff and housing", zone_handoffs_and_four_resident_housing_are_safe},
        {"persistence crash recovery", persistence_recovers_checkpoints_journal_and_gci},
        {"semantic GCI round trip", gci_semantic_conversion_preserves_native_save},
        {"clock jobs and replication", clock_jobs_and_replication_survive_empty_time},
        {"real runtime eight-bot smoke", real_runtime_serves_eight_moving_bots},
        {"production client loopback", production_clients_connect_move_and_render_each_other},
        {"canonical town bootstrap restart", canonical_town_bootstrap_survives_clients_and_restart},
    };
    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS  " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL  " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << "/" << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
