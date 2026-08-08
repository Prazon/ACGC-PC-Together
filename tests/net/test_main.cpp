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
#include "acnet/shop.hpp"
#include "acnet/transport.hpp"
#include "acnet/world.hpp"
#include "acnet/zone.hpp"
#include "acserver/persistence.hpp"
#include "acserver/config.hpp"
#include "acserver/mod_packstore.hpp"
#include "acserver/mod_calendar.hpp"
#include "acserver/mod_host.hpp"
#include "acserver/mod_registry.hpp"
#include "acserver/mod_strings.hpp"
#include "acserver/database.hpp"
#include "acserver/gci.hpp"
#include "acserver/town_clock.hpp"
#include "acserver/town_runtime.hpp"
#include "pc_network_config.h"

#include <algorithm>
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
#include <map>
#include <set>
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
    economy.mail_id = 77;
    CHECK(acnet::encode(economy, bytes));
    acnet::EconomyRequest decoded_economy;
    CHECK(acnet::decode(bytes, decoded_economy));
    CHECK(decoded_economy.recipient == 99);
    CHECK(decoded_economy.amount == 500);
    CHECK(decoded_economy.mail_id == 77);

    acnet::EconomyResult economy_result;
    economy_result.code = acnet::ResultCode::Ok;
    economy_result.type = acnet::EconomyOpType::ClaimMail;
    economy_result.idempotency = {16, 17};
    economy_result.inventory_revision = 8;
    economy_result.auxiliary_revision = 9;
    economy_result.balance = 4000;
    economy_result.mail_id = 77;
    economy_result.item = 0x2203;
    CHECK(acnet::encode(economy_result, bytes));
    acnet::EconomyResult decoded_economy_result;
    CHECK(acnet::decode(bytes, decoded_economy_result));
    CHECK(decoded_economy_result.type == acnet::EconomyOpType::ClaimMail);
    CHECK(decoded_economy_result.mail_id == 77);
    CHECK(decoded_economy_result.auxiliary_revision == 9);

    /* Operator operations are outside the client-reachable range in both
     * directions, so neither codec will carry one. */
    acnet::EconomyRequest administrative = economy;
    administrative.type = acnet::EconomyOpType::AdminSendMail;
    CHECK(!acnet::encode(administrative, bytes));
    acnet::EconomyResult administrative_result = economy_result;
    administrative_result.type = acnet::EconomyOpType::AdminGrantBells;
    CHECK(!acnet::encode(administrative_result, bytes));

    acnet::MailDelta mail_delta;
    mail_delta.account = 99;
    mail_delta.mailbox_revision = 4;
    mail_delta.record.id = 77;
    mail_delta.record.sender = 45;
    mail_delta.record.recipient = 99;
    mail_delta.record.attachment = 0x2203;
    mail_delta.record.revision = 1;
    mail_delta.record.location = acnet::MailLocation::Carried;
    mail_delta.record.content.font = 3;
    mail_delta.record.content.body[0] = 'H';
    CHECK(acnet::encode_mail_delta(mail_delta, bytes));
    acnet::MailDelta decoded_mail_delta;
    CHECK(acnet::decode_mail_delta(bytes, decoded_mail_delta));
    CHECK(decoded_mail_delta.record.id == 77);
    CHECK(decoded_mail_delta.record.attachment == 0x2203);
    CHECK(decoded_mail_delta.mailbox_revision == 4);
    CHECK(!decoded_mail_delta.removed);
    CHECK(decoded_mail_delta.record.content.body[0] == 'H');
    CHECK(decoded_mail_delta.record.content.font == 3);
    CHECK(decoded_mail_delta.record.location == acnet::MailLocation::Carried);
    acnet::MailDelta mismatched = mail_delta;
    mismatched.account = 100; // a letter must belong to the mailbox it changes
    CHECK(!acnet::encode_mail_delta(mismatched, bytes));

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
    house.surfaces.wallpaper[0] = 12;
    house.surfaces.flooring[2] = 34;
    house.surfaces.pattern_bits[1] = 3;
    house.surfaces.exterior_palette = 5;
    house.surfaces.ordered_exterior_palette = 6;
    house.surfaces.next_exterior_palette = 7;
    house.surfaces.door_design = 9;
    house.music_box[0] = 0xDEADBEEFu;
    house.music_box[1] = 0x0BADF00Du;
    CHECK(acnet::encode(house, bytes));
    acnet::HouseUpdate decoded_house;
    CHECK(acnet::decode(bytes, decoded_house));
    CHECK(decoded_house.house_id == house.house_id);
    CHECK(decoded_house.upgrade_level == 3);
    CHECK(decoded_house.main_light_on);
    CHECK(decoded_house.music_tracks[0] == 91);
    CHECK(decoded_house.furniture_switches[1] == 0x1122334455667788ULL);
    CHECK(decoded_house.furniture.at({4, 5, 2, 1}).item == 0x3001);
    CHECK(decoded_house.surfaces == house.surfaces);
    CHECK(decoded_house.music_box == house.music_box);

    /* A pattern_bits byte outside the two flags mHm_fllot_bit_c defines is not
     * clamped into range, it is a decode failure -- otherwise a peer could hand
     * the encoder bytes it would then refuse to re-encode. */
    acnet::HouseUpdate bad_surface = house;
    bad_surface.surfaces.pattern_bits[1] = 0x04;
    CHECK(!acnet::encode(bad_surface, bytes));

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
    inventory.equipped.item = 0x2203;
    CHECK(world.register_inventory(player.account, inventory));
    acnet::EncounterAuthority encounters(&players, &world, 12345);
    acnet::EncounterRequest request;
    request.account = player.account;
    request.idempotency = {1, 2};
    request.kind = acnet::EncounterKind::Fish;
    request.expected_inventory_revision = inventory.revision;
    /* A rod in a pocket is not a rod in the hand. */
    acnet::InventoryState pocketed = inventory;
    pocketed.equipped = {};
    pocketed.slots[0].item = 0x2203;
    CHECK(world.set_inventory(player.account, pocketed));
    acnet::EncounterRequest stowed = request;
    stowed.idempotency = {1, 3};
    CHECK(encounters.resolve(stowed, 100, 1700000000, 0).code == acnet::ResultCode::InvalidState);
    CHECK(world.set_inventory(player.account, inventory));
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

void held_items_are_a_conserving_server_swap() {
    acnet::PlayerDirectory players;
    acnet::PlayerView player;
    player.account = 7;
    player.entity = 11;
    player.zone = 1;
    CHECK(players.upsert(player));
    acnet::WorldAuthority world(&players);
    acnet::InventoryState inventory;
    inventory.slots[0].item = 0x2202; /* shovel */
    inventory.slots[1].item = 0x2203; /* rod */
    CHECK(world.register_inventory(player.account, inventory));
    acnet::EconomyAuthority economy(&world, &players);
    CHECK(economy.register_account(player.account, {}));

    const auto units = [&]() {
        const acnet::InventoryState* state = world.inventory(player.account);
        std::size_t count = state->equipped.item != 0 ? 1U : 0U;
        for (const acnet::ItemSlot& slot : state->slots) count += slot.item != 0 ? 1U : 0U;
        return count;
    };
    CHECK(units() == 2);

    acnet::EconomyRequest hold;
    hold.type = acnet::EconomyOpType::HoldItem;
    hold.account = player.account;
    hold.idempotency = {1, 1};
    hold.expected_inventory_revision = world.inventory(player.account)->revision;
    hold.inventory_slot = 0;
    hold.expected_item = 0x2202;
    const acnet::EconomyResult equipped = economy.apply(hold);
    CHECK(equipped.code == acnet::ResultCode::Ok);
    CHECK(world.inventory(player.account)->equipped.item == 0x2202);
    CHECK(world.inventory(player.account)->slots[0].item == 0);
    CHECK(units() == 2);

    /* A replay must not swap a second time. */
    const acnet::EconomyResult replay = economy.apply(hold);
    CHECK(replay.replayed);
    CHECK(world.inventory(player.account)->equipped.item == 0x2202);
    CHECK(world.inventory(player.account)->slots[0].item == 0);

    /* Swapping straight to the other tool puts the shovel back where the rod was. */
    acnet::EconomyRequest swap = hold;
    swap.idempotency = {1, 2};
    swap.expected_inventory_revision = world.inventory(player.account)->revision;
    swap.inventory_slot = 1;
    swap.expected_item = 0x2203;
    CHECK(economy.apply(swap).code == acnet::ResultCode::Ok);
    CHECK(world.inventory(player.account)->equipped.item == 0x2203);
    CHECK(world.inventory(player.account)->slots[1].item == 0x2202);
    CHECK(units() == 2);

    /* Putting away is the same swap against an empty slot. */
    acnet::EconomyRequest putaway = hold;
    putaway.idempotency = {1, 3};
    putaway.expected_inventory_revision = world.inventory(player.account)->revision;
    putaway.inventory_slot = 4;
    putaway.expected_item = 0;
    CHECK(economy.apply(putaway).code == acnet::ResultCode::Ok);
    CHECK(world.inventory(player.account)->equipped.item == 0);
    CHECK(world.inventory(player.account)->slots[4].item == 0x2203);
    CHECK(units() == 2);

    /* Swapping an empty hand with an empty slot is not a move. */
    acnet::EconomyRequest nothing = hold;
    nothing.idempotency = {1, 4};
    nothing.expected_inventory_revision = world.inventory(player.account)->revision;
    nothing.inventory_slot = 7;
    nothing.expected_item = 0;
    CHECK(economy.apply(nothing).code == acnet::ResultCode::InvalidState);

    /* A stale view of the pocket is refused rather than silently retargeted. */
    acnet::EconomyRequest mismatched = hold;
    mismatched.idempotency = {1, 5};
    mismatched.expected_inventory_revision = world.inventory(player.account)->revision;
    mismatched.inventory_slot = 4;
    mismatched.expected_item = 0x2202;
    CHECK(economy.apply(mismatched).code == acnet::ResultCode::InvalidState);

    /* Out of bounds is malformed, not a crash. */
    acnet::EconomyRequest oob = hold;
    oob.idempotency = {1, 6};
    oob.expected_inventory_revision = world.inventory(player.account)->revision;
    oob.inventory_slot = 200;
    oob.expected_item = 0;
    CHECK(economy.apply(oob).code == acnet::ResultCode::Malformed);
    CHECK(units() == 2);
}

void player_presentation_round_trips_and_is_bounded() {
    acnet::PlayerAnimation animation;
    animation.body = acnet::kPlayerAnimationCount - 1;
    animation.overlay = 3;
    animation.part_table = acnet::kPlayerPartTableCount - 1;
    animation.item_state = acnet::kPlayerItemStateCount - 1;
    animation.looping = false;
    animation.reversed = true;

    /* The input command carries it to the server. */
    acnet::InputCommand command;
    command.sequence = 9;
    command.estimated_server_tick = 4;
    command.action = acnet::kPlayerActionCount - 1;
    command.animation = animation;
    command.appearance_bits.decoy = true;
    command.appearance_bits.sunburn = 3;
    command.appearance_bits.umbrella_state = acnet::kUmbrellaStateCount - 1;
    command.appearance_bits.carried_item = 0x1234;
    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode(command, payload));
    acnet::InputCommand decoded_command;
    CHECK(acnet::decode(payload, decoded_command));
    CHECK(decoded_command.animation == animation);
    CHECK(decoded_command.appearance_bits == command.appearance_bits);

    /* The Player delta carries it back out to every viewer. */
    acnet::PlayerPresentationDelta delta;
    delta.account = 12;
    delta.entity = 34;
    delta.presentation.animation = animation;
    delta.presentation.equipped_item = 0x2202;
    delta.presentation.appearance_bits.bee_swell = true;
    delta.presentation.appearance_bits.change_color = true;
    delta.presentation.appearance_bits.sunburn = acnet::kMaximumSunburnRank;
    delta.presentation.appearance_bits.umbrella_state = acnet::kUmbrellaStateCount - 1;
    delta.presentation.appearance_bits.carried_item = 0x3040;
    std::vector<std::uint8_t> delta_bytes;
    CHECK(acnet::encode_player_delta(delta, delta_bytes));
    acnet::PlayerPresentationDelta decoded_delta;
    CHECK(acnet::decode_player_delta(delta_bytes, decoded_delta));
    CHECK(decoded_delta.account == delta.account);
    CHECK(decoded_delta.entity == delta.entity);
    CHECK(decoded_delta.presentation == delta.presentation);

    /* Truncation and trailing bytes are both refused. */
    std::vector<std::uint8_t> truncated(delta_bytes.begin(), delta_bytes.end() - 1);
    CHECK(!acnet::decode_player_delta(truncated, decoded_delta));
    std::vector<std::uint8_t> extended = delta_bytes;
    extended.push_back(0);
    CHECK(!acnet::decode_player_delta(extended, decoded_delta));

    /* Every index is a table lookup on the receiving client, so out-of-range
     * values must never survive the decoder. */
    /* Counted forward from the start -- account (8) then entity (8) -- rather
     * than back from the end, so adding a presentation field cannot silently
     * point these mutations at the wrong byte. */
    const std::size_t body_offset = 16;
    std::vector<std::uint8_t> bad_body = delta_bytes;
    bad_body[body_offset] = static_cast<std::uint8_t>(acnet::kPlayerAnimationCount);
    CHECK(!acnet::decode_player_delta(bad_body, decoded_delta));
    std::vector<std::uint8_t> bad_part_table = delta_bytes;
    bad_part_table[body_offset + 2] = acnet::kPlayerPartTableCount;
    CHECK(!acnet::decode_player_delta(bad_part_table, decoded_delta));
    std::vector<std::uint8_t> bad_item_state = delta_bytes;
    bad_item_state[body_offset + 3] = acnet::kPlayerItemStateCount;
    CHECK(!acnet::decode_player_delta(bad_item_state, decoded_delta));
    std::vector<std::uint8_t> bad_flags = delta_bytes;
    bad_flags[body_offset + 4] = 0xFF;
    CHECK(!acnet::decode_player_delta(bad_flags, decoded_delta));

    /* The resource selectors are table lookups too: a sunburn rank past the
     * palette table or an undefined umbrella action must not survive either,
     * and neither may an unused bit in the shared flag byte. */
    std::vector<std::uint8_t> bad_appearance = delta_bytes;
    bad_appearance[body_offset + 7] = 0x08;
    CHECK(!acnet::decode_player_delta(bad_appearance, decoded_delta));
    std::vector<std::uint8_t> bad_sunburn = delta_bytes;
    bad_sunburn[body_offset + 8] = acnet::kMaximumSunburnRank + 1;
    CHECK(!acnet::decode_player_delta(bad_sunburn, decoded_delta));
    std::vector<std::uint8_t> bad_umbrella = delta_bytes;
    bad_umbrella[body_offset + 9] = acnet::kUmbrellaStateCount;
    CHECK(!acnet::decode_player_delta(bad_umbrella, decoded_delta));

    /* The same bounds apply on the way in, and the movement authority refuses
     * a command that fails them rather than forwarding it. */
    acnet::MovementSimulator movement;
    CHECK(movement.add_player(3, 4, 1, {}));
    acnet::InputCommand accepted;
    accepted.sequence = 1;
    accepted.animation = animation;
    accepted.client_transform.action = acnet::kPlayerActionCount - 1;
    CHECK(movement.submit(3, accepted) == acnet::ResultCode::Ok);
    acnet::InputCommand out_of_range = accepted;
    out_of_range.sequence = 2;
    out_of_range.animation.body = acnet::kPlayerAnimationCount;
    CHECK(movement.submit(3, out_of_range) == acnet::ResultCode::Malformed);
    acnet::InputCommand bad_action = accepted;
    bad_action.sequence = 3;
    bad_action.client_transform.action = acnet::kPlayerActionCount;
    CHECK(movement.submit(3, bad_action) == acnet::ResultCode::Malformed);
    movement.tick();
    CHECK(movement.snapshot(3).presentation.animation == animation);
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
        CHECK(database.schema_version(error) == 4);   /* 4 adds mod_state */
        CHECK(database.journal_mode(error) == "wal");
        CHECK(database.record_account(42, 1700000000, error));
        CHECK(database.record_session(91, 42, true, 1700000001, error));
        CHECK(database.record_transaction(1, 42, 20, acnet::ResultCode::Ok, 1700000002, error));
        CHECK(database.audit(42, "test_action", "bounded details", 1700000003, error));

        /* mod_state backs calendar.store/load. Scoped per mod so one mod cannot
         * read or overwrite another's bookkeeping. */
        std::string stored;
        CHECK(!database.get_mod_state("lantern-night", "awarded_year", stored));
        CHECK(database.set_mod_state("lantern-night", "awarded_year", "n2026", 1700000004));
        CHECK(database.get_mod_state("lantern-night", "awarded_year", stored));
        CHECK(stored == "n2026");
        CHECK(!database.get_mod_state("other-mod", "awarded_year", stored));
        /* Upsert rather than a duplicate-key failure. */
        CHECK(database.set_mod_state("lantern-night", "awarded_year", "n2027", 1700000005));
        CHECK(database.get_mod_state("lantern-night", "awarded_year", stored));
        CHECK(stored == "n2027");
        /* A quote in a value must not break the statement. */
        CHECK(database.set_mod_state("lantern-night", "note", "it's fine", 1700000006));
        CHECK(database.get_mod_state("lantern-night", "note", stored));
        CHECK(stored == "it's fine");
        CHECK(std::filesystem::exists(database.path()));
        CHECK(!std::filesystem::exists(root / "config.toml"));
    }
    acserver::DatabaseStore reopened(root);
    CHECK(reopened.initialize(error));
    CHECK(reopened.schema_version(error) == 4);
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

/* --- Mod host (P1) ------------------------------------------------------ */

namespace modtest {

struct Sandbox {
    std::filesystem::path root;
    ~Sandbox() { std::error_code error; std::filesystem::remove_all(root, error); }
};

Sandbox make_mods_dir(const char* tag) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    Sandbox sandbox{std::filesystem::temp_directory_path() /
                    ("acgc-mods-" + std::string(tag) + "-" + std::to_string(unique))};
    std::filesystem::create_directories(sandbox.root);
    return sandbox;
}

void write_mod(const std::filesystem::path& mods_dir,
               const std::string& id,
               const std::string& lua,
               const std::string& extra_manifest = std::string()) {
    const std::filesystem::path dir = mods_dir / id;
    std::filesystem::create_directories(dir);
    {
        std::ofstream manifest(dir / "mod.toml");
        manifest << "id = \"" << id << "\"\n"
                 << "name = \"" << id << "\"\n"
                 << "version = \"1.0.0\"\n"
                 << "api_version = " << acserver::kModApiVersion << "\n"
                 << "entry = \"init.lua\"\n"
                 << extra_manifest;
    }
    std::ofstream entry(dir / "init.lua");
    entry << lua;
}

} // namespace modtest

/* The sandbox is the security boundary: a mod is data an operator installed,
 * but it must not be able to reach the filesystem, the clock, or the host
 * process. The dangerous libraries are not vendored at all, so this asserts
 * they are genuinely absent rather than merely shadowed. */
void mod_sandbox_denies_escapes() {
    const auto mods = modtest::make_mods_dir("sandbox");
    modtest::write_mod(mods.root, "escape",
        "denied = {}\n"
        "for _, name in ipairs({'io','os','package','require','dofile','loadfile',\n"
        "                       'load','debug','collectgarbage','rawset','rawget',\n"
        "                       'setmetatable','print','coroutine'}) do\n"
        "  if _G[name] ~= nil then denied[#denied+1] = name end\n"
        "end\n"
        "if #denied > 0 then error('reachable: ' .. table.concat(denied, ',')) end\n"
        "function on_hour() end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));
    CHECK(registry.load_order().size() == 1);

    acserver::ModHost host;
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));
    /* Loading cleanly is the assertion: the chunk errors if anything is reachable. */
    CHECK(!host.quarantined("escape"));
    CHECK(host.last_error("escape").empty());
    CHECK(host.call_hook("escape", "on_hour"));
}

/* A mod that allocates without bound must fail its own call, not exhaust the
 * host. The ceiling is enforced in the allocator, so it trips regardless of
 * what the mod is doing when it happens. */
void mod_memory_cap_holds() {
    const auto mods = modtest::make_mods_dir("memory");
    modtest::write_mod(mods.root, "greedy",
        "function on_hour()\n"
        "  local t = {}\n"
        "  for i = 1, 100000000 do t[i] = string.rep('x', 64) end\n"
        "end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    acserver::ModLimits limits;
    limits.memory_bytes = 512u * 1024u;
    limits.instructions_hour = 100000000;   /* isolate the memory ceiling from the budget */
    acserver::ModHost host;
    CHECK(host.load_all(registry, limits, error));
    CHECK(!host.quarantined("greedy"));

    CHECK(!host.call_hook("greedy", "on_hour"));
    CHECK(!host.last_error("greedy").empty());
    /* The host survives, and a later well-behaved call still works. */
    CHECK(host.metrics().hook_errors >= 1);
}

/* An infinite loop must abort that callback and leave the town serving. */
void mod_instruction_budget_holds() {
    const auto mods = modtest::make_mods_dir("budget");
    modtest::write_mod(mods.root, "spinner",
        "spun = 0\n"
        "function on_hour() while true do spun = spun + 1 end end\n"
        "function on_day_start() end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    acserver::ModLimits limits;
    limits.instructions_hour = 20000;
    acserver::ModHost host;
    CHECK(host.load_all(registry, limits, error));

    CHECK(!host.call_hook("spinner", "on_hour"));
    CHECK(host.last_error("spinner").find("instruction budget") != std::string::npos);
    /* One failure is not quarantine, and an unrelated hook still runs. */
    CHECK(!host.quarantined("spinner"));
    CHECK(host.call_hook("spinner", "on_day_start"));
}

/* Repeated failure disables the mod and stops calling it, but the host keeps
 * running -- a broken holiday mod is never worth dropping a town's players. */
void mod_errors_quarantine() {
    const auto mods = modtest::make_mods_dir("quarantine");
    modtest::write_mod(mods.root, "broken",
        "calls = 0\n"
        "function on_hour() calls = calls + 1 error('always') end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    acserver::ModLimits limits;
    limits.errors_to_quarantine = 3;
    acserver::ModHost host;
    CHECK(host.load_all(registry, limits, error));

    CHECK(!host.call_hook("broken", "on_hour"));
    CHECK(!host.quarantined("broken"));
    CHECK(!host.call_hook("broken", "on_hour"));
    CHECK(!host.quarantined("broken"));
    CHECK(!host.call_hook("broken", "on_hour"));
    CHECK(host.quarantined("broken"));

    /* Once quarantined the hook is not entered again. */
    const std::uint64_t before = host.metrics().hooks_called;
    CHECK(!host.call_hook("broken", "on_hour"));
    CHECK(host.metrics().hooks_called == before);
}

/* Load order must be a pure function of the mod set. Filesystem iteration order
 * varies between machines, and a load-order bug that only reproduces on one of
 * them is the kind that survives to release. */
void mod_load_order_is_deterministic() {
    const auto mods = modtest::make_mods_dir("order");
    modtest::write_mod(mods.root, "zulu", "function on_hour() end\n", "requires = [\"alpha\"]\n");
    modtest::write_mod(mods.root, "alpha", "function on_hour() end\n");
    modtest::write_mod(mods.root, "mike", "function on_hour() end\n", "requires = [\"zulu\"]\n");

    std::string error;
    acserver::ModRegistry first;
    CHECK(first.scan(mods.root, error));
    std::vector<std::string> ids;
    for (const auto& manifest : first.load_order()) ids.push_back(manifest.id);
    CHECK(ids.size() == 3);
    CHECK(ids[0] == "alpha");
    CHECK(ids[1] == "zulu");
    CHECK(ids[2] == "mike");

    acserver::ModRegistry second;
    CHECK(second.scan(mods.root, error));
    std::vector<std::string> repeat;
    for (const auto& manifest : second.load_order()) repeat.push_back(manifest.id);
    CHECK(repeat == ids);
    /* Same set, same digest -- this is what ServerHello advertises. */
    CHECK(first.manifest_digest() == second.manifest_digest());

    /* A cycle is refused rather than partially ordered. */
    const auto cyclic = modtest::make_mods_dir("cycle");
    modtest::write_mod(cyclic.root, "aaa", "\n", "requires = [\"bbb\"]\n");
    modtest::write_mod(cyclic.root, "bbb", "\n", "requires = [\"aaa\"]\n");
    acserver::ModRegistry broken;
    CHECK(!broken.scan(cyclic.root, error));
    CHECK(error.find("cycle") != std::string::npos);
}

/* Manifest validation, and the two rules that protect the town: an api_version
 * this build does not implement is refused up front rather than failing later
 * on a missing function, and `entry` may not escape the mod directory. */
void mod_manifest_is_validated() {
    acserver::ModManifest manifest;
    std::string error;

    CHECK(acserver::parse_mod_manifest(
        "id = \"lantern-night\"\nversion = \"1.0.0\"\napi_version = 1\n", manifest, error));
    CHECK(manifest.id == "lantern-night");
    CHECK(manifest.entry == "init.lua");
    CHECK(manifest.name == "lantern-night");

    CHECK(!acserver::parse_mod_manifest("id = \"x\"\nversion = \"1\"\napi_version = 99\n", manifest, error));
    CHECK(error.find("api_version") != std::string::npos);

    CHECK(!acserver::parse_mod_manifest(
        "id = \"x\"\nversion = \"1\"\napi_version = 1\nentry = \"../../etc/passwd.lua\"\n",
        manifest, error));
    CHECK(error.find("entry") != std::string::npos);

    CHECK(!acserver::parse_mod_manifest("id = \"Bad_Id\"\nversion = \"1\"\napi_version = 1\n", manifest, error));
    CHECK(!acserver::parse_mod_manifest("id = \"x\"\napi_version = 1\n", manifest, error));
    CHECK(!acserver::parse_mod_manifest("id = \"x\"\nversion = \"1\"\napi_version = 1\nnope = 1\n",
                                        manifest, error));

    /* A directory whose id disagrees with its folder name is refused: the
     * folder is what an operator reads, so the two must not diverge. */
    const auto mods = modtest::make_mods_dir("mismatch");
    const std::filesystem::path dir = mods.root / "folder-name";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "mod.toml");
        out << "id = \"other-name\"\nversion = \"1\"\napi_version = 1\n";
    }
    { std::ofstream out(dir / "init.lua"); out << "\n"; }
    acserver::ModRegistry registry;
    CHECK(!registry.scan(mods.root, error));
    CHECK(error.find("does not match directory") != std::string::npos);

    /* No mods directory at all is the normal case, not an error. */
    acserver::ModRegistry none;
    CHECK(none.scan(mods.root / "definitely-absent", error));
    CHECK(none.empty());
}

/* Randomness a mod sees must be reproducible from server state: `os` is absent
 * so it cannot read the clock, and math.random is host-seeded from the mod's
 * content hash. Two hosts over the same mod must agree. */
void mod_randomness_is_deterministic() {
    const auto mods = modtest::make_mods_dir("random");
    modtest::write_mod(mods.root, "dice",
        "rolls = {}\n"
        "function on_hour()\n"
        "  for i = 1, 8 do rolls[i] = math.random(1, 1000000) end\n"
        "  if rolls[1] == rolls[2] and rolls[2] == rolls[3] then error('not varying') end\n"
        "end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    acserver::ModHost first;
    CHECK(first.load_all(registry, acserver::ModLimits{}, error));
    CHECK(first.call_hook("dice", "on_hour"));

    acserver::ModHost second;
    CHECK(second.load_all(registry, acserver::ModLimits{}, error));
    CHECK(second.call_hook("dice", "on_hour"));
    CHECK(first.last_error("dice").empty());
    CHECK(second.last_error("dice").empty());
}

/* One mod failing to load must not deny the others. */
void mod_one_bad_mod_does_not_block_the_rest() {
    const auto mods = modtest::make_mods_dir("mixed");
    modtest::write_mod(mods.root, "good", "function on_hour() end\n");
    modtest::write_mod(mods.root, "syntax", "function on_hour( end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    acserver::ModHost host;
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));
    CHECK(host.quarantined("syntax"));
    CHECK(!host.last_error("syntax").empty());
    CHECK(!host.quarantined("good"));
    CHECK(host.call_hook("good", "on_hour"));
}

/* Mod-authored text has to reach the client in the game's own codepage, which
 * is not ASCII. Anything without a glyph is a load-time error naming the
 * character, never a silent substitution. */
void mod_strings_transcode_to_the_game_codepage() {
    acserver::GameString out;
    std::string error;

    CHECK(acserver::transcode_utf8("Lantern Night", out, error));
    CHECK(out.size() == 13);
    CHECK(out[0] == 'L');

    /* Latin-1 accents map to their own glyph indices, not to ASCII. */
    CHECK(acserver::transcode_utf8("Caf\xC3\xA9", out, error));   /* "Café" */
    CHECK(out.size() == 4);
    CHECK(out[3] == 124);                                          /* CHAR_ACUTE_e */

    /* A curly quote has no glyph: refused, and the message says which. */
    CHECK(!acserver::transcode_utf8("Don\xE2\x80\x99t", out, error));
    CHECK(error.find("U+2019") != std::string::npos);

    /* Truncated UTF-8 must be rejected, not read past the end. */
    CHECK(!acserver::transcode_utf8(std::string("bad\xC3", 4), out, error));
    CHECK(error.find("invalid UTF-8") != std::string::npos);

    /* Name records are fixed-width and space-padded. */
    CHECK(acserver::transcode_utf8("Lantern", out, error));
    CHECK(acserver::pad_to(out, 16, error));
    CHECK(out.size() == 16);
    CHECK(out[15] == ' ');
    CHECK(!acserver::pad_to(out, 4, error));
}

/* Date maths a holiday depends on. Verified against real 2026 dates: getting
 * these subtly wrong would put a town's holiday on the wrong day, and nothing
 * downstream would notice. */
void mod_calendar_date_helpers_are_correct() {
    CHECK(acserver::weekday_of(2026, 8, 8) == 6);        /* Saturday */
    CHECK(acserver::weekday_of(1970, 1, 1) == 4);        /* Thursday, the epoch */
    CHECK(acserver::weekday_of(2000, 2, 29) == 2);       /* Tuesday, a leap day */

    CHECK(acserver::days_in_month(2026, 2) == 28);
    CHECK(acserver::days_in_month(2024, 2) == 29);       /* leap */
    CHECK(acserver::days_in_month(1900, 2) == 28);       /* century, not leap */
    CHECK(acserver::days_in_month(2000, 2) == 29);       /* 400-year, leap */
    CHECK(acserver::days_in_month(2026, 11) == 30);

    /* Equinoxes land on the 19th-21st and 21st-23rd respectively; the original
     * game's sports fairs key off the same days. */
    const int vernal = acserver::vernal_equinox_day(2026);
    CHECK(vernal >= 19 && vernal <= 21);
    const int autumn = acserver::autumn_equinox_day(2026);
    CHECK(autumn >= 21 && autumn <= 23);
}

/* Every recurrence form a mod can declare, against known 2026 dates. */
void mod_holidays_resolve_per_year() {
    std::vector<acserver::ResolvedHoliday> out;
    std::string error;

    /* Fixed date. */
    acserver::HolidaySpec fixed;
    fixed.id = "lantern";
    fixed.name_key = "n";
    fixed.recurrence = acserver::Recurrence::FixedDate;
    fixed.month = 10;
    fixed.day = 7;
    fixed.hour_from = 18;
    fixed.hour_to = 22;
    CHECK(acserver::resolve_holiday(fixed, 2026, out, error));
    CHECK(out.size() == 1);
    CHECK(out[0].month == 10 && out[0].day == 7);

    /* Nth weekday: US Thanksgiving 2026 is Thursday 26 November. */
    out.clear();
    acserver::HolidaySpec nth;
    nth.id = "harvest";
    nth.name_key = "n";
    nth.recurrence = acserver::Recurrence::NthWeekday;
    nth.month = 11;
    nth.week = 4;
    nth.weekday = 4;                                     /* Thursday */
    CHECK(acserver::resolve_holiday(nth, 2026, out, error));
    CHECK(out.size() == 1);
    CHECK(out[0].day == 26);

    /* days_after is how the day-after-Thanksgiving sale is expressed. */
    out.clear();
    nth.days_after = 1;
    CHECK(acserver::resolve_holiday(nth, 2026, out, error));
    CHECK(out[0].day == 27);

    /* Second Sunday in May 2026 is the 10th. */
    out.clear();
    acserver::HolidaySpec mothers;
    mothers.id = "mothers";
    mothers.name_key = "n";
    mothers.recurrence = acserver::Recurrence::NthWeekday;
    mothers.month = 5;
    mothers.week = 2;
    mothers.weekday = 0;                                 /* Sunday */
    CHECK(acserver::resolve_holiday(mothers, 2026, out, error));
    CHECK(out[0].day == 10);

    /* Last Sunday in November 2026 is the 29th. */
    out.clear();
    acserver::HolidaySpec last;
    last.id = "last";
    last.name_key = "n";
    last.recurrence = acserver::Recurrence::LastWeekday;
    last.month = 11;
    last.weekday = 0;
    CHECK(acserver::resolve_holiday(last, 2026, out, error));
    CHECK(out.size() == 1);
    CHECK(out[0].day == 29);

    /* Every Sunday in June 2026: the 7th, 14th, 21st and 28th. */
    out.clear();
    acserver::HolidaySpec weekly;
    weekly.id = "tourney";
    weekly.name_key = "n";
    weekly.recurrence = acserver::Recurrence::EveryWeekday;
    weekly.month = 6;
    weekly.weekday = 0;
    CHECK(acserver::resolve_holiday(weekly, 2026, out, error));
    CHECK(out.size() == 4);
    CHECK(out[0].day == 7 && out[1].day == 14 && out[2].day == 21 && out[3].day == 28);

    /* A fifth Monday that does not exist yields no occurrence rather than a
     * date wrapped into the next month. */
    out.clear();
    acserver::HolidaySpec fifth;
    fifth.id = "fifth";
    fifth.name_key = "n";
    fifth.recurrence = acserver::Recurrence::NthWeekday;
    fifth.month = 2;
    fifth.week = 5;
    fifth.weekday = 1;
    CHECK(acserver::resolve_holiday(fifth, 2026, out, error));
    CHECK(out.empty());

    /* 29 February simply does not occur in a common year. */
    out.clear();
    acserver::HolidaySpec leap;
    leap.id = "leap";
    leap.name_key = "n";
    leap.recurrence = acserver::Recurrence::FixedDate;
    leap.month = 2;
    leap.day = 29;
    CHECK(acserver::resolve_holiday(leap, 2026, out, error));
    CHECK(out.empty());
    CHECK(acserver::resolve_holiday(leap, 2024, out, error));
    CHECK(out.size() == 1 && out[0].day == 29);
}

/* Malformed specs are refused at registration, where the error can name the
 * mod, rather than resolving to a nonsense date. */
void mod_holiday_specs_are_validated() {
    std::string error;
    acserver::HolidaySpec spec;
    spec.id = "x";
    spec.name_key = "n";

    spec.month = 13;
    CHECK(!acserver::validate_holiday(spec, error));
    spec.month = 10;

    spec.day = 32;
    CHECK(!acserver::validate_holiday(spec, error));
    spec.day = 7;

    spec.hour_from = 22;
    spec.hour_to = 6;
    CHECK(!acserver::validate_holiday(spec, error));
    CHECK(error.find("hour_to") != std::string::npos);
    spec.hour_from = 0;
    spec.hour_to = 23;

    spec.recurrence = acserver::Recurrence::NthWeekday;
    spec.week = 6;
    CHECK(!acserver::validate_holiday(spec, error));
    spec.week = 2;
    spec.weekday = 9;
    CHECK(!acserver::validate_holiday(spec, error));
    spec.weekday = 3;
    CHECK(acserver::validate_holiday(spec, error));

    spec.name_key.clear();
    CHECK(!acserver::validate_holiday(spec, error));
}

/* Active and rumour windows, including the boundaries that decide whether a
 * holiday fires at all. */
void mod_holiday_windows_are_bounded() {
    std::vector<acserver::ResolvedHoliday> out;
    std::string error;
    acserver::HolidaySpec spec;
    spec.id = "lantern";
    spec.name_key = "n";
    spec.month = 10;
    spec.day = 7;
    spec.hour_from = 18;
    spec.hour_to = 22;
    spec.rumor_days_before = 6;
    CHECK(acserver::resolve_holiday(spec, 2026, out, error));
    const acserver::ResolvedHoliday& holiday = out[0];

    acnet::TownDate date;
    date.year = 2026;
    date.month = 10;
    date.day = 7;

    date.hour = 17;
    CHECK(!acserver::holiday_active_at(holiday, date));
    date.hour = 18;                                       /* inclusive lower bound */
    CHECK(acserver::holiday_active_at(holiday, date));
    date.hour = 22;                                       /* inclusive upper bound */
    CHECK(acserver::holiday_active_at(holiday, date));
    date.hour = 23;
    CHECK(!acserver::holiday_active_at(holiday, date));

    date.day = 6;
    date.hour = 20;
    CHECK(!acserver::holiday_active_at(holiday, date));

    /* Rumour runs the six days before, not the day itself. */
    date.day = 7;
    CHECK(!acserver::holiday_rumor_at(holiday, 2026, date));
    date.day = 6;
    CHECK(acserver::holiday_rumor_at(holiday, 2026, date));
    date.day = 1;                                         /* six days before */
    CHECK(acserver::holiday_rumor_at(holiday, 2026, date));
    date.day = 30;                                        /* seven days before */
    date.month = 9;
    CHECK(!acserver::holiday_rumor_at(holiday, 2026, date));

    /* A window that reaches back across a month boundary still works, because
     * the comparison is in absolute days rather than day-of-month. */
    std::vector<acserver::ResolvedHoliday> newyear;
    acserver::HolidaySpec january;
    january.id = "ny";
    january.name_key = "n";
    january.month = 1;
    january.day = 3;
    january.rumor_days_before = 5;
    CHECK(acserver::resolve_holiday(january, 2026, newyear, error));
    acnet::TownDate december;
    december.year = 2025;
    december.month = 12;
    december.day = 30;
    CHECK(acserver::holiday_rumor_at(newyear[0], 2026, december));
}

/* The whole P2 path: a mod written the way a mod author would write it,
 * declaring holidays through the Lua API, resolved against a real year. */
void mod_calendar_api_registers_holidays() {
    const auto mods = modtest::make_mods_dir("calapi");
    modtest::write_mod(mods.root, "lantern-night",
        "calendar.register {\n"
        "  id = 'lantern_night',\n"
        "  name = 'holiday_name',\n"
        "  date = { month = 10, day = 7 },\n"
        "  hours = { from = 18, to = 22 },\n"
        "  rumor = { days_before = 6 },\n"
        "}\n"
        "calendar.register {\n"
        "  id = 'harvest',\n"
        "  name = 'harvest_name',\n"
        "  date = { month = 11, week = 4, weekday = 'thursday' },\n"
        "  hours = { from = 15, to = 20 },\n"
        "}\n"
        "calendar.register {\n"
        "  id = 'tourney',\n"
        "  name = 'tourney_name',\n"
        "  date = { month = 6, every = 'sunday' },\n"
        "  marker = false,\n"
        "}\n"
        "calendar.on('day_start', function() end)\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    acserver::ModHost host;
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));
    CHECK(!host.quarantined("lantern-night"));
    CHECK(host.last_error("lantern-night").empty());

    const std::vector<acserver::HolidaySpec>& specs = host.holidays();
    CHECK(specs.size() == 3);
    /* Ids are namespaced so two mods can both declare "harvest". */
    CHECK(specs[0].id == "lantern-night.lantern_night");
    CHECK(specs[0].hour_from == 18 && specs[0].hour_to == 22);
    CHECK(specs[0].rumor_days_before == 6);
    CHECK(specs[1].recurrence == acserver::Recurrence::NthWeekday);
    CHECK(specs[1].weekday == 4);
    CHECK(specs[2].recurrence == acserver::Recurrence::EveryWeekday);
    CHECK(!specs[2].marker);

    /* calendar.on installs the global the host calls by name. */
    CHECK(host.call_hook("lantern-night", "on_day_start"));

    /* Resolved against 2026: 7 October, Thanksgiving on the 26th, four Sundays. */
    std::vector<acserver::ResolvedHoliday> resolved;
    for (const acserver::HolidaySpec& spec : specs) {
        CHECK(acserver::resolve_holiday(spec, 2026, resolved, error));
    }
    CHECK(resolved.size() == 6);
    CHECK(resolved[0].month == 10 && resolved[0].day == 7);
    CHECK(resolved[1].month == 11 && resolved[1].day == 26);

    acnet::TownDate date;
    date.year = 2026;
    date.month = 10;
    date.day = 7;
    date.hour = 19;
    CHECK(acserver::holiday_active_at(resolved[0], date));
}

/* A malformed spec fails at registration with a message naming the problem,
 * and takes the whole mod with it rather than half-registering a calendar. */
void mod_calendar_api_rejects_bad_specs() {
    const auto bad_hours = modtest::make_mods_dir("badhours");
    modtest::write_mod(bad_hours.root, "broken",
        "calendar.register { id = 'ok', name = 'n', date = { month = 1, day = 1 } }\n"
        "calendar.register { id = 'bad', name = 'n', date = { month = 1, day = 1 },\n"
        "                    hours = { from = 22, to = 6 } }\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(bad_hours.root, error));
    acserver::ModHost host;
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));
    CHECK(host.quarantined("broken"));
    CHECK(host.last_error("broken").find("hour_to") != std::string::npos);
    /* The first, valid registration is rolled back too: half a mod's calendar
     * is worse than none of it. */
    CHECK(host.holidays().empty());

    /* A date table that selects no recurrence at all. */
    const auto no_form = modtest::make_mods_dir("noform");
    modtest::write_mod(no_form.root, "vague",
        "calendar.register { id = 'x', name = 'n', date = { month = 5 } }\n");
    acserver::ModRegistry vague_registry;
    CHECK(vague_registry.scan(no_form.root, error));
    acserver::ModHost vague_host;
    CHECK(vague_host.load_all(vague_registry, acserver::ModLimits{}, error));
    CHECK(vague_host.quarantined("vague"));

    /* A misspelled weekday is named rather than silently treated as Sunday. */
    const auto misspelled = modtest::make_mods_dir("weekday");
    modtest::write_mod(misspelled.root, "typo",
        "calendar.register { id = 'x', name = 'n',\n"
        "                    date = { month = 5, week = 2, weekday = 'sundy' } }\n");
    acserver::ModRegistry typo_registry;
    CHECK(typo_registry.scan(misspelled.root, error));
    acserver::ModHost typo_host;
    CHECK(typo_host.load_all(typo_registry, acserver::ModLimits{}, error));
    CHECK(typo_host.quarantined("typo"));
    CHECK(typo_host.last_error("typo").find("sundy") != std::string::npos);
}

/* Two mods declaring the same local id must not collide, and a mod that is
 * quarantined later stops shaping the calendar. */
void mod_calendar_holidays_are_namespaced() {
    const auto mods = modtest::make_mods_dir("namespace");
    modtest::write_mod(mods.root, "alpha",
        "calendar.register { id = 'harvest', name = 'n', date = { month = 9, day = 1 } }\n"
        "function on_hour() error('alpha always fails') end\n");
    modtest::write_mod(mods.root, "beta",
        "calendar.register { id = 'harvest', name = 'n', date = { month = 9, day = 2 } }\n"
        "function on_hour() end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    acserver::ModLimits limits;
    limits.errors_to_quarantine = 1;
    acserver::ModHost host;
    CHECK(host.load_all(registry, limits, error));
    CHECK(host.holidays().size() == 2);
    CHECK(host.holidays()[0].id == "alpha.harvest");
    CHECK(host.holidays()[1].id == "beta.harvest");

    /* alpha fails its hook and is quarantined; its holiday goes with it. */
    CHECK(!host.call_hook("alpha", "on_hour"));
    CHECK(host.quarantined("alpha"));
    host.drop_quarantined_holidays();
    CHECK(host.holidays().size() == 1);
    CHECK(host.holidays()[0].id == "beta.harvest");
}

namespace modtest {

/* A ModWorld a test controls, so the Lua surface can be exercised without a
 * whole TownRuntime. Records effects so a test can assert what a mod asked for. */
class FakeWorld : public acserver::ModWorld {
public:
    acnet::TownDate date;
    int weather_kind = 0;
    std::vector<std::uint64_t> online;
    std::set<std::string> active_holidays;
    bool allow_effects = true;

    std::vector<int> weather_requests;
    std::vector<std::pair<std::uint64_t, std::uint16_t>> grants;
    std::vector<std::pair<std::string, std::string>> announcements;
    std::vector<std::pair<std::uint8_t, std::uint8_t>> song_grants;
    std::map<std::string, std::string> kv;

    acnet::TownDate today() const override { return date; }
    int weather() const override { return weather_kind; }
    std::vector<std::uint64_t> players_online() const override { return online; }
    bool holiday_active(const std::string& id) const override { return active_holidays.count(id) != 0; }

    bool set_weather(int kind) override {
        weather_requests.push_back(kind);
        if (allow_effects) weather_kind = kind;
        return allow_effects;
    }
    bool grant_item(std::uint64_t account, std::uint16_t item) override {
        grants.emplace_back(account, item);
        return allow_effects;
    }
    void announce(const std::string& mod_id, const std::string& key) override {
        announcements.emplace_back(mod_id, key);
    }
    bool grant_song(std::uint8_t house_slot, std::uint8_t song) override {
        song_grants.emplace_back(house_slot, song);
        return allow_effects;
    }
    bool store(const std::string& mod_id, const std::string& key, const std::string& value) override {
        if (!allow_effects) return false;
        kv[mod_id + "/" + key] = value;
        return true;
    }
    bool load(const std::string& mod_id, const std::string& key, std::string& value) const override {
        const auto it = kv.find(mod_id + "/" + key);
        if (it == kv.end()) return false;
        value = it->second;
        return true;
    }
};

} // namespace modtest

/* The query side. A mod's only clock is calendar.today(): `os` is absent, so
 * anything time-dependent has to come through here and is therefore
 * reproducible from server state. */
void mod_calendar_queries_read_the_town() {
    const auto mods = modtest::make_mods_dir("query");
    modtest::write_mod(mods.root, "probe",
        "seen = {}\n"
        "function on_hour()\n"
        "  local t = calendar.today()\n"
        "  seen.year, seen.month, seen.day, seen.hour, seen.weekday =\n"
        "      t.year, t.month, t.day, t.hour, t.weekday\n"
        "  seen.weather = calendar.weather()\n"
        "  seen.players = #calendar.players_online()\n"
        "  seen.active = calendar.is_active('lantern')\n"
        "  if seen.year ~= 2026 then error('year ' .. tostring(seen.year)) end\n"
        "  if seen.weather ~= 'rain' then error('weather ' .. seen.weather) end\n"
        "  if seen.players ~= 2 then error('players ' .. seen.players) end\n"
        "  if not seen.active then error('holiday should be active') end\n"
        "end\n");

    modtest::FakeWorld world;
    world.date.year = 2026;
    world.date.month = 10;
    world.date.day = 7;
    world.date.hour = 19;
    world.date.weekday = 3;
    world.weather_kind = 2;                       /* rain */
    world.online = { 11, 22 };
    world.active_holidays.insert("probe.lantern");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    acserver::ModHost host;
    host.set_world(&world);
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));
    CHECK(host.call_hook("probe", "on_hour"));
    CHECK(host.last_error("probe").empty());
}

/* The effect side. A mod asks; the runtime commits. A refusal is visible to the
 * mod rather than a silent no-op, which is what lets a mod written against an
 * authoritative town behave sensibly when the town says no. */
void mod_calendar_effects_route_through_the_runtime() {
    const auto mods = modtest::make_mods_dir("effects");
    modtest::write_mod(mods.root, "lantern-night",
        "function on_holiday_begin(id)\n"
        "  if id ~= 'lantern_night' then error('unexpected id ' .. id) end\n"
        "  calendar.set_weather('clear')\n"
        "  calendar.announce('banner')\n"
        "end\n"
        "function on_holiday_end(id)\n"
        "  for _, account in ipairs(calendar.players_online()) do\n"
        "    calendar.grant(account, 0x30A1)\n"
        "  end\n"
        "  calendar.store('awarded_year', calendar.today().year)\n"
        "end\n");

    modtest::FakeWorld world;
    world.date.year = 2026;
    world.online = { 7, 9 };

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));
    acserver::ModHost host;
    host.set_world(&world);
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));

    /* The host passes the un-namespaced id to the owning mod. */
    host.call_holiday_hook("lantern-night.lantern_night", true);
    CHECK(world.weather_requests.size() == 1);
    CHECK(world.weather_requests[0] == 0);
    CHECK(world.announcements.size() == 1);
    CHECK(world.announcements[0].first == "lantern-night");
    CHECK(world.announcements[0].second == "banner");

    host.call_holiday_hook("lantern-night.lantern_night", false);
    CHECK(world.grants.size() == 2);
    CHECK(world.grants[0].first == 7 && world.grants[0].second == 0x30A1);
    CHECK(world.kv["lantern-night/awarded_year"] == "n2026");
    CHECK(host.last_error("lantern-night").empty());

    /* A hook belonging to another mod is not called. */
    host.call_holiday_hook("someone-else.lantern_night", true);
    CHECK(world.weather_requests.size() == 1);
}

/* store/load round-trips the three value types a mod can persist, and is
 * per-mod: one mod cannot read another's state. */
void mod_calendar_store_round_trips() {
    const auto mods = modtest::make_mods_dir("store");
    modtest::write_mod(mods.root, "keeper",
        "function on_hour()\n"
        "  calendar.store('count', 41)\n"
        "  calendar.store('label', 'lantern')\n"
        "  calendar.store('done', true)\n"
        "  if calendar.load('count') ~= 41 then error('number') end\n"
        "  if calendar.load('label') ~= 'lantern' then error('string') end\n"
        "  if calendar.load('done') ~= true then error('boolean') end\n"
        "  if calendar.load('absent') ~= nil then error('absent should be nil') end\n"
        "end\n");
    modtest::write_mod(mods.root, "nosy",
        "function on_hour()\n"
        "  if calendar.load('count') ~= nil then error('read another mod state') end\n"
        "end\n");

    modtest::FakeWorld world;
    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));
    acserver::ModHost host;
    host.set_world(&world);
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));

    CHECK(host.call_hook("keeper", "on_hour"));
    CHECK(host.last_error("keeper").empty());
    CHECK(host.call_hook("nosy", "on_hour"));
    CHECK(host.last_error("nosy").empty());

    /* Oversized keys and values are refused rather than truncated. */
    const auto big = modtest::make_mods_dir("big");
    modtest::write_mod(big.root, "hoarder",
        "function on_hour() calendar.store('k', string.rep('x', 600)) end\n");
    acserver::ModRegistry big_registry;
    CHECK(big_registry.scan(big.root, error));
    acserver::ModHost big_host;
    big_host.set_world(&world);
    CHECK(big_host.load_all(big_registry, acserver::ModLimits{}, error));
    CHECK(!big_host.call_hook("hoarder", "on_hour"));
    CHECK(big_host.last_error("hoarder").find("512") != std::string::npos);
}

/* Registration is load-time only. A mod that calls register from a hook would
 * be mutating a calendar that has already been resolved and replicated, so it
 * gets a clear error instead. */
void mod_registration_is_load_time_only() {
    const auto mods = modtest::make_mods_dir("latereg");
    modtest::write_mod(mods.root, "sneaky",
        "calendar.register { id = 'early', name = 'n', date = { month = 1, day = 1 } }\n"
        "function on_hour()\n"
        "  calendar.register { id = 'late', name = 'n', date = { month = 2, day = 2 } }\n"
        "end\n");

    modtest::FakeWorld world;
    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));
    acserver::ModHost host;
    host.set_world(&world);
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));
    CHECK(host.holidays().size() == 1);

    CHECK(!host.call_hook("sneaky", "on_hour"));
    CHECK(host.last_error("sneaky").find("while the mod is loading") != std::string::npos);
    CHECK(host.holidays().size() == 1);
}

/* The operator console's mod summary. This is the only view an operator has of
 * whether their mods are working, so the counts have to be right for the case
 * that actually happens: some mods fine, one broken. A summary that reported
 * three mods running when one had been quarantined would be worse than no
 * summary at all -- it would say the thing the operator wants to hear. */
void mod_summaries_report_what_is_running() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-mod-summary-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};

    const std::filesystem::path town = root / "town";
    std::filesystem::create_directories(town);
    modtest::write_mod(town / "mods", "aviary",
        "calendar.register { id = 'dawn', name = 'dawn_name', date = { month = 3, day = 2 } }\n"
        "calendar.register { id = 'dusk', name = 'dusk_name', date = { month = 3, day = 9 } }\n"
        "music.define { id = 'birdsong', name = 'birdsong_name', audio = 'birdsong.pcasset' }\n");
    modtest::write_mod(town / "mods", "brittle",
        "local nothing = nil\n"
        "return nothing.field\n");
    modtest::write_mod(town / "mods", "cellar",
        "calendar.register { id = 'vintage', name = 'vintage_name', date = { month = 9, day = 1 } }\n");

    acserver::TownRuntimeConfig config;
    config.port = 0;
    config.data_directory = town;
    config.connection_timeout_ms = 60000;
    config.invite_key = "summary-key";

    std::string error;
    acserver::TownRuntime server(config);
    CHECK(server.initialize(1700000000, error));

    const std::vector<acserver::RuntimeModSummary> summaries = server.mod_summaries();

    /* Every installed mod is listed, including the broken one. An operator
     * debugging a mod that "did nothing" must be able to tell a mod that failed
     * from one the server never found -- omitting the failure makes those two
     * situations print identically. */
    CHECK(summaries.size() == 3);

    /* Load order, ties broken by id -- never filesystem order. */
    CHECK(summaries[0].id == "aviary");
    CHECK(summaries[1].id == "brittle");
    CHECK(summaries[2].id == "cellar");

    CHECK(!summaries[0].quarantined);
    CHECK(summaries[0].version == "1.0.0");
    CHECK(summaries[0].holidays == 2);
    CHECK(summaries[0].songs == 1);
    CHECK(summaries[0].last_error.empty());

    CHECK(summaries[1].quarantined);
    /* The reason, not just the state: the console prints this, and a Lua error
     * naming a file and line is usually the whole diagnosis. */
    CHECK(!summaries[1].last_error.empty());
    /* A quarantined mod owns nothing. The host drops the declarations of a mod
     * it has stopped running, so these are not merely hidden -- that holiday is
     * genuinely not on the calendar, and the summary must not imply it is. */
    CHECK(summaries[1].holidays == 0);
    CHECK(summaries[1].songs == 0);

    CHECK(!summaries[2].quarantined);
    CHECK(summaries[2].holidays == 1);
    CHECK(summaries[2].songs == 0);

    CHECK(server.mod_quarantined_count() == 1);

    /* The counts the console prints agree with the lists the rest of the server
     * works from. Two independent tallies of the same thing drift apart exactly
     * when someone adds a third way to register something. */
    std::size_t counted_holidays = 0;
    std::size_t counted_songs = 0;
    for (const acserver::RuntimeModSummary& summary : summaries) {
        counted_holidays += summary.holidays;
        counted_songs += summary.songs;
    }
    CHECK(counted_holidays == 3);
    CHECK(counted_songs == 1);
    CHECK(server.loaded_mods().size() == 3);

    CHECK(server.shutdown(error));
}

/* A town with no mods installed must be indistinguishable from one built before
 * mods existed. The console keys off an empty summary to print nothing at all,
 * so "empty" has to mean empty rather than one placeholder entry. */
void mod_summaries_are_empty_without_mods() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-mod-nosummary-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::filesystem::create_directories(root);

    acserver::TownRuntimeConfig config;
    config.port = 0;
    config.data_directory = root;
    config.connection_timeout_ms = 60000;
    config.invite_key = "empty-key";

    std::string error;
    acserver::TownRuntime server(config);
    CHECK(server.initialize(1700000000, error));
    CHECK(server.mod_summaries().empty());
    CHECK(server.mod_quarantined_count() == 0);
    CHECK(server.shutdown(error));
}

/* The whole P2 path against a real TownRuntime: a mod on disk is discovered,
 * sandboxed, loaded, its holidays resolved against the authoritative clock, and
 * its persisted state survives a restart. */
void mod_calendar_drives_a_real_town() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-mod-town-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};

    const std::filesystem::path town = root / "town";
    std::filesystem::create_directories(town / "mods" / "lantern-night");
    {
        std::ofstream manifest(town / "mods" / "lantern-night" / "mod.toml");
        manifest << "id = \"lantern-night\"\nname = \"Lantern Night\"\nversion = \"1.0.0\"\n"
                 << "api_version = " << acserver::kModApiVersion << "\n";
    }
    {
        std::ofstream entry(town / "mods" / "lantern-night" / "init.lua");
        entry << "calendar.register {\n"
                 "  id = 'lantern_night', name = 'holiday_name',\n"
                 "  date = { month = 10, day = 7 },\n"
                 "  hours = { from = 18, to = 22 },\n"
                 "}\n"
                 "calendar.register {\n"
                 "  id = 'harvest', name = 'harvest_name',\n"
                 "  date = { month = 11, week = 4, weekday = 'thursday' },\n"
                 "}\n"
                 "function on_holiday_begin(id)\n"
                 "  calendar.store('last_begin', id)\n"
                 "end\n";
    }

    acserver::TownRuntimeConfig config;
    config.port = 0;
    config.data_directory = town;
    config.connection_timeout_ms = 60000;
    config.invite_key = "mod-town-key";
    /* A fixed clock parked inside the Lantern Night window, so the holiday is
     * live the moment the town starts. 2026-10-07 19:00 UTC. */
    config.clock.mode = acserver::ClockMode::Fixed;
    config.clock.sync_to_system_clock = false;
    config.clock.starting_town_unix_seconds = 1791399600;

    std::string error;
    {
        acserver::TownRuntime server(config);
        CHECK(server.initialize(1700000000, error));

        /* The mod loaded and its holidays resolved for the town's year. */
        CHECK(server.loaded_mods().size() == 1);
        CHECK(server.loaded_mods()[0] == "lantern-night");
        const std::vector<acserver::ResolvedHoliday>& calendar = server.mod_calendar();
        CHECK(calendar.size() == 2);
        CHECK(calendar[0].id == "lantern-night.lantern_night");
        CHECK(calendar[0].month == 10 && calendar[0].day == 7);
        CHECK(calendar[1].id == "lantern-night.harvest");
        CHECK(calendar[1].month == 11 && calendar[1].day == 26);   /* Thanksgiving 2026 */

        /* A digest over the mod set, which is what ServerHello will advertise. */
        const std::array<std::uint8_t, 32> digest = server.mod_manifest_digest();
        bool any = false;
        for (const std::uint8_t byte : digest) any = any || byte != 0;
        CHECK(any);

        /* Stepping past a minute boundary fires the begin edge. */
        for (int i = 0; i < 4; ++i) {
            CHECK(server.step(static_cast<std::uint64_t>(1000 + i * 30000), 1700000000 + i * 30, error));
        }
        CHECK(server.shutdown(error));
    }

    /* The town restarts, re-resolves the same calendar, and the mod's stored
     * state is still there. */
    {
        acserver::TownRuntime server(config);
        CHECK(server.initialize(1700001000, error));
        CHECK(server.mod_calendar().size() == 2);

        acserver::DatabaseStore database(town);
        CHECK(database.initialize(error));
        std::string stored;
        CHECK(database.get_mod_state("lantern-night", "last_begin", stored));
        CHECK(stored == "slantern_night");   /* 's' tag plus the unqualified id */
        CHECK(server.shutdown(error));
    }
}

/* Wire format for the mod calendar. The client indexes `strings` directly and
 * draws a marker from `flags`, so every bound here is one the decoder must
 * enforce rather than trust. */
void mod_calendar_replicates() {
    acnet::ModCalendarState state;
    state.revision = 7;
    state.year = 2026;
    state.manifest_digest.fill(0xAB);
    state.strings.push_back({ 'L', 'a', 'n', 't', 'e', 'r', 'n' });
    state.strings.push_back({ 'H', 'a', 'r', 'v', 'e', 's', 't' });

    acnet::ModHolidayEntry lantern;
    lantern.month = 10;
    lantern.day = 7;
    lantern.hour_from = 18;
    lantern.hour_to = 22;
    lantern.flags = acnet::kModHolidayMarker | acnet::kModHolidayLive;
    lantern.name_index = 0;
    state.holidays.push_back(lantern);

    acnet::ModHolidayEntry harvest;
    harvest.month = 11;
    harvest.day = 26;
    harvest.flags = acnet::kModHolidayMarker;
    harvest.name_index = 1;
    state.holidays.push_back(harvest);

    std::vector<std::uint8_t> encoded;
    CHECK(acnet::encode_mod_calendar(state, encoded));
    acnet::ModCalendarState decoded;
    CHECK(acnet::decode_mod_calendar(encoded, decoded));
    CHECK(decoded.revision == 7);
    CHECK(decoded.year == 2026);
    CHECK(decoded.manifest_digest == state.manifest_digest);
    CHECK(decoded.holidays.size() == 2);
    CHECK(decoded.holidays[0].month == 10 && decoded.holidays[0].day == 7);
    CHECK((decoded.holidays[0].flags & acnet::kModHolidayLive) != 0);
    CHECK((decoded.holidays[1].flags & acnet::kModHolidayLive) == 0);
    CHECK(decoded.strings.size() == 2);
    CHECK(decoded.strings[1].size() == 7);

    /* Full at both caps still round-trips. */
    acnet::ModCalendarState full;
    full.revision = 1;
    full.year = 2026;
    for (std::size_t i = 0; i < acnet::kMaxModStrings; ++i) {
        full.strings.push_back(std::vector<std::uint8_t>(acnet::kMaxModStringBytes, 'x'));
    }
    for (std::size_t i = 0; i < acnet::kMaxModHolidayEntries; ++i) {
        acnet::ModHolidayEntry entry;
        entry.month = 6;
        entry.day = 1;
        entry.name_index = static_cast<std::uint8_t>(i);
        full.holidays.push_back(entry);
    }
    CHECK(acnet::encode_mod_calendar(full, encoded));
    CHECK(acnet::decode_mod_calendar(encoded, decoded));
    CHECK(decoded.holidays.size() == acnet::kMaxModHolidayEntries);

    /* Refusals. Each of these is something a hostile or buggy peer could send. */
    acnet::ModCalendarState bad = state;
    bad.revision = 0;                                  /* zero revision is never valid */
    CHECK(!acnet::encode_mod_calendar(bad, encoded));

    bad = state;
    bad.holidays[0].month = 13;
    CHECK(!acnet::encode_mod_calendar(bad, encoded));

    bad = state;
    bad.holidays[0].hour_to = 3;                       /* window ends before it starts */
    CHECK(!acnet::encode_mod_calendar(bad, encoded));

    bad = state;
    bad.holidays[0].name_index = 9;                    /* dangling string index */
    CHECK(!acnet::encode_mod_calendar(bad, encoded));

    bad = state;
    bad.holidays.resize(acnet::kMaxModHolidayEntries + 1, lantern);
    CHECK(!acnet::encode_mod_calendar(bad, encoded));

    /* A reserved flag bit must be refused, so adding one later cannot be
     * mistaken for a flag an old client already understood. */
    CHECK(acnet::encode_mod_calendar(state, encoded));
    encoded[38] |= 0x80;                               /* first holiday's flags byte */
    CHECK(!acnet::decode_mod_calendar(encoded, decoded));

    /* Truncation at every length must be refused rather than read past. */
    CHECK(acnet::encode_mod_calendar(state, encoded));
    for (std::size_t cut = 0; cut < encoded.size(); ++cut) {
        std::vector<std::uint8_t> truncated(encoded.begin(), encoded.begin() + static_cast<long>(cut));
        acnet::ModCalendarState ignored;
        CHECK(!acnet::decode_mod_calendar(truncated, ignored));
    }
    /* Trailing junk is refused too -- the decoder must consume exactly. */
    CHECK(acnet::encode_mod_calendar(state, encoded));
    encoded.push_back(0);
    CHECK(!acnet::decode_mod_calendar(encoded, decoded));
}

/* Delivery wire format. Every bound here is one a hostile or buggy peer could
 * try to exceed, and the client stores what arrives on disk, so each is checked
 * on the way in as well as out. */
void asset_delivery_messages_round_trip() {
    std::vector<std::uint8_t> encoded;

    /* --- Manifest --- */
    acnet::AssetManifest manifest;
    manifest.revision = 3;
    manifest.manifest_digest.fill(0x5A);
    for (int i = 0; i < 4; ++i) {
        acnet::AssetManifestEntry entry;
        entry.hash.fill(static_cast<std::uint8_t>(i + 1));
        entry.size = 1024u * static_cast<std::uint32_t>(i + 1);
        entry.kind = static_cast<std::uint16_t>(i);
        entry.item_handle = static_cast<std::uint16_t>(0x3000 + i);
        manifest.entries.push_back(entry);
    }
    CHECK(acnet::encode(manifest, encoded));
    acnet::AssetManifest decoded_manifest;
    CHECK(acnet::decode(encoded, decoded_manifest));
    CHECK(decoded_manifest.revision == 3);
    CHECK(decoded_manifest.entries.size() == 4);
    CHECK(decoded_manifest.entries[2].size == 3072);
    CHECK(decoded_manifest.entries[3].item_handle == 0x3003);
    CHECK(decoded_manifest.manifest_digest == manifest.manifest_digest);

    /* Zero revision, a zero-sized blob, an oversized blob and an unknown kind
     * are each refused. */
    acnet::AssetManifest bad = manifest;
    bad.revision = 0;
    CHECK(!acnet::encode(bad, encoded));
    bad = manifest;
    bad.entries[0].size = 0;
    CHECK(!acnet::encode(bad, encoded));
    bad = manifest;
    bad.entries[0].size = static_cast<std::uint32_t>(acnet::kMaxAssetBlobBytes) + 1;
    CHECK(!acnet::encode(bad, encoded));
    bad = manifest;
    bad.entries[0].kind = 9;
    CHECK(!acnet::encode(bad, encoded));

    /* A manifest describing more total bytes than the client will store is
     * refused while summing, not after parsing the whole thing. */
    acnet::AssetManifest huge;
    huge.revision = 1;
    const std::size_t entries_to_overflow =
        static_cast<std::size_t>(acnet::kMaxManifestBytes / acnet::kMaxAssetBlobBytes) + 2;
    for (std::size_t i = 0; i < entries_to_overflow && i < acnet::kMaxAssetEntries; ++i) {
        acnet::AssetManifestEntry entry;
        entry.hash.fill(static_cast<std::uint8_t>(i));
        entry.size = static_cast<std::uint32_t>(acnet::kMaxAssetBlobBytes);
        huge.entries.push_back(entry);
    }
    CHECK(acnet::encode(huge, encoded));
    acnet::AssetManifest refused;
    CHECK(!acnet::decode(encoded, refused));

    /* --- Chunk request --- */
    acnet::AssetChunkRequest request;
    request.hash.fill(0x11);
    request.first_chunk = 7;
    request.chunk_count = static_cast<std::uint16_t>(acnet::kAssetWindowChunks);
    CHECK(acnet::encode(request, encoded));
    acnet::AssetChunkRequest decoded_request;
    CHECK(acnet::decode(encoded, decoded_request));
    CHECK(decoded_request.first_chunk == 7);
    CHECK(decoded_request.chunk_count == acnet::kAssetWindowChunks);

    /* A zero window can never complete; an oversized one is a client trying to
     * outrun its own pacing. */
    acnet::AssetChunkRequest bad_request = request;
    bad_request.chunk_count = 0;
    CHECK(!acnet::encode(bad_request, encoded));
    bad_request.chunk_count = static_cast<std::uint16_t>(acnet::kAssetWindowChunks + 1);
    CHECK(!acnet::encode(bad_request, encoded));

    /* ...and the decoder refuses them even if a peer hand-rolls the bytes. */
    CHECK(acnet::encode(request, encoded));
    encoded[encoded.size() - 2] = 0;
    encoded[encoded.size() - 1] = 0;
    CHECK(!acnet::decode(encoded, decoded_request));

    /* --- Chunk --- */
    acnet::AssetChunk chunk;
    chunk.hash.fill(0x22);
    chunk.index = 2;
    chunk.total_chunks = 5;
    chunk.bytes.assign(acnet::kAssetChunkBytes, 0xEE);
    CHECK(acnet::encode(chunk, encoded));
    /* One chunk must fit one packet, so delivery never involves fragmentation. */
    CHECK(encoded.size() <= acnet::kMaxPlaintextPayloadBytes);
    acnet::AssetChunk decoded_chunk;
    CHECK(acnet::decode(encoded, decoded_chunk));
    CHECK(decoded_chunk.index == 2);
    CHECK(decoded_chunk.total_chunks == 5);
    CHECK(decoded_chunk.bytes.size() == acnet::kAssetChunkBytes);
    CHECK(decoded_chunk.bytes[0] == 0xEE);

    /* An index outside the transfer, and a zero-length transfer, are refused --
     * both would have the client writing outside the blob it is assembling. */
    acnet::AssetChunk bad_chunk = chunk;
    bad_chunk.index = 5;
    CHECK(!acnet::encode(bad_chunk, encoded));
    bad_chunk = chunk;
    bad_chunk.total_chunks = 0;
    CHECK(!acnet::encode(bad_chunk, encoded));
    bad_chunk = chunk;
    bad_chunk.bytes.assign(acnet::kAssetChunkBytes + 1, 0);
    CHECK(!acnet::encode(bad_chunk, encoded));

    /* Truncation at every length, and trailing junk, are both refused. */
    CHECK(acnet::encode(chunk, encoded));
    for (std::size_t cut = 0; cut < encoded.size(); cut += 7) {
        std::vector<std::uint8_t> truncated(encoded.begin(), encoded.begin() + static_cast<long>(cut));
        acnet::AssetChunk ignored;
        CHECK(!acnet::decode(truncated, ignored));
    }
    CHECK(acnet::encode(chunk, encoded));
    encoded.push_back(0);
    CHECK(!acnet::decode(encoded, decoded_chunk));
}

/* The server's content store: what it will serve, how it chunks it, and what it
 * refuses. */
void asset_pack_store_serves_content() {
    const auto mods = modtest::make_mods_dir("packstore");

    /* Two mods, one of which ships a byte-identical copy of the other's
     * texture -- dedup is the main reason for content addressing. */
    std::filesystem::create_directories(mods.root / "alpha" / "content");
    std::filesystem::create_directories(mods.root / "beta" / "content");
    const std::string model(5000, 'M');       /* spans several chunks */
    const std::string shared(64, 'S');
    {
        std::ofstream(mods.root / "alpha" / "content" / "lantern.pcasset", std::ios::binary) << model;
        std::ofstream(mods.root / "alpha" / "content" / "shared.tex", std::ios::binary) << shared;
        std::ofstream(mods.root / "beta" / "content" / "copy.tex", std::ios::binary) << shared;
        /* Not under content/, so never served: a mod's source stays private. */
        std::ofstream(mods.root / "alpha" / "init.lua") << "-- private\n";
    }

    acserver::ModPackStore store;
    std::string error;
    CHECK(store.build(mods.root, error));

    /* Three files, two distinct blobs. */
    CHECK(store.blob_count() == 2);
    CHECK(store.manifest().entries.size() == 2);
    CHECK(store.total_bytes() == model.size() + shared.size());

    /* The manifest advertises a real digest and a non-zero revision. */
    CHECK(store.manifest().revision != 0);
    bool digest_set = false;
    for (const std::uint8_t byte : store.manifest().manifest_digest) digest_set = digest_set || byte != 0;
    CHECK(digest_set);

    /* Find the model's entry and reassemble it chunk by chunk, exactly as a
     * client would. */
    const acnet::AssetManifestEntry* model_entry = nullptr;
    for (const acnet::AssetManifestEntry& entry : store.manifest().entries) {
        if (entry.size == model.size()) model_entry = &entry;
    }
    CHECK(model_entry != nullptr);
    CHECK(model_entry->kind == 0);   /* .pcasset */

    const std::uint32_t chunks = store.chunk_count(model_entry->hash);
    CHECK(chunks > 1);
    std::vector<std::uint8_t> rebuilt;
    for (std::uint32_t i = 0; i < chunks; ++i) {
        std::vector<std::uint8_t> piece;
        CHECK(store.chunk(model_entry->hash, i, piece));
        CHECK(piece.size() <= acnet::kAssetChunkBytes);
        rebuilt.insert(rebuilt.end(), piece.begin(), piece.end());
    }
    CHECK(rebuilt.size() == model.size());
    CHECK(std::equal(rebuilt.begin(), rebuilt.end(), model.begin()));

    /* What arrives must hash to what was advertised -- that is the whole
     * integrity story, and it is why no signature scheme is needed. */
    CHECK(acnet::sha256(rebuilt.data(), rebuilt.size()) == model_entry->hash);

    /* A chunk past the end, and an unknown hash, are both refused rather than
     * served as empty. */
    std::vector<std::uint8_t> ignored;
    CHECK(!store.chunk(model_entry->hash, chunks, ignored));
    std::array<std::uint8_t, 32> unknown{};
    unknown.fill(0xAB);
    CHECK(!store.chunk(unknown, 0, ignored));
    CHECK(store.chunk_count(unknown) == 0);

    /* A town with no mods at all still builds a valid, empty manifest. */
    acserver::ModPackStore empty;
    CHECK(empty.build(mods.root / "definitely-absent", error));
    CHECK(empty.blob_count() == 0);
    CHECK(empty.manifest().revision != 0);

    /* An oversized asset is refused at build time, where an operator sees it,
     * rather than at serve time where a player would. */
    const auto oversized = modtest::make_mods_dir("packbig");
    std::filesystem::create_directories(oversized.root / "huge" / "content");
    {
        std::ofstream out(oversized.root / "huge" / "content" / "big.pcasset", std::ios::binary);
        const std::string block(64 * 1024, 'X');
        for (std::size_t written = 0; written <= acnet::kMaxAssetBlobBytes; written += block.size()) {
            out << block;
        }
    }
    acserver::ModPackStore refused;
    CHECK(!refused.build(oversized.root, error));
    CHECK(error.find("per-asset limit") != std::string::npos);
}

/* Custom songs. The binding constraint is the stereo's 64-bit music_box with
 * 55 bits already spoken for, so the interesting behaviour is all around what
 * happens at nine. */
void mod_music_defines_custom_songs() {
    const auto mods = modtest::make_mods_dir("music");
    modtest::write_mod(mods.root, "lantern-night",
        "slot_a = music.define { id = 'lantern_waltz', name = 'song_a', audio = 'waltz.ogg' }\n"
        "slot_b = music.define { id = 'lantern_march', name = 'song_b', audio = 'march.ogg' }\n"
        "function on_hour() end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    modtest::FakeWorld world;
    acserver::ModHost host;
    host.set_world(&world);
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));
    CHECK(!host.quarantined("lantern-night"));

    const std::vector<acserver::CustomSong>& songs = host.music().songs();
    CHECK(songs.size() == 2);
    /* Slots start immediately after the vanilla minidisks, in registration
     * order, so the same mod set yields the same ids every start -- which
     * matters because the id is what a saved music_box holds. */
    CHECK(songs[0].slot == acserver::kVanillaSongCount);
    CHECK(songs[1].slot == acserver::kVanillaSongCount + 1);
    CHECK(songs[0].id == "lantern-night.lantern_waltz");
    CHECK(songs[0].audio == "waltz.ogg");
    CHECK(host.music().remaining() == acserver::kMaxCustomSongs - 2);
}

/* Nine is the cap, and it comes from the bitfield rather than from this code.
 * The tenth definition must fail with an error that explains why. */
void mod_music_respects_the_music_box_limit() {
    acserver::ModMusicRegistry registry;
    std::string error;

    for (int i = 0; i < acserver::kMaxCustomSongs; ++i) {
        const std::string id = "mod.song" + std::to_string(i);
        CHECK(registry.define(id, "name", "a.ogg", error));
    }
    CHECK(registry.songs().size() == acserver::kMaxCustomSongs);
    CHECK(registry.remaining() == 0);
    /* The last slot is the top bit of the field. */
    CHECK(registry.songs().back().slot == acserver::kMusicBoxBits - 1);

    CHECK(!registry.define("mod.overflow", "name", "a.ogg", error));
    CHECK(error.find("music_box") != std::string::npos);

    /* Duplicate ids and unsafe audio names are refused. */
    acserver::ModMusicRegistry fresh;
    CHECK(fresh.define("mod.one", "name", "a.ogg", error));
    CHECK(!fresh.define("mod.one", "name", "b.ogg", error));
    CHECK(error.find("duplicate") != std::string::npos);
    CHECK(!fresh.define("mod.two", "name", "../../etc/passwd", error));
    CHECK(error.find("bare filename") != std::string::npos);
    CHECK(!fresh.define("mod.three", "name", "", error));
}

/* A quarantined mod loses its songs, and the slots it held are NOT handed to
 * anyone else -- renumbering would make every stereo already holding one play
 * something different. */
void mod_music_slots_are_never_reused() {
    acserver::ModMusicRegistry registry;
    std::string error;

    CHECK(registry.define("alpha.one", "name", "a.ogg", error));
    CHECK(registry.define("beta.one", "name", "b.ogg", error));
    const std::uint8_t beta_slot = registry.songs()[1].slot;

    registry.drop_by_owner("alpha");
    CHECK(registry.songs().size() == 1);
    /* beta keeps the bit it was given. */
    CHECK(registry.songs()[0].id == "beta.one");
    CHECK(registry.songs()[0].slot == beta_slot);

    /* A song defined afterwards gets a fresh slot, not alpha's. */
    CHECK(registry.define("gamma.one", "name", "c.ogg", error));
    CHECK(registry.songs()[1].slot == beta_slot + 1);
    CHECK(registry.slot_of("alpha.one") == -1);
}

/* music.grant routes through the same authority the operator command uses. */
void mod_music_grant_routes_through_the_runtime() {
    const auto mods = modtest::make_mods_dir("musicgrant");
    modtest::write_mod(mods.root, "dj",
        "music.define { id = 'tune', name = 'song', audio = 'tune.ogg' }\n"
        "function on_hour() music.grant(0, 55) end\n"
        "function on_day_start() music.grant(9, 55) end\n");

    acserver::ModRegistry registry;
    std::string error;
    CHECK(registry.scan(mods.root, error));

    modtest::FakeWorld world;
    acserver::ModHost host;
    host.set_world(&world);
    CHECK(host.load_all(registry, acserver::ModLimits{}, error));

    CHECK(host.call_hook("dj", "on_hour"));
    CHECK(world.song_grants.size() == 1);
    CHECK(world.song_grants[0].first == 0);
    CHECK(world.song_grants[0].second == 55);

    /* An out-of-range house slot is refused at the binding, before it reaches
     * the runtime. */
    CHECK(!host.call_hook("dj", "on_day_start"));
    CHECK(world.song_grants.size() == 1);
    CHECK(host.last_error("dj").find("house slot") != std::string::npos);

    /* Registration is load-time only, exactly like calendar.register. */
    const auto late = modtest::make_mods_dir("musiclate");
    modtest::write_mod(late.root, "sneaky",
        "function on_hour()\n"
        "  music.define { id = 'late', name = 'n', audio = 'a.ogg' }\n"
        "end\n");
    acserver::ModRegistry late_registry;
    CHECK(late_registry.scan(late.root, error));
    acserver::ModHost late_host;
    late_host.set_world(&world);
    CHECK(late_host.load_all(late_registry, acserver::ModLimits{}, error));
    CHECK(!late_host.call_hook("sneaky", "on_hour"));
    CHECK(late_host.last_error("sneaky").find("while the mod is loading") != std::string::npos);
    CHECK(late_host.music().songs().empty());
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
    /* The key survives the round trip, semicolon and all, so migrating an older
     * configuration does not silently leave a server that refuses to start. */
    CHECK(generated_config.invite_key == config.invite_key);
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
    original.native_fruit = 10243; // ITM_FOOD_PEACH
    original.town_name = {{'W', 'i', 'n', 'n', 'i', 'p', 'e', 'g'}};
    original.appearance.name = {{'R', 'e', 's', 'i', 'd', 'e', 'n', 't'}};
    original.appearance.gender = 1;
    original.appearance.face = 6;
    original.appearance.clothing = 0xFE20;
    original.appearance.clothing_index = 0x103;
    original.pattern.present = true;
    original.pattern.palette = 5;
    for (std::size_t i = 0; i < original.pattern.texture.size(); ++i)
        original.pattern.texture[i] = static_cast<std::uint8_t>(i * 37U);
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
    CHECK(decoded.native_fruit == original.native_fruit);
    CHECK(decoded.town_name == original.town_name);
    CHECK(decoded.appearance.name == original.appearance.name);
    CHECK(decoded.appearance.clothing == original.appearance.clothing);
    CHECK(decoded.appearance.clothing_index == 0x103);
    CHECK(decoded.pattern.present);
    CHECK(decoded.pattern.palette == 5);
    CHECK(decoded.pattern.texture == original.pattern.texture);
    CHECK(decoded.tiles.size() == acnet::kTownBootstrapTileCount);
    CHECK(decoded.tiles[17].buried);
    CHECK(decoded.tiles[1234].item == original.tiles[1234].item);
    /* A client that could not read the acre layout sends no island section at
     * all, and that has to survive the round trip as "no island reported"
     * rather than as an island at acre zero. */
    CHECK(decoded.island_tiles.empty());

    original.island_block_x = {{4, 5}};
    original.island_tiles.resize(acnet::kIslandBootstrapTileCount);
    for (std::size_t i = 0; i < original.island_tiles.size(); ++i) {
        original.island_tiles[i].item = static_cast<std::uint16_t>(0x4000U + i);
        original.island_tiles[i].buried = (i % 5U) == 0;
    }
    CHECK(acnet::encode(original, payload));
    CHECK(acnet::decode(payload, decoded));
    CHECK(decoded.island_tiles.size() == acnet::kIslandBootstrapTileCount);
    CHECK(decoded.island_block_x[0] == 4 && decoded.island_block_x[1] == 5);
    CHECK(decoded.island_tiles[10].item == original.island_tiles[10].item);
    CHECK(decoded.island_tiles[5].buried);

    /* A partial island, an out-of-range acre, or acres in the wrong order are
     * all refused: the server would otherwise install an island somewhere the
     * client's field does not have one. */
    acnet::TownBootstrap malformed = original;
    malformed.island_tiles.pop_back();
    CHECK(!acnet::encode(malformed, payload));
    malformed = original;
    malformed.island_block_x = {{4, acnet::kFieldBlockXCount}};
    CHECK(!acnet::encode(malformed, payload));
    malformed = original;
    malformed.island_block_x = {{5, 4}};
    CHECK(!acnet::encode(malformed, payload));

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
    for (std::uint64_t i = 0; i < acnet::kMaxPlayersPerZone; ++i) {
        acnet::PlayerSnapshot player;
        player.entity = 0x100000001ULL + i;
        player.account = 100 + i;
        player.zone = 1;
        player.acknowledged_input = static_cast<std::uint32_t>(50 + i);
        player.transform.position = {static_cast<float>(i), 2.0F, static_cast<float>(i * 3)};
        player.transform.velocity = {0.1F, 0.0F, -0.2F};
        player.transform.yaw = static_cast<std::int16_t>(i * 100);
        player.transform.action = static_cast<std::uint16_t>(120 + i);
        player.appearance.clothing_index = i == 3 ? 0x103 : 3;
        player.appearance.revision = 9;
        if (i == 3) {
            player.pattern.present = true;
            player.pattern.palette = 5;
            player.pattern.texture.fill(0xA5);
        }
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
    CHECK(decoded.players.size() == acnet::kMaxPlayersPerZone);
    CHECK(decoded.players[7].account == 107);
    CHECK(decoded.house_light_mask == 0x5);
    CHECK(decoded.players[7].transition_phase == acnet::DoorTransitionPhase::Arriving);
    CHECK(decoded.players[7].transition_door == 202);
    CHECK(same_float(decoded.players[7].transform.position.z, 21.0F));
    CHECK(decoded.players[7].transform.action == 127);
    CHECK(decoded.players[3].appearance.clothing_index == 0);
    CHECK(decoded.players[3].appearance.revision == 9);
    /* The 512-byte bitmap rides reliable baselines/appearance updates, never
     * the high-rate transform stream. */
    CHECK(!decoded.players[3].pattern.present);
    CHECK(bytes.size() <= acnet::kMaxPlaintextPayloadBytes);
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
    a.action = 4;
    acnet::Transform b = a;
    b.position.x = 10.0F;
    b.action = 5;
    CHECK(history.push(10, a));
    CHECK(history.push(20, b));
    CHECK(!history.push(19, b));
    const auto midpoint = history.sample(15.0);
    CHECK(midpoint.has_value());
    CHECK(same_float(midpoint->position.x, 5.0F));
    CHECK(midpoint->action == 5);
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
    /* Owning the shovel is not enough while it is still in slot 1. */
    CHECK(world.apply(dig).code == acnet::ResultCode::InvalidState);
    tools.slots[1] = {};
    tools.equipped.item = 0x2202;
    CHECK(world.set_inventory(1, tools));
    dig.idempotency = {1, 7};
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
    plant.expected_item = 0x2900;
    CHECK(world.apply(plant).code == acnet::ResultCode::Ok);
    CHECK(world.tile(unrelated)->terrain == acnet::TerrainState::Planted);
    CHECK(world.tile(unrelated)->item == 0x0800);
}

/* The generated price table is swept out of the game's own
 * mSP_ItemNo2ItemPrice, so these pins are the values the shop dialogue quotes.
 * They cover each shape the original prices differently: the flat per-category
 * tables, the shellfish table, the signboard constant, and the two entries
 * that depend on town state rather than a table. */
void shop_prices_match_the_original_tables() {
    constexpr std::uint16_t kNet = 8704;
    constexpr std::uint16_t kAxe = 8705;
    constexpr std::uint16_t kShovel = 8706;
    constexpr std::uint16_t kRod = 8707;
    constexpr std::uint16_t kShell = 9492;
    constexpr std::uint16_t kSignboard = 9502;
    constexpr std::uint16_t kApple = 10240;
    constexpr std::uint16_t kCherry = 10241;
    constexpr std::uint16_t kGrabBag = 11776;

    CHECK(acnet::shop_item_price(kNet) == 500);
    CHECK(acnet::shop_item_price(kAxe) == 400);
    CHECK(acnet::shop_item_price(kShovel) == 500);
    CHECK(acnet::shop_item_price(kRod) == 500);
    CHECK(acnet::shop_item_price(kShell) == 160);
    CHECK(acnet::shop_item_price(kSignboard) == 500);

    /* Nook pays a quarter of the shelf price. */
    CHECK(acnet::shop_sell_price(kRod) == 125);
    CHECK(acnet::shop_sell_price(kShell) == 40);

    /* Fruit is cheap only at home. */
    CHECK(acnet::shop_item_price(kApple, kApple) == 400);
    CHECK(acnet::shop_item_price(kApple, kCherry) == 2000);
    CHECK(acnet::shop_item_price(kApple) == 2000);
    CHECK(acnet::shop_item_price(kCherry, kCherry) == 400);

    /* The grab bag costs the year, and is worth nothing without one. */
    CHECK(acnet::shop_item_price(kGrabBag, 0, 2001) == 2001);
    CHECK(acnet::shop_item_price(kGrabBag) == 0);

    /* Nothing, and an id the game does not price, are both free. */
    CHECK(acnet::shop_item_price(0) == 0);
    CHECK(acnet::shop_item_price(1) == 0);
    CHECK(acnet::shop_sell_price(0) == 0);

    /* Every item the shop can actually stock has to have a price, or the
     * shelf would show something that cannot be bought. */
    acnet::ShopStockState state;
    state.tier = acnet::ShopTier::DepartmentStore;
    std::uint64_t counter = 0;
    const auto sequence = [&counter]() { return counter++ * 2654435761U + 12345U; };
    acnet::shop_randomise_priorities(state, sequence);
    const std::vector<acnet::ShopEntry> stock = acnet::roll_shop_stock(state, sequence);
    CHECK(!stock.empty());
    for (const acnet::ShopEntry& entry : stock) {
        CHECK(entry.item != 0);
        CHECK(entry.price != 0);
        CHECK(entry.price == acnet::shop_item_price(entry.item));
    }
}

/* The shelf has to be the *whole* shelf. A Buy names a row by index, so if the
 * server rolled only the rarity-list draws and the game appended tools and
 * plants locally, the two would disagree about what index 12 is. */
void shop_shelf_is_the_whole_shelf() {
    constexpr std::uint16_t kShovel = 8706;
    constexpr std::uint16_t kSignboard = 9502;
    constexpr std::uint16_t kUmbrellaFirst = 8708;
    constexpr std::uint16_t kSapling = 10496;
    constexpr std::uint16_t kCedarSapling = 10497;
    constexpr std::uint16_t kPansyBag = 10498;
    constexpr std::uint16_t kRedPaint = 8749;

    std::uint64_t counter = 0;
    const auto sequence = [&counter]() { return counter++ * 6364136223846793005ULL + 1442695040888963407ULL; };
    const auto has = [](const std::vector<acnet::ShopEntry>& stock, std::uint16_t item) {
        return std::any_of(stock.begin(), stock.end(),
                           [item](const acnet::ShopEntry& e) { return e.item == item; });
    };
    const auto count_in_range = [](const std::vector<acnet::ShopEntry>& stock, std::uint16_t low,
                                   std::uint16_t high) {
        return std::count_if(stock.begin(), stock.end(), [low, high](const acnet::ShopEntry& e) {
            return e.item >= low && e.item <= high;
        });
    };

    /* Nookington's stocks every tool, a paint, a signboard, an umbrella, a
     * cedar sapling with plain ones behind it, and five distinct flower bags. */
    acnet::ShopStockState top;
    top.tier = acnet::ShopTier::DepartmentStore;
    acnet::shop_randomise_priorities(top, sequence);
    const std::vector<acnet::ShopEntry> shelf = acnet::roll_shop_stock(top, sequence);
    CHECK(has(shelf, kShovel));
    CHECK(has(shelf, kSignboard));
    CHECK(has(shelf, kCedarSapling));
    CHECK(has(shelf, kSapling));
    CHECK(count_in_range(shelf, kRedPaint, kRedPaint + 11) == 1);
    CHECK(count_in_range(shelf, kUmbrellaFirst, kUmbrellaFirst + 31) == 1);
    /* Five plants, all different -- the original never repeats a flower. */
    CHECK(count_in_range(shelf, kPansyBag, kPansyBag + 8) == 5);
    CHECK(shelf.size() <= acnet::kShopMaximumGoods);

    /* The paint colour advances one step per roll and wraps after twelve. */
    acnet::ShopStockState rotating;
    rotating.tier = acnet::ShopTier::DepartmentStore;
    acnet::shop_randomise_priorities(rotating, sequence);
    std::vector<std::uint16_t> seen;
    for (int day = 0; day < 13; ++day) {
        const std::vector<acnet::ShopEntry> today = acnet::roll_shop_stock(rotating, sequence);
        for (const acnet::ShopEntry& entry : today) {
            if (entry.item >= kRedPaint && entry.item <= kRedPaint + 11) seen.push_back(entry.item);
        }
    }
    CHECK(seen.size() == 13);
    for (std::size_t i = 0; i < 12; ++i) CHECK(seen[i] == kRedPaint + i);
    CHECK(seen[12] == kRedPaint); // wrapped

    /* Nook's Cranny gates tools on lifetime sales, and no paint or signboard. */
    acnet::ShopStockState cranny;
    cranny.tier = acnet::ShopTier::Zakka;
    acnet::shop_randomise_priorities(cranny, sequence);
    const std::vector<acnet::ShopEntry> new_shop = acnet::roll_shop_stock(cranny, sequence);
    CHECK(has(new_shop, kShovel));            // the only tool a new store sells
    CHECK(count_in_range(new_shop, 8704, 8707) == 1);
    CHECK(!has(new_shop, kSignboard));
    CHECK(!has(new_shop, kCedarSapling));     // Nookway and above only

    cranny.sales_sum = 12000; // past the axe threshold
    const std::vector<acnet::ShopEntry> earned = acnet::roll_shop_stock(cranny, sequence);
    CHECK(count_in_range(earned, 8704, 8707) == 2); // l_zakka_goods stocks two
}

/* A sale pays what the shop dialogue quotes. Without a resolver installed the
 * authority has no price table of its own, so nothing is sellable -- which is
 * what the server did before the resolver existed. */
void selling_pays_the_generated_price() {
    constexpr std::uint16_t kRod = 8707;    // 500 new, so 125 secondhand
    constexpr std::uint16_t kApple = 10240;
    constexpr std::uint16_t kCherry = 10241;
    constexpr std::uint16_t kUnpriced = 1;

    acnet::PlayerDirectory players;
    acnet::WorldAuthority world(&players);
    acnet::InventoryState inventory;
    inventory.slots[0].item = kRod;
    inventory.slots[1].item = kApple;
    inventory.slots[2].item = kCherry;
    inventory.slots[3].item = kUnpriced;
    CHECK(world.register_inventory(1, inventory));
    acnet::EconomyAuthority economy(&world);
    CHECK(economy.register_account(1));

    acnet::EconomyRequest sell;
    sell.type = acnet::EconomyOpType::Sell;
    sell.account = 1;

    /* No resolver yet: an item with a real price still cannot be sold. */
    sell.idempotency = {1, 1};
    sell.inventory_slot = 0;
    sell.expected_inventory_revision = 1;
    CHECK(economy.apply(sell).code == acnet::ResultCode::InvalidState);

    /* A town that grows cherries pays a premium for the apple. */
    economy.set_sell_price_resolver(
        [](std::uint16_t item) { return acnet::shop_sell_price(item, kCherry); });

    sell.idempotency = {1, 2};
    sell.inventory_slot = 0;
    CHECK(economy.apply(sell).code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->bells == 125);

    sell.idempotency = {1, 3};
    sell.inventory_slot = 1; // foreign apple, 2000 / 4
    sell.expected_inventory_revision = world.inventory(1)->revision;
    CHECK(economy.apply(sell).code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->bells == 125 + 500);

    sell.idempotency = {1, 4};
    sell.inventory_slot = 2; // the town's own cherry, 400 / 4
    sell.expected_inventory_revision = world.inventory(1)->revision;
    CHECK(economy.apply(sell).code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->bells == 125 + 500 + 100);

    /* Something the game does not price is refused rather than given away. */
    sell.idempotency = {1, 5};
    sell.inventory_slot = 3;
    sell.expected_inventory_revision = world.inventory(1)->revision;
    CHECK(economy.apply(sell).code == acnet::ResultCode::InvalidState);
    CHECK(world.inventory(1)->slots[3].item == kUnpriced);

    /* An explicit override still wins, which is how a test prices by hand. */
    economy.set_sell_price(kUnpriced, 7);
    sell.idempotency = {1, 6};
    CHECK(economy.apply(sell).code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->bells == 125 + 500 + 100 + 7);
}

/* The shelf is town-wide state: a purchase has to reach everyone, and the index
 * a Buy names has to mean the same row on both sides. */
void shop_tier_is_earned_and_server_owned() {
    acnet::ShopStockState state;
    CHECK(state.tier == acnet::ShopTier::Zakka);

    /* mSP_PlusSales clamps at the next tier's threshold, so one enormous
     * purchase cannot skip a tier -- the store climbs one step at a time. */
    acnet::shop_add_sales(state, 1000000);
    CHECK(state.sales_sum == acnet::kShopCombiniSalesSum);
    CHECK(acnet::shop_earned_tier(state) == acnet::ShopTier::Conbini);

    state.tier = acnet::ShopTier::Conbini;
    acnet::shop_add_sales(state, 1000000);
    CHECK(state.sales_sum == acnet::kShopSuperSalesSum);
    CHECK(acnet::shop_earned_tier(state) == acnet::ShopTier::Super);

    state.tier = acnet::ShopTier::Super;
    acnet::shop_add_sales(state, 1000000);
    CHECK(state.sales_sum == acnet::kShopDepartmentSalesSum);
    /* Nookington's also needs somebody from outside the town to have shopped,
     * so the total alone is not enough. */
    CHECK(acnet::shop_earned_tier(state) == acnet::ShopTier::Super);
    state.visitor_shopped = true;
    CHECK(acnet::shop_earned_tier(state) == acnet::ShopTier::DepartmentStore);

    /* The top tier has nothing above it to clamp against, and the total must
     * saturate rather than wrap -- nothing bounds what a transaction is worth. */
    state.tier = acnet::ShopTier::DepartmentStore;
    acnet::shop_add_sales(state, 0xFFFFFFFFu);
    CHECK(state.sales_sum == 0xFFFFFFFFu);
    acnet::shop_add_sales(state, 5000);
    CHECK(state.sales_sum == 0xFFFFFFFFu);

    /* Both fields ride the shelf, so a viewer learns about an upgrade. */
    acnet::ShopState shop;
    shop.revision = 4;
    shop.tier = static_cast<std::uint8_t>(acnet::ShopTier::Super);
    shop.sales_sum = 91000;
    shop.stock = {{4104, 1200, 1}};
    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode_shop_delta(shop, payload));
    acnet::ShopState decoded;
    CHECK(acnet::decode_shop_delta(payload, decoded));
    CHECK(decoded.tier == shop.tier);
    CHECK(decoded.sales_sum == 91000);

    /* A tier the game does not define is a decode failure, not a value the
     * client has to range-check before indexing its shelf-size table. */
    acnet::ShopState bad = shop;
    bad.tier = 4;
    CHECK(!acnet::encode_shop_delta(bad, payload));
}

void turnip_market_is_town_state() {
    /* Every trend, many weeks, from a deterministic stream: the schedule must
     * always be usable, because a zero price is what the economy reads as
     * "unsellable" and is exactly the bug this replaces. */
    std::mt19937_64 rng(20260807);
    const auto unit = [&rng]() -> double {
        return static_cast<double>(rng() >> 11) / 9007199254740992.0;
    };
    acnet::TurnipMarket market;
    bool saw_spike = false;
    bool saw_falling = false;
    for (int week = 0; week < 400; ++week) {
        acnet::roll_turnip_week(market, unit);
        CHECK(market.trend < acnet::kTurnipTrendCount);
        /* Kabu_decide_price_sunday: [0.7, 1.3) * 100. */
        CHECK(market.daily_price[0] >= 70 && market.daily_price[0] < 130);
        for (std::uint16_t price : market.daily_price) {
            CHECK(price != 0);
            CHECK(price <= acnet::kTurnipPriceMaximum);
        }
        if (market.trend == 0) saw_spike = true;
        if (market.trend == 2) saw_falling = true;
    }
    /* The trend walk must actually reach every branch, or the odds table is
     * transcribed wrong in a way no single roll would show. */
    CHECK(saw_spike);
    CHECK(saw_falling);

    /* aNSC_kabu_sum {10, 50, 100, 0}, multiplied by the day's price and *not*
     * divided by the sell/buy ratio -- turnips bypass it in the original. */
    acnet::TurnipMarket fixed;
    fixed.daily_price = {{100, 111, 122, 133, 144, 155, 166}};
    CHECK(acnet::turnip_sell_price(fixed, 0x2F00, 1) == 1110);
    CHECK(acnet::turnip_sell_price(fixed, 0x2F01, 1) == 5550);
    CHECK(acnet::turnip_sell_price(fixed, 0x2F02, 3) == 13300);
    /* A spoiled turnip is worth nothing, on any day. */
    CHECK(acnet::turnip_sell_price(fixed, 0x2F03, 3) == 0);
    /* Not a turnip, and an out-of-range weekday. */
    CHECK(acnet::turnip_sell_price(fixed, 0x1000, 1) == 0);
    CHECK(acnet::turnip_sell_price(fixed, 0x2F00, 7) == 0);
    CHECK(acnet::turnip_sell_price(fixed, 0x2F00, -1) == 0);

    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode_turnip_delta(fixed, payload));
    acnet::TurnipMarket decoded;
    CHECK(acnet::decode_turnip_delta(payload, decoded));
    CHECK(decoded == fixed);

    /* A trend the game does not define, and a price beyond Kabu_PRICE_MAX, are
     * both decode failures rather than values a viewer has to sanity-check. */
    acnet::TurnipMarket bad = fixed;
    bad.trend = acnet::kTurnipTrendCount;
    CHECK(!acnet::encode_turnip_delta(bad, payload));
    bad = fixed;
    bad.daily_price[2] = acnet::kTurnipPriceMaximum + 1;
    CHECK(!acnet::encode_turnip_delta(bad, payload));

    /* Sunday is weekday 0, matching lbRTC_SUNDAY -- 1970-01-01 was a Thursday,
     * and 2026-08-09 is a Sunday. */
    CHECK(acnet::town_date_from_seconds(0).weekday == 4);
    CHECK(acnet::town_date_from_seconds(1786233600).weekday == 0); // 2026-08-09
    CHECK(acnet::town_date_from_seconds(1786060800).weekday == 5); // 2026-08-07
    /* Before the epoch the day division has to floor, not truncate toward
     * zero, or the weekday walks backwards by one for every negative day. */
    CHECK(acnet::town_date_from_seconds(-86400).weekday == 3); // 1969-12-31, a Wednesday
}

void shop_shelf_replicates_town_wide() {
    acnet::ShopState shop;
    shop.revision = 9;
    shop.stock = {{4104, 1200, 1}, {8707, 500, 2}, {10240, 400, 0}};

    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode_shop_delta(shop, payload));
    acnet::ShopState decoded;
    CHECK(acnet::decode_shop_delta(payload, decoded));
    CHECK(decoded.revision == shop.revision);
    CHECK(decoded.stock.size() == shop.stock.size());
    for (std::size_t i = 0; i < shop.stock.size(); ++i) {
        CHECK(decoded.stock[i].item == shop.stock[i].item);
        CHECK(decoded.stock[i].price == shop.stock[i].price);
        CHECK(decoded.stock[i].quantity == shop.stock[i].quantity);
    }

    /* A sold-out row keeps its slot so later indices do not shift. */
    CHECK(decoded.stock[2].quantity == 0);

    /* Revision zero is not a shelf, and trailing bytes are a malformed one. */
    acnet::ShopState empty_revision;
    empty_revision.revision = 0;
    std::vector<std::uint8_t> rejected;
    CHECK(!acnet::encode_shop_delta(empty_revision, rejected));
    payload.push_back(0);
    CHECK(!acnet::decode_shop_delta(payload, decoded));
    payload.pop_back();
    payload.pop_back();
    CHECK(!acnet::decode_shop_delta(payload, decoded));

    /* An oversized shelf is refused rather than truncated. */
    acnet::ShopState oversized;
    oversized.revision = 1;
    oversized.stock.resize(4096);
    CHECK(!acnet::encode_shop_delta(oversized, rejected));

    /* Town-wide means it reaches a viewer standing anywhere, unlike a tile. */
    acnet::DeltaLog log;
    acnet::ReplicationDelta delta;
    delta.kind = acnet::ResourceKind::Shop;
    delta.zone = 0;
    delta.target_account = 0;
    CHECK(acnet::encode_shop_delta(shop, delta.payload));
    log.append(delta);
    acnet::InterestContext elsewhere;
    elsewhere.account = 77;
    elsewhere.zone = 104; // inside someone's house, nowhere near the shop
    const acnet::DeltaQueryResult visible = log.since(0, elsewhere, 16);
    CHECK(visible.deltas.size() == 1);
    CHECK(visible.deltas[0].kind == acnet::ResourceKind::Shop);
    acnet::ShopState received;
    CHECK(acnet::decode_shop_delta(visible.deltas[0].payload, received));
    CHECK(received.stock.size() == shop.stock.size());
}

/* The counter sells a whole selection for one quoted total, so the transaction
 * has to be one atomic request -- and the wallet cannot hold more than the cap,
 * with the excess coming back as money bags. */
void selling_a_selection_is_atomic_and_caps_the_wallet() {
    constexpr std::uint16_t kRod = 8707;      // sells for 125
    constexpr std::uint16_t kUnpriced = 1;
    const acnet::WalletOverflowRule rule = acnet::shop_wallet_overflow_rule();
    CHECK(rule.maximum == 99999);
    CHECK(rule.chunk == 30000);

    acnet::PlayerDirectory players;
    acnet::WorldAuthority world(&players);
    acnet::InventoryState inventory;
    inventory.slots[0].item = kRod;
    inventory.slots[1].item = kRod;
    inventory.slots[2].item = kRod;
    CHECK(world.register_inventory(1, inventory));

    acnet::EconomyConfig config;
    config.wallet_maximum = rule.maximum;
    config.wallet_overflow_chunk = rule.chunk;
    config.wallet_overflow_item = rule.bag_item;
    /* No player directory: standing in the shop is validated elsewhere, and
     * this case is about what the sale does to the pockets and the wallet. */
    acnet::EconomyAuthority economy(&world, nullptr, config);
    CHECK(economy.register_account(1));
    economy.set_sell_price_resolver([](std::uint16_t item) { return acnet::shop_sell_price(item); });

    /* Three rods in one request: one transaction, one total. */
    acnet::EconomyRequest sell;
    sell.type = acnet::EconomyOpType::Sell;
    sell.account = 1;
    sell.idempotency = {2, 1};
    sell.slot_mask = 0b111;
    sell.expected_inventory_revision = 1;
    const acnet::EconomyResult sold = economy.apply(sell);
    CHECK(sold.code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->bells == 375);
    for (std::size_t i = 0; i < 3; ++i) CHECK(world.inventory(1)->slots[i].item == 0);

    /* Replaying the same key must not pay twice. */
    const acnet::EconomyResult replay = economy.apply(sell);
    CHECK(replay.replayed);
    CHECK(world.inventory(1)->bells == 375);

    /* One unsellable pocket in the selection voids the whole sale. */
    acnet::InventoryState mixed;
    mixed.slots[0].item = kRod;
    mixed.slots[1].item = kUnpriced;
    CHECK(world.register_inventory(2, mixed));
    CHECK(economy.register_account(2));
    acnet::EconomyRequest partial;
    partial.type = acnet::EconomyOpType::Sell;
    partial.account = 2;
    partial.idempotency = {2, 2};
    partial.slot_mask = 0b11;
    partial.expected_inventory_revision = 1;
    CHECK(economy.apply(partial).code == acnet::ResultCode::InvalidState);
    CHECK(world.inventory(2)->slots[0].item == kRod);   // nothing moved
    CHECK(world.inventory(2)->slots[1].item == kUnpriced);
    CHECK(world.inventory(2)->bells == 0);

    /* A mask naming a pocket that does not exist is malformed, not ignored. */
    partial.idempotency = {2, 3};
    partial.slot_mask = 1U << 15;
    CHECK(economy.apply(partial).code == acnet::ResultCode::Malformed);

    /* Above the cap the overflow comes back as bags, starting in the slot the
     * sold item vacated. */
    acnet::InventoryState rich;
    rich.bells = 99900;
    rich.slots[0].item = kRod;
    CHECK(world.register_inventory(3, rich));
    CHECK(economy.register_account(3));
    acnet::EconomyRequest big;
    big.type = acnet::EconomyOpType::Sell;
    big.account = 3;
    big.idempotency = {2, 4};
    big.inventory_slot = 0;
    big.expected_inventory_revision = 1;
    CHECK(economy.apply(big).code == acnet::ResultCode::Ok);
    /* 99900 + 125 is over the cap, so 30000 peels off into a bag. */
    CHECK(world.inventory(3)->bells == 70025);
    CHECK(world.inventory(3)->slots[0].item == rule.bag_item);

    /* With the rule unconfigured the authority keeps no game constants of its
     * own and the wallet simply grows -- the default for a bare test. */
    acnet::InventoryState uncapped;
    uncapped.bells = 99900;
    uncapped.slots[0].item = kRod;
    acnet::WorldAuthority plain_world(&players);
    CHECK(plain_world.register_inventory(4, uncapped));
    acnet::EconomyAuthority plain(&plain_world);
    CHECK(plain.register_account(4));
    plain.set_sell_price_resolver([](std::uint16_t item) { return acnet::shop_sell_price(item); });
    acnet::EconomyRequest plain_sell;
    plain_sell.type = acnet::EconomyOpType::Sell;
    plain_sell.account = 4;
    plain_sell.idempotency = {2, 5};
    plain_sell.inventory_slot = 0;
    plain_sell.expected_inventory_revision = 1;
    CHECK(plain.apply(plain_sell).code == acnet::ResultCode::Ok);
    CHECK(plain_world.inventory(4)->bells == 100025); // past the cap, no bag
    CHECK(plain_world.inventory(4)->slots[0].item == 0);
}

/* One town, one collection: a donation has to reach everyone, and a second
 * player must not be able to donate a species already on display. */
void museum_collection_replicates_and_refuses_duplicates() {
    constexpr std::uint16_t kFish = 6000;
    constexpr std::uint16_t kBug = 6001;

    acnet::MuseumState museum;
    museum.revision = 4;
    museum.donated_items = {kFish, kBug};

    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode_museum_delta(museum, payload));
    acnet::MuseumState decoded;
    CHECK(acnet::decode_museum_delta(payload, decoded));
    CHECK(decoded.revision == 4);
    CHECK(decoded.donated_items.size() == 2);
    CHECK(decoded.donated_items.count(kFish) == 1);
    CHECK(decoded.donated_items.count(kBug) == 1);

    /* An empty museum is a real state, not a malformed one. */
    acnet::MuseumState empty;
    empty.revision = 1;
    std::vector<std::uint8_t> empty_payload;
    CHECK(acnet::encode_museum_delta(empty, empty_payload));
    CHECK(acnet::decode_museum_delta(empty_payload, decoded));
    CHECK(decoded.donated_items.empty());

    /* Revision zero, trailing bytes, and truncation are all refused. */
    acnet::MuseumState bad;
    bad.revision = 0;
    std::vector<std::uint8_t> rejected;
    CHECK(!acnet::encode_museum_delta(bad, rejected));
    payload.push_back(0);
    CHECK(!acnet::decode_museum_delta(payload, decoded));
    payload.pop_back();
    payload.pop_back();
    CHECK(!acnet::decode_museum_delta(payload, decoded));

    /* Town-wide: it reaches a player who is not in the museum. */
    acnet::DeltaLog log;
    acnet::ReplicationDelta delta;
    delta.kind = acnet::ResourceKind::Museum;
    delta.zone = 0;
    delta.target_account = 0;
    CHECK(acnet::encode_museum_delta(museum, delta.payload));
    log.append(delta);
    acnet::InterestContext outside;
    outside.account = 12;
    outside.zone = 1;
    const acnet::DeltaQueryResult visible = log.since(0, outside, 8);
    CHECK(visible.deltas.size() == 1);
    CHECK(visible.deltas[0].kind == acnet::ResourceKind::Museum);

    /* Two players, one species. The second donation is refused and the item
     * stays in the donor's pocket. */
    acnet::PlayerDirectory players;
    acnet::WorldAuthority world(&players);
    acnet::InventoryState first;
    first.slots[0].item = kFish;
    acnet::InventoryState second;
    second.slots[0].item = kFish;
    CHECK(world.register_inventory(1, first));
    CHECK(world.register_inventory(2, second));
    acnet::EconomyAuthority economy(&world);
    CHECK(economy.register_account(1));
    CHECK(economy.register_account(2));

    acnet::EconomyRequest donate;
    donate.type = acnet::EconomyOpType::Donate;
    donate.account = 1;
    donate.idempotency = {3, 1};
    donate.inventory_slot = 0;
    donate.expected_inventory_revision = 1;
    donate.expected_aux_revision = economy.museum().revision;
    const acnet::EconomyResult accepted = economy.apply(donate);
    CHECK(accepted.code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->slots[0].item == 0);
    CHECK(economy.museum().donated_items.count(kFish) == 1);

    acnet::EconomyRequest duplicate;
    duplicate.type = acnet::EconomyOpType::Donate;
    duplicate.account = 2;
    duplicate.idempotency = {3, 2};
    duplicate.inventory_slot = 0;
    duplicate.expected_inventory_revision = 1;
    duplicate.expected_aux_revision = economy.museum().revision;
    CHECK(economy.apply(duplicate).code == acnet::ResultCode::InvalidState);
    CHECK(world.inventory(2)->slots[0].item == kFish);

    /* A donor quoting the collection as it was before the first donation is
     * told to refresh rather than silently overwriting it. */
    acnet::EconomyRequest stale;
    stale.type = acnet::EconomyOpType::Donate;
    stale.account = 2;
    stale.idempotency = {3, 3};
    stale.inventory_slot = 0;
    stale.expected_inventory_revision = 1;
    stale.expected_aux_revision = accepted.auxiliary_revision - 1;
    CHECK(economy.apply(stale).code == acnet::ResultCode::StaleRevision);
}

/* ResourceKind::Npc was declared long before anything produced one, so an NPC
 * only ever moved on a fresh baseline. */
void npc_state_replicates_between_baselines() {
    acnet::NpcState npc;
    npc.entity = 1000;
    npc.zone = 2;
    npc.revision = 5;
    npc.schedule_state = 3;
    npc.animation = 1;
    npc.emotion = 2;
    npc.destination = 77;
    npc.transform.position = {30.0F, 0.0F, 45.0F};
    npc.transform.yaw = 4096;

    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode_npc_delta(npc, payload));
    acnet::NpcState decoded;
    CHECK(acnet::decode_npc_delta(payload, decoded));
    CHECK(decoded.entity == npc.entity);
    CHECK(decoded.zone == npc.zone);
    CHECK(decoded.revision == npc.revision);
    CHECK(decoded.schedule_state == npc.schedule_state);
    CHECK(decoded.animation == npc.animation);
    CHECK(decoded.emotion == npc.emotion);
    CHECK(decoded.destination == npc.destination);
    CHECK(decoded.transform.position.z == npc.transform.position.z);
    CHECK(decoded.transform.yaw == npc.transform.yaw);

    /* Entity zero, zone zero, revision zero, and a non-finite position are all
     * refused, as is a trailing byte. */
    acnet::NpcState bad = npc;
    std::vector<std::uint8_t> rejected;
    bad.entity = 0;
    CHECK(!acnet::encode_npc_delta(bad, rejected));
    bad = npc;
    bad.zone = 0;
    CHECK(!acnet::encode_npc_delta(bad, rejected));
    bad = npc;
    bad.revision = 0;
    CHECK(!acnet::encode_npc_delta(bad, rejected));
    bad = npc;
    bad.transform.position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!acnet::encode_npc_delta(bad, rejected));
    payload.push_back(0);
    CHECK(!acnet::decode_npc_delta(payload, decoded));

    /* Zone-scoped, unlike the shelf: a viewer in another zone is not told. */
    acnet::DeltaLog log;
    acnet::ReplicationDelta delta;
    delta.kind = acnet::ResourceKind::Npc;
    delta.zone = npc.zone;
    delta.has_position = true;
    delta.position = npc.transform.position;
    CHECK(acnet::encode_npc_delta(npc, delta.payload));
    log.append(delta);

    acnet::InterestContext in_shop;
    in_shop.account = 5;
    in_shop.zone = 2;
    CHECK(log.since(0, in_shop, 8).deltas.size() == 1);

    acnet::InterestContext outdoors;
    outdoors.account = 6;
    outdoors.zone = 1;
    CHECK(log.since(0, outdoors, 8).deltas.empty());
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

void mail_and_banking_are_server_authoritative() {
    acnet::PlayerDirectory players;
    acnet::WorldAuthority world(&players);
    acnet::InventoryState sender_inventory;
    sender_inventory.bells = 900;
    sender_inventory.slots[0].item = 0x1000;
    acnet::InventoryState recipient_inventory;
    CHECK(world.register_inventory(1, sender_inventory));
    CHECK(world.register_inventory(2, recipient_inventory));

    acnet::EconomyAuthority economy(&world);
    acnet::AccountLedger sender_ledger;
    sender_ledger.bank_balance = 100;
    sender_ledger.debt = 400;
    CHECK(economy.register_account(1, sender_ledger));
    CHECK(economy.register_account(2));
    const std::uint64_t initial_items = economy.total_item_units();
    const std::uint64_t initial_bells = economy.total_bells();

    /* Banking is a revisioned, idempotent server transaction: the ledger only
     * moves through apply(), a stale revision is refused, and a replayed key
     * returns the first answer instead of moving the money twice. */
    acnet::EconomyRequest deposit;
    deposit.type = acnet::EconomyOpType::Deposit;
    deposit.account = 1;
    deposit.idempotency = {71, 1};
    deposit.expected_inventory_revision = 1;
    deposit.expected_aux_revision = 1;
    deposit.amount = 500;
    const acnet::EconomyResult deposited = economy.apply(deposit);
    CHECK(deposited.code == acnet::ResultCode::Ok);
    CHECK(deposited.type == acnet::EconomyOpType::Deposit);
    CHECK(deposited.balance == 600);
    CHECK(deposited.bells == 400);
    CHECK(deposited.auxiliary_revision == 2);
    const acnet::EconomyResult deposit_replay = economy.apply(deposit);
    CHECK(deposit_replay.replayed);
    CHECK(deposit_replay.balance == 600);
    CHECK(economy.ledger(1)->bank_balance == 600);

    acnet::EconomyRequest stale_withdraw;
    stale_withdraw.type = acnet::EconomyOpType::Withdraw;
    stale_withdraw.account = 1;
    stale_withdraw.idempotency = {71, 2};
    stale_withdraw.expected_inventory_revision = deposited.inventory_revision;
    stale_withdraw.expected_aux_revision = 1; // pre-deposit ledger revision
    stale_withdraw.amount = 600;
    CHECK(economy.apply(stale_withdraw).code == acnet::ResultCode::StaleRevision);

    acnet::EconomyRequest overdraw = stale_withdraw;
    overdraw.idempotency = {71, 3};
    overdraw.expected_aux_revision = deposited.auxiliary_revision;
    overdraw.amount = 601;
    CHECK(economy.apply(overdraw).code == acnet::ResultCode::InvalidState);

    acnet::EconomyRequest overpay;
    overpay.type = acnet::EconomyOpType::PayDebt;
    overpay.account = 1;
    overpay.idempotency = {71, 4};
    overpay.expected_inventory_revision = deposited.inventory_revision;
    overpay.expected_aux_revision = deposited.auxiliary_revision;
    overpay.amount = 401; // more than the 400 owed
    CHECK(economy.apply(overpay).code == acnet::ResultCode::InvalidState);
    CHECK(economy.ledger(1)->debt == 400);
    CHECK(economy.total_bells() == initial_bells);

    /* Mail: the attachment leaves the sender's pocket, lands in the recipient's
     * mailbox, and is conserved the whole way. */
    acnet::EconomyRequest attach;
    attach.type = acnet::EconomyOpType::AttachMail;
    attach.account = 1;
    attach.idempotency = {72, 1};
    attach.expected_inventory_revision = deposited.inventory_revision;
    attach.inventory_slot = 0;
    attach.expected_item = 0x1000;
    attach.recipient = 2;
    const acnet::EconomyResult attached = economy.apply(attach);
    CHECK(attached.code == acnet::ResultCode::Ok);
    CHECK(attached.mail_id != 0);
    CHECK(world.inventory(1)->slots[0].item == 0);
    CHECK(economy.mailbox(2)->mail.size() == 1);
    CHECK(economy.mailbox(2)->mail[0] == attached.mail_id);
    CHECK(economy.mail_for(2).size() == 1);
    CHECK(economy.total_item_units() == initial_items);

    acnet::EconomyRequest self_mail;
    self_mail.type = acnet::EconomyOpType::AttachMail;
    self_mail.account = 2;
    self_mail.idempotency = {72, 2};
    self_mail.expected_inventory_revision = world.inventory(2)->revision;
    self_mail.inventory_slot = 0;
    self_mail.recipient = 2;
    CHECK(economy.apply(self_mail).code != acnet::ResultCode::Ok);

    /* A letter gives up its present only once it is carried, exactly as in the
     * original game: claiming straight out of the mailbox is refused. */
    acnet::EconomyRequest premature_claim;
    premature_claim.type = acnet::EconomyOpType::ClaimMail;
    premature_claim.account = 2;
    premature_claim.idempotency = {73, 0};
    premature_claim.expected_inventory_revision = world.inventory(2)->revision;
    premature_claim.expected_aux_revision = economy.mailbox(2)->revision;
    premature_claim.mail_id = attached.mail_id;
    CHECK(economy.apply(premature_claim).code == acnet::ResultCode::NotFound);

    /* Only the addressee may take, only against the current mail revision. */
    acnet::EconomyRequest wrong_taker;
    wrong_taker.type = acnet::EconomyOpType::TakeMail;
    wrong_taker.account = 1;
    wrong_taker.idempotency = {73, 1};
    wrong_taker.expected_inventory_revision = world.inventory(1)->revision;
    wrong_taker.expected_aux_revision = economy.mailbox(1)->revision;
    wrong_taker.mail_id = attached.mail_id;
    CHECK(economy.apply(wrong_taker).code == acnet::ResultCode::NotFound);

    acnet::EconomyRequest stale_take;
    stale_take.type = acnet::EconomyOpType::TakeMail;
    stale_take.account = 2;
    stale_take.idempotency = {73, 2};
    stale_take.expected_inventory_revision = world.inventory(2)->revision;
    stale_take.expected_aux_revision = 1; // pre-delivery mail revision
    stale_take.mail_id = attached.mail_id;
    const acnet::EconomyResult stale_taken = economy.apply(stale_take);
    CHECK(stale_taken.code == acnet::ResultCode::StaleRevision);
    CHECK(stale_taken.auxiliary_revision == economy.mailbox(2)->revision);

    acnet::EconomyRequest take = stale_take;
    take.idempotency = {73, 3};
    take.expected_aux_revision = economy.mailbox(2)->revision;
    const acnet::EconomyResult taken = economy.apply(take);
    CHECK(taken.code == acnet::ResultCode::Ok);
    CHECK(taken.type == acnet::EconomyOpType::TakeMail);
    CHECK(economy.mailbox(2)->mail.empty());
    CHECK(economy.mailbox(2)->carried.size() == 1);
    CHECK(economy.mail(attached.mail_id)->location == acnet::MailLocation::Carried);
    CHECK(taken.auxiliary_revision == economy.mailbox(2)->revision);
    /* Taking a letter moves no item: the present is still inside it. */
    CHECK(economy.total_item_units() == initial_items);
    CHECK(economy.apply(take).replayed);

    acnet::EconomyRequest claim;
    claim.type = acnet::EconomyOpType::ClaimMail;
    claim.account = 2;
    claim.idempotency = {73, 4};
    claim.expected_inventory_revision = world.inventory(2)->revision;
    claim.expected_aux_revision = economy.mailbox(2)->revision;
    claim.mail_id = attached.mail_id;
    const acnet::EconomyResult claimed = economy.apply(claim);
    CHECK(claimed.code == acnet::ResultCode::Ok);
    CHECK(claimed.type == acnet::EconomyOpType::ClaimMail);
    CHECK(claimed.item == 0x1000);
    CHECK(world.inventory(2)->slots[claimed.inventory_slot].item == 0x1000);
    /* The letter itself survives with an empty present, still carried. */
    CHECK(economy.mail(attached.mail_id) != nullptr);
    CHECK(economy.mail(attached.mail_id)->attachment == 0);
    CHECK(economy.mailbox(2)->carried.size() == 1);
    CHECK(claimed.auxiliary_revision == economy.mailbox(2)->revision);
    CHECK(economy.total_item_units() == initial_items);
    const acnet::EconomyResult claim_replay = economy.apply(claim);
    CHECK(claim_replay.replayed);
    CHECK(economy.total_item_units() == initial_items);

    /* A second claim on an emptied letter is refused rather than duplicating. */
    acnet::EconomyRequest reclaim = claim;
    reclaim.idempotency = {73, 5};
    reclaim.expected_inventory_revision = world.inventory(2)->revision;
    reclaim.expected_aux_revision = economy.mailbox(2)->revision;
    CHECK(economy.apply(reclaim).code == acnet::ResultCode::InvalidState);

    /* Discarding frees the pocket slot; a letter still holding a present may
     * not be discarded, because that would destroy the item. */
    acnet::EconomyRequest discard;
    discard.type = acnet::EconomyOpType::DiscardMail;
    discard.account = 2;
    discard.idempotency = {73, 6};
    discard.expected_inventory_revision = world.inventory(2)->revision;
    discard.expected_aux_revision = economy.mailbox(2)->revision;
    discard.mail_id = attached.mail_id;
    const acnet::EconomyResult discarded = economy.apply(discard);
    CHECK(discarded.code == acnet::ResultCode::Ok);
    CHECK(economy.mail(attached.mail_id) == nullptr);
    CHECK(economy.mailbox(2)->carried.empty());
    CHECK(economy.total_item_units() == initial_items);

    /* Operator gifts commit through the same authority. Bells appear in the
     * bank with a fresh ledger revision; a letter appears in the mailbox with
     * the operator as sender. */
    CHECK(economy.admin_grant_bank_bells(999, 100).code == acnet::ResultCode::NotFound);
    const acnet::Revision ledger_revision = economy.ledger(2)->revision;
    const acnet::EconomyResult granted = economy.admin_grant_bank_bells(2, 12345);
    CHECK(granted.code == acnet::ResultCode::Ok);
    CHECK(granted.balance == 12345);
    CHECK(economy.ledger(2)->bank_balance == 12345);
    CHECK(granted.auxiliary_revision != ledger_revision);
    CHECK(economy.total_bells() == initial_bells + 12345);

    acnet::MailContent gift;
    const std::string message = "Thanks for playing!";
    std::copy(message.begin(), message.end(), gift.body.begin());
    const acnet::EconomyResult posted = economy.admin_send_mail(2, 0x2203, gift);
    CHECK(posted.code == acnet::ResultCode::Ok);
    const acnet::MailRecord* letter = economy.mail(posted.mail_id);
    CHECK(letter != nullptr);
    CHECK(letter->sender == acnet::kAdministratorAccount);
    CHECK(letter->recipient == 2);
    CHECK(letter->attachment == 0x2203);
    CHECK(std::equal(message.begin(), message.end(), letter->content.body.begin()));
    CHECK(economy.mailbox(2)->mail.size() == 1);
    CHECK(economy.admin_send_mail(999, 0, gift).code == acnet::ResultCode::NotFound);

    /* The mailbox is bounded, and a full one refuses both a player letter and
     * an operator letter rather than silently dropping either. */
    for (std::size_t i = economy.mailbox(2)->mail.size(); i < acnet::kMailboxCapacity; ++i) {
        CHECK(economy.admin_send_mail(2, 0, gift).code == acnet::ResultCode::Ok);
    }
    CHECK(economy.mailbox(2)->mail.size() == acnet::kMailboxCapacity);
    CHECK(economy.admin_send_mail(2, 0, gift).code == acnet::ResultCode::Capacity);
    acnet::InventoryState refilled = *world.inventory(1);
    refilled.slots[0].item = 0x1001;
    refilled.revision = 50;
    CHECK(world.set_inventory(1, refilled));
    acnet::EconomyRequest overflow;
    overflow.type = acnet::EconomyOpType::AttachMail;
    overflow.account = 1;
    overflow.idempotency = {74, 1};
    overflow.expected_inventory_revision = 50;
    overflow.inventory_slot = 0;
    overflow.recipient = 2;
    CHECK(economy.apply(overflow).code == acnet::ResultCode::Capacity);
    CHECK(world.inventory(1)->slots[0].item == 0x1001); // the item never left the pocket

    /* A full pocket of letters must not be a dead end: taking is refused, and
     * discarding one is what makes room again. This is the whole reason
     * DiscardMail exists. The mailbox is full at this point, so emptying it
     * into the pocket fills the pocket exactly. */
    {
        std::size_t taken_count = 0;
        while (!economy.mailbox(2)->mail.empty()) {
            acnet::EconomyRequest fill;
            fill.type = acnet::EconomyOpType::TakeMail;
            fill.account = 2;
            fill.idempotency = {76, static_cast<std::uint64_t>(taken_count + 1)};
            fill.expected_inventory_revision = world.inventory(2)->revision;
            fill.expected_aux_revision = economy.mailbox(2)->revision;
            fill.mail_id = economy.mailbox(2)->mail.front();
            CHECK(economy.apply(fill).code == acnet::ResultCode::Ok);
            ++taken_count;
        }
        CHECK(taken_count == acnet::kMailboxCapacity);
        CHECK(economy.mailbox(2)->carried.size() == acnet::kCarriedMailCapacity);

        CHECK(economy.admin_send_mail(2, 0, gift).code == acnet::ResultCode::Ok);
        const std::uint64_t blocked = economy.mailbox(2)->mail.back();
        acnet::EconomyRequest overflow_take;
        overflow_take.type = acnet::EconomyOpType::TakeMail;
        overflow_take.account = 2;
        overflow_take.idempotency = {77, 1};
        overflow_take.expected_inventory_revision = world.inventory(2)->revision;
        overflow_take.expected_aux_revision = economy.mailbox(2)->revision;
        overflow_take.mail_id = blocked;
        CHECK(economy.apply(overflow_take).code == acnet::ResultCode::Capacity);

        /* The oldest carried letter is the one still holding a present, and
         * throwing that away would destroy the item, so it is refused. */
        acnet::EconomyRequest discard_present;
        discard_present.type = acnet::EconomyOpType::DiscardMail;
        discard_present.account = 2;
        discard_present.idempotency = {77, 4};
        discard_present.expected_inventory_revision = world.inventory(2)->revision;
        discard_present.expected_aux_revision = economy.mailbox(2)->revision;
        discard_present.mail_id = economy.mailbox(2)->carried.front();
        CHECK(economy.mail(discard_present.mail_id)->attachment != 0);
        CHECK(economy.apply(discard_present).code == acnet::ResultCode::InvalidState);

        acnet::EconomyRequest free_slot;
        free_slot.type = acnet::EconomyOpType::DiscardMail;
        free_slot.account = 2;
        free_slot.idempotency = {77, 2};
        free_slot.expected_inventory_revision = world.inventory(2)->revision;
        free_slot.expected_aux_revision = economy.mailbox(2)->revision;
        free_slot.mail_id = economy.mailbox(2)->carried.back();
        CHECK(economy.mail(free_slot.mail_id)->attachment == 0);
        CHECK(economy.apply(free_slot).code == acnet::ResultCode::Ok);
        CHECK(economy.mailbox(2)->carried.size() == acnet::kCarriedMailCapacity - 1);

        acnet::EconomyRequest retry = overflow_take;
        retry.idempotency = {77, 3};
        retry.expected_aux_revision = economy.mailbox(2)->revision;
        CHECK(economy.apply(retry).code == acnet::ResultCode::Ok);
        CHECK(economy.mailbox(2)->carried.size() == acnet::kCarriedMailCapacity);
    }

    /* Administrative operations are not reachable through the request path even
     * when a request is built locally rather than decoded from the wire. */
    acnet::EconomyRequest forged;
    forged.type = acnet::EconomyOpType::AdminGrantBells;
    forged.account = 1;
    forged.idempotency = {75, 1};
    forged.amount = 1000000;
    CHECK(economy.apply(forged).code == acnet::ResultCode::Unauthorized);
    CHECK(economy.ledger(1)->bank_balance == 600);
    std::vector<std::uint8_t> forged_bytes;
    CHECK(!acnet::encode(forged, forged_bytes));
}

void operator_gifts_survive_a_restart() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-admin-gift-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    constexpr std::int64_t wall = 1700000000;
    acserver::TownRuntimeConfig config;
    config.port = 0;
    config.data_directory = root / "town";
    config.connection_timeout_ms = 60000;
    config.invite_key = "gift-test-key";
    std::string error;
    constexpr acnet::AccountId account = 4242;

    {
        acserver::TownRuntime server(config);
        CHECK(server.initialize(wall, error));
        /* Gifts address accounts, and an account exists once its player has
         * connected at least once. */
        CHECK(!server.grant_bank_bells(account, 1000, error));
        CHECK(!error.empty());
        acnet::ClientConfig client_config;
        client_config.server_port = server.bound_port();
        client_config.account = account;
        client_config.invite_key = config.invite_key;
        acnet::ClientRuntime client(client_config);
        CHECK(client.start(1000, error));
        for (std::uint64_t i = 0; i < 200 && client.baseline() == nullptr; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(1000 + i, wall, error));
            CHECK(client.poll(1000 + i, error));
        }
        CHECK(client.baseline() != nullptr);
        CHECK(client.mailbox().revision != 0);
        CHECK(client.mail().empty());

        const std::uint64_t balance_before = client.baseline()->ledger.bank_balance;
        CHECK(server.grant_bank_bells(account, 30000, error));
        CHECK(server.send_mail(account, 0x2203, "Welcome to town", error));
        const std::vector<acserver::RuntimeAccountSummary> summaries = server.account_summaries();
        CHECK(summaries.size() == 1);
        CHECK(summaries[0].account == account);
        CHECK(summaries[0].bank_balance == balance_before + 30000);
        CHECK(summaries[0].pending_mail == 1);

        /* The addressee is told about the letter while connected, without
         * waiting for another baseline. */
        for (std::uint64_t i = 0; i < 200 && client.mail().empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(1400 + i, wall, error));
            CHECK(client.poll(1400 + i, error));
        }
        CHECK(client.mail().size() == 1);
        CHECK(client.mail()[0].sender == acnet::kAdministratorAccount);
        CHECK(client.mail()[0].attachment == 0x2203);
        CHECK(client.mailbox().mail.size() == 1);

        /* The original two steps, both server transactions: take the letter out
         * of the mailbox, then take its present out of the carried letter. */
        const acnet::Revision ledger_revision_before = client.baseline()->ledger.revision;
        acnet::EconomyRequest take;
        take.type = acnet::EconomyOpType::TakeMail;
        take.account = account;
        take.idempotency = {91, 6};
        take.expected_inventory_revision = client.baseline()->inventory.revision;
        take.expected_aux_revision = client.mailbox().revision;
        take.mail_id = client.mail()[0].id;
        const std::uint64_t gift_mail_id = take.mail_id;
        CHECK(client.request(take, 1600, error));
        std::optional<acnet::EconomyResult> taken;
        for (std::uint64_t i = 0; i < 200 && !taken.has_value(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(1600 + i, wall, error));
            CHECK(client.poll(1600 + i, error));
            taken = client.take_economy_result();
        }
        CHECK(taken.has_value());
        CHECK(taken->code == acnet::ResultCode::Ok);
        for (std::uint64_t i = 0; i < 200 && client.mailbox().carried.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(1650 + i, wall, error));
            CHECK(client.poll(1650 + i, error));
        }
        CHECK(client.mailbox().mail.empty());
        CHECK(client.mailbox().carried.size() == 1);

        acnet::EconomyRequest claim;
        claim.type = acnet::EconomyOpType::ClaimMail;
        claim.account = account;
        claim.idempotency = {91, 7};
        claim.expected_inventory_revision = client.baseline()->inventory.revision;
        claim.expected_aux_revision = client.mailbox().revision;
        claim.mail_id = gift_mail_id;
        CHECK(client.request(claim, 1700, error));
        std::optional<acnet::EconomyResult> claimed;
        for (std::uint64_t i = 0; i < 200 && !claimed.has_value(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(1700 + i, wall, error));
            CHECK(client.poll(1700 + i, error));
            claimed = client.take_economy_result();
        }
        CHECK(claimed.has_value());
        CHECK(claimed->code == acnet::ResultCode::Ok);
        CHECK(claimed->item == 0x2203);
        /* A result's auxiliary_revision belongs to the authority its operation
         * touched. A mail claim must land on the mailbox mirror and leave the
         * bank mirror alone, or the next deposit would quote a mailbox revision
         * and be rejected as stale. */
        CHECK(claimed->type == acnet::EconomyOpType::ClaimMail);
        CHECK(client.mailbox().revision == claimed->auxiliary_revision);
        CHECK(client.baseline()->ledger.revision == ledger_revision_before);
        CHECK(client.baseline()->ledger.bank_balance == balance_before + 30000);
        /* The letter stays in the pocket with its present gone, as in the
         * original -- the player still has something to read. */
        for (std::uint64_t i = 0; i < 200 && client.mail()[0].attachment != 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(server.step(1900 + i, wall, error));
            CHECK(client.poll(1900 + i, error));
        }
        CHECK(client.mail().size() == 1);
        CHECK(client.mail()[0].attachment == 0);
        CHECK(client.mailbox().carried.size() == 1);
        CHECK(server.shutdown(error));
    }

    /* A gift is journalled before it is reported, so it is still there after a
     * restart -- including a second letter posted while the town was stopped. */
    acserver::TownRuntime restarted(config);
    CHECK(restarted.initialize(wall + 5, error));
    std::vector<acserver::RuntimeAccountSummary> summaries = restarted.account_summaries();
    CHECK(summaries.size() == 1);
    CHECK(summaries[0].bank_balance >= 30000);
    CHECK(summaries[0].pending_mail == 0);
    CHECK(summaries[0].carried_mail == 1); /* the read letter survived the restart */
    CHECK(restarted.send_mail(account, 0x2204, "A second gift", error));
    summaries = restarted.account_summaries();
    CHECK(summaries[0].pending_mail == 1);
    CHECK(restarted.shutdown(error));

    acserver::TownRuntime reopened(config);
    CHECK(reopened.initialize(wall + 10, error));
    const std::vector<acserver::RuntimeAccountSummary> reopened_summaries = reopened.account_summaries();
    CHECK(reopened_summaries.size() == 1);
    CHECK(reopened_summaries[0].bank_balance >= 30000);
    CHECK(reopened_summaries[0].pending_mail == 1);
    CHECK(reopened.shutdown(error));

    /* Startup decodes the checkpoint and then the newest journalled state on
     * the same authority. A letter that appears in both must not be read as a
     * duplicate, and one letter has to survive alongside a second posted after
     * the checkpoint even when the process never shuts down cleanly. */
    {
        acserver::TownRuntime crashing(config);
        CHECK(crashing.initialize(wall + 15, error));
        CHECK(crashing.account_summaries()[0].pending_mail == 1);
        CHECK(crashing.send_mail(account, 0x2205, "posted after the checkpoint", error));
        CHECK(crashing.account_summaries()[0].pending_mail == 2);
        // No shutdown: the newest journal record is never superseded by a checkpoint.
    }
    acserver::TownRuntime replayed(config);
    CHECK(replayed.initialize(wall + 20, error));
    CHECK(replayed.account_summaries()[0].pending_mail == 2);
    CHECK(replayed.shutdown(error));
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

void gyroids_replicate_and_transact() {
    /* Wire round trips first: the operation, the result, and the delta. */
    acnet::GyroidOperation update;
    update.type = acnet::GyroidOpType::Update;
    update.account = 1;
    update.idempotency = {70, 1};
    update.house_id = 10000;
    update.expected_gyroid_revision = 1;
    update.expected_inventory_revision = 1;
    update.items[0] = {0x1100, acnet::kGyroidExchangeSale, 400};
    update.items[2] = {0x2200, acnet::kGyroidExchangeFree, 0};
    update.message[0] = 'Y';
    update.message[127] = 'o';
    std::vector<std::uint8_t> bytes;
    CHECK(acnet::encode(update, bytes));
    acnet::GyroidOperation decoded_op;
    CHECK(acnet::decode(bytes, decoded_op));
    CHECK(decoded_op.type == acnet::GyroidOpType::Update);
    CHECK(decoded_op.items[0].price == 400);
    CHECK(decoded_op.items[2].item == 0x2200);
    CHECK(decoded_op.message[127] == 'o');
    bytes.pop_back();
    CHECK(!acnet::decode(bytes, decoded_op));
    /* A sale without a price, a price on a free item, and an empty slot with
     * terms are all nonsense the codec refuses in both directions. */
    acnet::GyroidOperation invalid = update;
    invalid.items[0] = {0x1100, acnet::kGyroidExchangeSale, 0};
    CHECK(!acnet::encode(invalid, bytes));
    invalid.items[0] = {0x1100, acnet::kGyroidExchangeFree, 5};
    CHECK(!acnet::encode(invalid, bytes));
    invalid.items[0] = {0, acnet::kGyroidExchangeSale, 0};
    CHECK(!acnet::encode(invalid, bytes));
    invalid = update;
    invalid.item_slot = acnet::kGyroidItemSlots;
    CHECK(!acnet::encode(invalid, bytes));

    acnet::GyroidResult wire_result;
    wire_result.code = acnet::ResultCode::Ok;
    wire_result.idempotency = {70, 1};
    wire_result.house_id = 10000;
    wire_result.gyroid_revision = 2;
    wire_result.inventory_revision = 3;
    wire_result.item = 0x1100;
    wire_result.price = 400;
    wire_result.inventory_slot = 5;
    CHECK(acnet::encode(wire_result, bytes));
    acnet::GyroidResult decoded_result;
    CHECK(acnet::decode(bytes, decoded_result));
    CHECK(decoded_result.price == 400 && decoded_result.inventory_slot == 5);

    acnet::GyroidDelta delta;
    delta.house_id = 10000;
    delta.original_slot = 2;
    delta.state.revision = 4;
    delta.state.items[3] = {0x3300, acnet::kGyroidExchangeDisplay, 0};
    delta.state.bells = 77;
    CHECK(acnet::encode_gyroid_delta(delta, bytes));
    acnet::GyroidDelta decoded_delta;
    CHECK(acnet::decode_gyroid_delta(bytes, decoded_delta));
    CHECK(decoded_delta.original_slot == 2 && decoded_delta.state.items[3].item == 0x3300 &&
          decoded_delta.state.bells == 77);
    delta.original_slot = acnet::kOriginalResidentSlots;
    CHECK(!acnet::encode_gyroid_delta(delta, bytes));

    /* Authority. Account 1 owns the house; account 2 is the browsing guest. */
    acnet::WorldAuthority world(nullptr);
    acnet::InventoryState owner_inventory;
    owner_inventory.slots[0].item = 0x1100;
    owner_inventory.slots[1].item = 0x2200;
    CHECK(world.register_inventory(1, owner_inventory));
    acnet::InventoryState guest_inventory;
    guest_inventory.bells = 1000;
    CHECK(world.register_inventory(2, guest_inventory));
    acnet::HousingAuthority housing(&world);
    housing.set_wallet_policy(99999, 30000, 0x1EC0);
    CHECK(housing.register_resident(0, 1, 100));

    /* The owner puts both pocket items on display; they leave the pockets. */
    update.house_id = housing.house_for(1)->house_id;
    auto result = housing.apply_gyroid(update);
    CHECK(result.code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->slots[0].item == 0);
    CHECK(world.inventory(1)->slots[1].item == 0);
    CHECK(housing.house_for(1)->gyroid.items[0].item == 0x1100);
    CHECK(housing.house_for(1)->gyroid.message[0] == 'Y');
    CHECK(housing.apply_gyroid(update).replayed);

    /* The guest may not buy with a stale revision, an empty slot, or thin air
     * for a wallet; the owner may not "take" their own display at all. */
    acnet::GyroidOperation take;
    take.type = acnet::GyroidOpType::Take;
    take.account = 2;
    take.idempotency = {70, 2};
    take.house_id = update.house_id;
    take.expected_gyroid_revision = 1;
    take.expected_inventory_revision = world.inventory(2)->revision;
    take.item_slot = 0;
    take.expected_item = 0x1100;
    CHECK(housing.apply_gyroid(take).code == acnet::ResultCode::StaleRevision);
    take.idempotency = {70, 3};
    take.expected_gyroid_revision = result.gyroid_revision;
    take.item_slot = 1;
    take.expected_item = 0;
    CHECK(housing.apply_gyroid(take).code == acnet::ResultCode::InvalidState);
    take.idempotency = {70, 4};
    take.account = 1;
    take.item_slot = 0;
    take.expected_item = 0x1100;
    take.expected_inventory_revision = world.inventory(1)->revision;
    CHECK(housing.apply_gyroid(take).code == acnet::ResultCode::Unauthorized);

    /* A real purchase: the item lands in the guest's pocket, the price leaves
     * their wallet, and the gyroid holds the proceeds. */
    take.idempotency = {70, 5};
    take.account = 2;
    take.expected_inventory_revision = world.inventory(2)->revision;
    result = housing.apply_gyroid(take);
    CHECK(result.code == acnet::ResultCode::Ok);
    CHECK(result.item == 0x1100 && result.price == 400);
    CHECK(world.inventory(2)->slots[result.inventory_slot].item == 0x1100);
    CHECK(world.inventory(2)->bells == 600);
    CHECK(housing.house_for(1)->gyroid.items[0].item == 0);
    CHECK(housing.house_for(1)->gyroid.bells == 400);

    /* The free item costs nothing; 600 bells stay put. */
    acnet::GyroidOperation take_free = take;
    take_free.idempotency = {70, 6};
    take_free.expected_gyroid_revision = result.gyroid_revision;
    take_free.expected_inventory_revision = result.inventory_revision;
    take_free.item_slot = 2;
    take_free.expected_item = 0x2200;
    result = housing.apply_gyroid(take_free);
    CHECK(result.code == acnet::ResultCode::Ok && result.price == 0);
    CHECK(world.inventory(2)->bells == 600);

    /* Only the owner collects, and the wallet cap breaks into money bags
     * exactly as a sale at the counter does. */
    acnet::GyroidOperation collect;
    collect.type = acnet::GyroidOpType::Collect;
    collect.account = 2;
    collect.idempotency = {70, 7};
    collect.house_id = take.house_id;
    collect.expected_gyroid_revision = result.gyroid_revision;
    collect.expected_inventory_revision = result.inventory_revision;
    CHECK(housing.apply_gyroid(collect).code == acnet::ResultCode::Unauthorized);
    world.inventory(1);
    {
        acnet::InventoryState rich = *world.inventory(1);
        rich.bells = 99900;
        CHECK(world.set_inventory(1, rich));
    }
    collect.account = 1;
    collect.idempotency = {70, 8};
    collect.expected_inventory_revision = world.inventory(1)->revision;
    result = housing.apply_gyroid(collect);
    CHECK(result.code == acnet::ResultCode::Ok);
    CHECK(result.bells_collected == 400);
    /* 99900 + 400 = 100300, over the 99999 cap: one 30000 bag comes back. */
    CHECK(world.inventory(1)->bells == 70300);
    CHECK(world.inventory(1)->slots[0].item == 0x1EC0);
    CHECK(housing.house_for(1)->gyroid.bells == 0);
    collect.idempotency = {70, 9};
    collect.expected_gyroid_revision = result.gyroid_revision;
    collect.expected_inventory_revision = result.inventory_revision;
    CHECK(housing.apply_gyroid(collect).code == acnet::ResultCode::InvalidState);

    /* Display-only items refuse a take. */
    acnet::GyroidOperation display = update;
    display.idempotency = {70, 10};
    display.expected_gyroid_revision = housing.house_for(1)->gyroid.revision;
    display.expected_inventory_revision = world.inventory(1)->revision;
    display.items = {};
    display.items[0] = {0x3300, acnet::kGyroidExchangeDisplay, 0};
    /* 0x3300 is not in the owner's pockets, so the diff must refuse it... */
    CHECK(housing.apply_gyroid(display).code == acnet::ResultCode::InvalidState);
    {
        acnet::InventoryState stocked = *world.inventory(1);
        stocked.slots[10].item = 0x3300;
        CHECK(world.set_inventory(1, stocked));
    }
    display.idempotency = {70, 11};
    display.expected_inventory_revision = world.inventory(1)->revision;
    CHECK(housing.apply_gyroid(display).code == acnet::ResultCode::Ok);
    acnet::GyroidOperation take_display = take;
    take_display.idempotency = {70, 12};
    take_display.expected_gyroid_revision = housing.house_for(1)->gyroid.revision;
    take_display.expected_inventory_revision = world.inventory(2)->revision;
    take_display.item_slot = 0;
    take_display.expected_item = 0x3300;
    CHECK(housing.apply_gyroid(take_display).code == acnet::ResultCode::InvalidState);
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
    player.appearance.clothing = 0xFE20;
    player.appearance.clothing_index = 0x104;
    player.appearance.revision = 7;
    player.pattern.present = true;
    player.pattern.palette = 11;
    player.pattern.texture.fill(0x5A);
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
    baseline.residents.slots[0].account = 9001;
    baseline.residents.slots[0].name = {'K', 'I', 'K', 'I', ' ', ' ', ' ', ' '};
    baseline.residents.slots[0].gender = 1;
    baseline.residents.slots[0].occupied = true;
    baseline.residents.slots[2].account = 9002;
    baseline.residents.slots[2].name = {'R', 'E', 'X', ' ', ' ', ' ', ' ', ' '};
    baseline.residents.slots[2].gender = 0;
    baseline.residents.slots[2].occupied = true;
    baseline.gyroids[0].occupied = true;
    baseline.gyroids[0].house_id = 10000;
    baseline.gyroids[0].state.revision = 3;
    baseline.gyroids[0].state.items[1] = {0x1234, acnet::kGyroidExchangeSale, 500};
    baseline.gyroids[0].state.message[0] = 'H';
    baseline.gyroids[0].state.bells = 1200;
    std::vector<std::uint8_t> baseline_bytes;
    CHECK(acnet::encode_baseline(baseline, baseline_bytes));
    acnet::ZoneBaseline decoded;
    CHECK(acnet::decode_baseline(baseline_bytes, decoded));
    CHECK(decoded.zone == 1);
    CHECK(decoded.tiles.size() == 1);
    CHECK(decoded.tiles[0].second.item == 777);
    CHECK(decoded.players.size() == 1);
    CHECK(decoded.players[0].appearance.clothing_index == 0x104);
    CHECK(decoded.players[0].appearance.revision == 7);
    CHECK(decoded.players[0].pattern.present);
    CHECK(decoded.players[0].pattern.palette == 11);
    CHECK(decoded.players[0].pattern.texture == player.pattern.texture);
    CHECK(decoded.npcs.size() == 1);
    /* Town-wide occupancy travels with the baseline and is independent of the
     * interest set above (one visible player, three in town). */
    CHECK(decoded.town_population == 3);
    CHECK(decoded.town_capacity == 16);
    /* House ownership likewise: slot 2's resident is not in the interest set and
     * need not even be logged in. */
    CHECK(decoded.residents == baseline.residents);
    CHECK(decoded.residents.slots[2].account == 9002);
    CHECK(!decoded.residents.slots[1].occupied);
    CHECK(decoded.residents.slots[1].account == 0);
    /* The gyroids ride the baseline town-wide, like the roster. */
    CHECK(decoded.gyroids[0].occupied);
    CHECK(decoded.gyroids[0].house_id == 10000);
    CHECK(decoded.gyroids[0].state.revision == 3);
    CHECK(decoded.gyroids[0].state.items[1].item == 0x1234);
    CHECK(decoded.gyroids[0].state.items[1].exchange == acnet::kGyroidExchangeSale);
    CHECK(decoded.gyroids[0].state.items[1].price == 500);
    CHECK(decoded.gyroids[0].state.message[0] == 'H');
    CHECK(decoded.gyroids[0].state.bells == 1200);
    CHECK(!decoded.gyroids[1].occupied);
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

/* A tile delta has to say who changed the tile and how, or a viewer cannot tell
 * a player throwing an item on the ground from a sapling growing overnight --
 * and cannot find the hand the arc should start from. */
void tile_delta_carries_actor_and_cause() {
    acnet::TileStateDelta delta;
    delta.address = {1, 12, -7};
    delta.state.revision = 9;
    delta.state.item = 0x2801;
    delta.state.condition = 3;
    delta.actor = 4242;
    delta.cause = acnet::TileChangeCause::Drop;
    std::vector<std::uint8_t> bytes;
    CHECK(acnet::encode_tile_delta(delta, bytes));
    acnet::TileStateDelta decoded;
    CHECK(acnet::decode_tile_delta(bytes, decoded));
    CHECK(decoded.address == delta.address);
    CHECK(decoded.state.item == 0x2801);
    CHECK(decoded.state.condition == 3);
    CHECK(decoded.actor == 4242);
    CHECK(decoded.cause == acnet::TileChangeCause::Drop);

    /* A change with no acting player -- growth, a save import, an operator
     * command -- defaults to the silent path rather than animating as somebody
     * else's drop. */
    acnet::TileStateDelta grown;
    grown.address = {1, 0, 0};
    grown.state.revision = 2;
    grown.state.item = 0x0040;
    CHECK(grown.actor == 0);
    CHECK(grown.cause == acnet::TileChangeCause::Server);
    CHECK(acnet::encode_tile_delta(grown, bytes));
    CHECK(acnet::decode_tile_delta(bytes, decoded));
    CHECK(decoded.actor == 0);
    CHECK(decoded.cause == acnet::TileChangeCause::Server);

    /* Every WorldOpType maps onto a cause, so a new operation cannot silently
     * arrive on the wire as Server and be mistaken for one nobody performed. */
    CHECK(acnet::tile_change_cause(acnet::WorldOpType::DropItem) == acnet::TileChangeCause::Drop);
    CHECK(acnet::tile_change_cause(acnet::WorldOpType::PickupItem) == acnet::TileChangeCause::Pickup);
    CHECK(acnet::tile_change_cause(acnet::WorldOpType::FillHole) == acnet::TileChangeCause::FillHole);

    /* A cause past the last enumerator is refused at both ends, and truncation
     * at any point in the two new fields is refused too. */
    acnet::TileStateDelta bad_cause = delta;
    bad_cause.cause = static_cast<acnet::TileChangeCause>(
        static_cast<std::uint8_t>(acnet::TileChangeCause::FillHole) + 1);
    CHECK(!acnet::encode_tile_delta(bad_cause, bytes));
    CHECK(acnet::encode_tile_delta(delta, bytes));
    bytes.back() = static_cast<std::uint8_t>(acnet::TileChangeCause::FillHole) + 1;
    CHECK(!acnet::decode_tile_delta(bytes, decoded));
    for (std::size_t drop = 1; drop <= 9; ++drop) {
        std::vector<std::uint8_t> truncated;
        CHECK(acnet::encode_tile_delta(delta, truncated));
        truncated.resize(truncated.size() - drop);
        CHECK(!acnet::decode_tile_delta(truncated, decoded));
    }
    CHECK(acnet::encode_tile_delta(delta, bytes));
    bytes.push_back(0);
    CHECK(!acnet::decode_tile_delta(bytes, decoded));
}

/* The viewer reacts to tile changes one at a time so it can animate them, and
 * reprojects the whole chunk only when it has to. Both halves of that are
 * client state, so both are tested here rather than inferred from the game. */
void tile_changes_queue_separately_from_baselines() {
    acnet::ClientRuntime client;
    const auto apply = [&](acnet::MessageType type, const std::vector<std::uint8_t>& payload) {
        acnet::DecodedPacket packet;
        packet.header.message_type = type;
        packet.payload = payload;
        std::string error;
        const bool ok = client.dispatch(packet, 1000, error);
        if (!ok) throw std::runtime_error("dispatch rejected message: " + error);
        return ok;
    };
    const auto tile_delta = [](std::int16_t x, acnet::AccountId actor, acnet::TileChangeCause cause) {
        acnet::ReplicationDelta delta;
        delta.kind = acnet::ResourceKind::Tile;
        delta.zone = 1;
        acnet::TileStateDelta tile;
        tile.address = {1, x, 0};
        tile.state.revision = 2;
        tile.state.item = 0x2801;
        tile.actor = actor;
        tile.cause = cause;
        CHECK(acnet::encode_tile_delta(tile, delta.payload));
        return delta;
    };

    /* Without a baseline there is nothing to apply a delta against. */
    CHECK(client.baseline_serial() == 0);
    CHECK(client.pending_tile_changes() == 0);

    acnet::ZoneBaseline baseline;
    baseline.revision = 5;
    baseline.zone = 1;
    baseline.tiles.emplace_back(acnet::TileAddress{1, 0, 0}, acnet::TileState{});
    std::vector<std::uint8_t> baseline_bytes;
    CHECK(acnet::encode_baseline(baseline, baseline_bytes));
    CHECK(apply(acnet::MessageType::Baseline, baseline_bytes));
    const std::uint32_t serial = client.baseline_serial();
    CHECK(serial == 1);

    std::vector<std::uint8_t> delta_bytes;
    CHECK(acnet::encode_deltas({tile_delta(3, 4242, acnet::TileChangeCause::Drop),
                                tile_delta(4, 0, acnet::TileChangeCause::Server)}, delta_bytes));
    CHECK(apply(acnet::MessageType::ReplicationDeltas, delta_bytes));
    /* A delta must not read as a new baseline: keying the bulk projection on
     * the replication revision is what made every nearby animation change
     * rewrite the whole interest chunk. */
    CHECK(client.baseline_serial() == serial);
    CHECK(client.pending_tile_changes() == 2);
    CHECK(!client.tile_changes_overflowed());

    /* The drain removes exactly what it copies, oldest first, so a reader with
     * a buffer smaller than the queue loses nothing. */
    acnet::TileChange drained[1];
    CHECK(client.drain_tile_changes(drained, 1) == 1);
    CHECK(drained[0].address.x == 3);
    CHECK(drained[0].actor == 4242);
    CHECK(drained[0].cause == acnet::TileChangeCause::Drop);
    CHECK(client.pending_tile_changes() == 1);
    CHECK(client.drain_tile_changes(drained, 1) == 1);
    CHECK(drained[0].address.x == 4);
    CHECK(drained[0].cause == acnet::TileChangeCause::Server);
    CHECK(client.drain_tile_changes(drained, 1) == 0);

    /* A reader that stops draining must be told, not silently starved: the lost
     * change is invisible until the next baseline otherwise. */
    for (std::int16_t x = 0; x < 300; ++x) {
        CHECK(acnet::encode_deltas({tile_delta(static_cast<std::int16_t>(x + 100), 7,
                                                acnet::TileChangeCause::Drop)}, delta_bytes));
        CHECK(apply(acnet::MessageType::ReplicationDeltas, delta_bytes));
    }
    CHECK(client.pending_tile_changes() == 256);
    CHECK(client.tile_changes_overflowed());

    /* A baseline is the whole truth for the chunk, so it supersedes anything
     * queued rather than letting stale changes replay over newer state. */
    baseline.revision = 400;
    CHECK(acnet::encode_baseline(baseline, baseline_bytes));
    CHECK(apply(acnet::MessageType::Baseline, baseline_bytes));
    CHECK(client.baseline_serial() == serial + 1);
    CHECK(client.pending_tile_changes() == 0);
    CHECK(!client.tile_changes_overflowed());
}

void resident_roster_replication() {
    acnet::ResidentRoster roster;
    roster.slots[1].account = 4242;
    roster.slots[1].name = {'N', 'O', 'O', 'K', ' ', ' ', ' ', ' '};
    roster.slots[1].gender = 2;
    roster.slots[1].occupied = true;
    std::vector<std::uint8_t> bytes;
    CHECK(acnet::encode_resident_delta(roster, bytes));
    CHECK(bytes.size() == acnet::kOriginalResidentSlots * 18U);
    acnet::ResidentRoster decoded;
    CHECK(acnet::decode_resident_delta(bytes, decoded));
    CHECK(decoded == roster);
    CHECK(decoded.slots[1].name[0] == 'N');
    CHECK(!decoded.slots[0].occupied);

    /* An all-vacant roster is a legitimate state -- a town nobody has claimed
     * yet -- and must survive the round trip rather than be treated as absent. */
    const acnet::ResidentRoster empty;
    std::vector<std::uint8_t> empty_bytes;
    CHECK(acnet::encode_resident_delta(empty, empty_bytes));
    CHECK(acnet::decode_resident_delta(empty_bytes, decoded));
    CHECK(decoded == empty);

    /* A vacant slot carries no identity, so a peer cannot smuggle a name or an
     * account past a reader that only consults the flag. */
    acnet::ResidentRoster smuggled;
    smuggled.slots[0].account = 7;
    CHECK(!acnet::encode_resident_delta(smuggled, bytes));
    CHECK(acnet::encode_resident_delta(roster, bytes));
    bytes[18] = 0; /* clear slot 1's `occupied` while leaving its payload behind */
    CHECK(!acnet::decode_resident_delta(bytes, decoded));

    /* A gender the game cannot represent, an occupied slot with no account, and
     * a truncated payload are all refused. */
    acnet::ResidentRoster bad_gender = roster;
    bad_gender.slots[1].gender = 3;
    CHECK(!acnet::encode_resident_delta(bad_gender, bytes));
    acnet::ResidentRoster no_account = roster;
    no_account.slots[1].account = 0;
    CHECK(!acnet::encode_resident_delta(no_account, bytes));
    CHECK(acnet::encode_resident_delta(roster, bytes));
    bytes.pop_back();
    CHECK(!acnet::decode_resident_delta(bytes, decoded));

    /* Town-wide: it must reach a viewer standing in a different zone, which is
     * the whole point -- the map shows house owners from inside a house. */
    acnet::ReplicationDelta delta;
    delta.kind = acnet::ResourceKind::Resident;
    delta.zone = 0;
    delta.reliable = true;
    CHECK(acnet::encode_resident_delta(roster, delta.payload));
    std::vector<std::uint8_t> batch_bytes;
    std::vector<acnet::ReplicationDelta> batch{delta};
    CHECK(acnet::encode_deltas(batch, batch_bytes));
    std::vector<acnet::ReplicationDelta> batch_decoded;
    CHECK(acnet::decode_deltas(batch_bytes, batch_decoded));
    CHECK(batch_decoded.size() == 1);
    CHECK(batch_decoded[0].kind == acnet::ResourceKind::Resident);

    acnet::DeltaLog log(4);
    log.append(delta);
    acnet::InterestContext elsewhere;
    elsewhere.account = 4242;
    elsewhere.zone = 101;
    const auto visible = log.since(0, elsewhere, 20);
    CHECK(visible.deltas.size() == 1);
    CHECK(visible.deltas[0].kind == acnet::ResourceKind::Resident);
}

/* The delivery loop against a live server: a client joins, receives the
 * manifest, pulls every chunk of an asset it does not have, and reassembles a
 * blob that hashes back to what was advertised. */
void asset_delivery_serves_a_real_client() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-asset-delivery-" + std::to_string(unique));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};

    const std::filesystem::path town = root / "town";
    std::filesystem::create_directories(town / "mods" / "lantern-set" / "content");
    /* Deliberately several chunks long, so the windowed pull is exercised
     * rather than a single-packet special case. */
    std::string payload_bytes;
    for (std::size_t i = 0; i < 6000; ++i) payload_bytes.push_back(static_cast<char>('a' + (i % 26)));
    {
        std::ofstream out(town / "mods" / "lantern-set" / "content" / "lantern.pcasset",
                          std::ios::binary);
        out << payload_bytes;
    }

    acserver::TownRuntimeConfig config;
    config.port = 0;
    config.data_directory = town;
    config.capacity = 4;
    config.connection_timeout_ms = 60000;
    acserver::TownRuntime runtime(config);
    std::string error;
    constexpr std::int64_t wall_start = 1700000000;
    CHECK(runtime.initialize(wall_start, error));
    CHECK(runtime.mod_packstore().blob_count() == 1);

    acnet::UdpSocket socket;
    acnet::ReliabilityPeer reliability;
    CHECK(socket.open(0, error));
    {
        acnet::ClientHello hello;
        hello.town = config.town_id;
        hello.account = 4242;
        hello.client_nonce = 77;
        std::vector<std::uint8_t> encoded;
        CHECK(acnet::encode(hello, encoded));
        const acnet::PacketHeader header =
            reliability.make_header(acnet::MessageType::ClientHello, acnet::Channel::Control, 0);
        std::vector<std::uint8_t> bytes;
        CHECK(acnet::encode_packet(header, encoded, bytes, error));
        CHECK(socket.send("127.0.0.1", runtime.bound_port(), bytes, error));
    }

    acnet::AssetManifest manifest;
    /* Bulk messages are session-scoped, so the request has to carry the session
     * the server handed back -- a header with session 0 is dropped before it
     * reaches the chunk service. */
    acnet::SessionId session = 0;
    bool have_manifest = false;
    std::map<std::uint32_t, std::vector<std::uint8_t>> received;
    std::uint32_t total_chunks = 0;
    bool requested = false;

    const std::uint64_t monotonic_start = acserver::monotonic_milliseconds();
    for (int frame = 0; frame < 400; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(runtime.step(monotonic_start + static_cast<std::uint64_t>(frame) * 16, wall_start, error));

        acnet::Datagram datagram;
        while (socket.receive(datagram, error)) {
            acnet::DecodedPacket packet;
            std::string decode_error;
            if (!acnet::decode_packet(datagram.bytes.data(), datagram.bytes.size(), packet, decode_error))
                continue;

            if (packet.header.message_type == acnet::MessageType::ServerHello) {
                acnet::ServerHello hello;
                if (acnet::decode(packet.payload, hello)) session = hello.session;
            } else if (packet.header.message_type == acnet::MessageType::AssetManifest) {
                CHECK(acnet::decode(packet.payload, manifest));
                CHECK(manifest.entries.size() == 1);
                have_manifest = true;
            } else if (packet.header.message_type == acnet::MessageType::AssetChunk) {
                acnet::AssetChunk chunk;
                CHECK(acnet::decode(packet.payload, chunk));
                total_chunks = chunk.total_chunks;
                received[chunk.index] = chunk.bytes;
            }
        }

        /* Ask once the manifest names something we do not have. A real client
         * would window this; one request covering every chunk is enough to
         * prove the service answers exactly what was asked. */
        if (have_manifest && session != 0 && !requested) {
            acnet::AssetChunkRequest request;
            request.hash = manifest.entries[0].hash;
            request.first_chunk = 0;
            request.chunk_count = static_cast<std::uint16_t>(acnet::kAssetWindowChunks);
            std::vector<std::uint8_t> encoded;
            CHECK(acnet::encode(request, encoded));
            const acnet::PacketHeader header = reliability.make_header(
                acnet::MessageType::AssetChunkRequest, acnet::Channel::Bulk, session);
            std::vector<std::uint8_t> out;
            CHECK(acnet::encode_packet(header, encoded, out, error));
            CHECK(socket.send("127.0.0.1", runtime.bound_port(), out, error));
            requested = true;
        }

        if (total_chunks != 0 && received.size() == total_chunks) break;
    }

    CHECK(have_manifest);
    CHECK(total_chunks > 1);              /* a windowed pull, not one packet */
    CHECK(received.size() == total_chunks);

    std::vector<std::uint8_t> rebuilt;
    for (std::uint32_t i = 0; i < total_chunks; ++i) {
        const auto found = received.find(i);
        CHECK(found != received.end());
        rebuilt.insert(rebuilt.end(), found->second.begin(), found->second.end());
    }
    CHECK(rebuilt.size() == payload_bytes.size());
    CHECK(std::equal(rebuilt.begin(), rebuilt.end(), payload_bytes.begin()));
    /* The delivered bytes hash to what the manifest advertised. This is the
     * whole integrity guarantee: identification and verification are the same
     * operation, which is why no signature is needed. */
    CHECK(acnet::sha256(rebuilt.data(), rebuilt.size()) == manifest.entries[0].hash);

    CHECK(runtime.shutdown(error));
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
        CHECK(first.frame(now, 26000, 0, 0, 0, {}, {}, first_local, corrected, has_correction, error));
        CHECK(!has_correction);
        if (has_correction) first_local = corrected;
        CHECK(second.frame(now, -26000, 0, 0, 0, {}, {}, second_local, corrected, has_correction, error));
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
        CHECK(first.frame(now, 0, 0, 0, 0, {}, {}, first_local, corrected, has_correction, error));
        CHECK(!has_correction);
        if (has_correction) first_local = corrected;
        CHECK(second.frame(now, 0, 0, 0, 0, {}, {}, second_local, corrected, has_correction, error));
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
        CHECK(first.frame(++settle_now, 0, 0, 0, 0, {}, {}, first_local, corrected, has_correction, error));
        CHECK(!has_correction);
        CHECK(second.frame(settle_now, 0, 0, 0, 0, {}, {}, second_local, corrected, has_correction, error));
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

    acnet::AppearanceUpdate patterned;
    patterned.appearance.name = {{'P', 'a', 't', 't', 'e', 'r', 'n', ' '}};
    patterned.appearance.gender = 0;
    patterned.appearance.face = 2;
    patterned.appearance.clothing = 0xFE20;
    patterned.appearance.clothing_index = 0x106;
    patterned.pattern.present = true;
    patterned.pattern.palette = 13;
    for (std::size_t i = 0; i < patterned.pattern.texture.size(); ++i)
        patterned.pattern.texture[i] = static_cast<std::uint8_t>(i ^ 0xA5U);
    CHECK(first.update_appearance(patterned, ++settle_now, error));
    bool pattern_converged = false;
    for (std::uint64_t i = 0; i < 200 && !pattern_converged; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(server.step(++settle_now, wall + 5, error));
        CHECK(first.poll(settle_now, error));
        CHECK(second.poll(settle_now, error));
        second_remotes = second.remote_players();
        pattern_converged = second_remotes.size() == 1 &&
                            second_remotes[0].appearance.clothing_index == 0x106 &&
                            second_remotes[0].appearance.revision != 0 &&
                            second_remotes[0].pattern.present &&
                            second_remotes[0].pattern.palette == 13 &&
                            second_remotes[0].pattern.texture == patterned.pattern.texture;
    }
    CHECK(pattern_converged);

    /* Each client swings something distinctive so the presentation delta is
     * exercised end to end: neither animation is what the other started in, so
     * arriving at it can only have come over the wire. */
    acnet::PlayerAnimation first_animation;
    first_animation.body = 31;  /* mPlayer_ANIM_NET1 */
    first_animation.overlay = 32;
    first_animation.part_table = 3; /* mPlayer_PART_TABLE_NET */
    first_animation.item_state = 2; /* mPlayer_ITEM_MAIN_NET_NORMAL */
    first_animation.looping = false;
    acnet::PlayerAnimation second_animation;
    second_animation.body = 2; /* mPlayer_ANIM_AXE1 */
    second_animation.overlay = 2;
    second_animation.part_table = 1; /* mPlayer_PART_TABLE_AXE */
    second_animation.item_state = 1; /* mPlayer_ITEM_MAIN_AXE_NORMAL */
    second_animation.reversed = true;

    /* Both accounts took an original resident slot, so each client must be able
     * to name the owner of every occupied house -- including, after the update
     * above, the other player's current name. This is what the town map reads;
     * a client's own save knows only the account it logged in as. */
    auto find_resident = [](const acnet::ResidentRoster& roster, acnet::AccountId account) {
        return std::find_if(roster.slots.begin(), roster.slots.end(),
                            [account](const acnet::ResidentIdentity& resident) {
                                return resident.occupied && resident.account == account;
                            });
    };
    auto roster_converged = [&](const acnet::ClientRuntime& viewer) {
        if (!viewer.has_residents()) return false;
        const acnet::ResidentRoster& roster = viewer.residents();
        const auto owner = find_resident(roster, first_config.account);
        return owner != roster.slots.end() && owner->name == patterned.appearance.name &&
               find_resident(roster, second_config.account) != roster.slots.end() &&
               /* The two remaining slots are authoritatively vacant, not unreported. */
               std::count_if(roster.slots.begin(), roster.slots.end(),
                             [](const acnet::ResidentIdentity& resident) { return resident.occupied; }) == 2;
    };
    bool residents_converged = false;
    for (std::uint64_t i = 0; i < 200 && !residents_converged; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(server.step(++settle_now, wall + 5, error));
        CHECK(first.poll(settle_now, error));
        CHECK(second.poll(settle_now, error));
        residents_converged = roster_converged(first) && roster_converged(second);
    }
    CHECK(residents_converged);

    /* Unlike every other convergence loop in this test this one used to spin
     * without yielding, giving real UDP loopback 120 iterations of simulated
     * time but almost no wall time to deliver in. It failed roughly one run in
     * five standalone and near enough every run under `make check`, where the
     * machine is busier -- a flake that made the release gate untrustworthy
     * rather than a defect in the code under test. Matched to its neighbours:
     * yield each iteration, and allow the same budget they do. */
    bool actions_converged = false;
    for (std::uint64_t i = 0; i < 300 && !actions_converged; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++settle_now;
        acnet::Transform corrected;
        bool has_correction = false;
        CHECK(first.frame(settle_now, 0, 0, 0, 119, first_animation, {}, first_local, corrected, has_correction, error));
        CHECK(!has_correction);
        CHECK(second.frame(settle_now, 0, 0, 0, 120, second_animation, {}, second_local, corrected, has_correction, error));
        CHECK(!has_correction);
        CHECK(server.step(settle_now, wall + 5, error));
        CHECK(first.poll(settle_now, error));
        CHECK(second.poll(settle_now, error));
        first_remotes = first.remote_players();
        second_remotes = second.remote_players();
        actions_converged = first_remotes.size() == 1 && second_remotes.size() == 1 &&
                            first_remotes[0].transform.action == 120 &&
                            second_remotes[0].transform.action == 119 &&
                            first_remotes[0].presentation.animation == second_animation &&
                            second_remotes[0].presentation.animation == first_animation;
    }
    CHECK(actions_converged);
    settle_now += 40;
    {
        acnet::Transform corrected;
        bool has_correction = false;
        CHECK(first.frame(settle_now, 0, 0, 0, 0, {}, {}, first_local, corrected, has_correction, error));
        CHECK(!has_correction);
        CHECK(second.frame(settle_now, 0, 0, 0, 0, {}, {}, second_local, corrected, has_correction, error));
        CHECK(!has_correction);
        CHECK(server.step(settle_now, wall + 5, error));
        CHECK(first.poll(settle_now, error));
        CHECK(second.poll(settle_now, error));
    }
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
    house_update.surfaces.wallpaper[0] = 21;
    house_update.surfaces.flooring[0] = 22;
    house_update.surfaces.pattern_bits[0] = 2;
    house_update.surfaces.exterior_palette = 3;
    house_update.surfaces.door_design = 4;
    house_update.music_box[0] = 0x1234u;
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
    /* The visitor, not just the owner: this is the whole point of replicating
     * the surfaces at all. */
    CHECK(second.baseline()->house.surfaces.wallpaper[0] == 21);
    CHECK(second.baseline()->house.surfaces.flooring[0] == 22);
    CHECK(second.baseline()->house.surfaces.pattern_bits[0] == 2);
    CHECK(second.baseline()->house.surfaces.exterior_palette == 3);
    CHECK(second.baseline()->house.surfaces.door_design == 4);
    CHECK(second.baseline()->house.surfaces == first.baseline()->house.surfaces);
    CHECK(second.baseline()->house.music_box[0] == 0x1234u);

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

    /* Catching needs the rod in hand, not merely owned, so hold it first --
     * the same transaction the pocket menu and the L/R tool cycle issue. */
    acnet::EconomyRequest hold;
    hold.type = acnet::EconomyOpType::HoldItem;
    hold.idempotency = {704, 705};
    hold.expected_inventory_revision = first.baseline()->inventory.revision;
    hold.inventory_slot = 0;
    hold.expected_item = 0x2203;
    CHECK(first.baseline()->inventory.slots[0].item == 0x2203);
    CHECK(first.baseline()->inventory.equipped.item == 0);
    CHECK(first.request(hold, transaction_now, error));
    std::optional<acnet::EconomyResult> hold_result;
    for (std::uint64_t i = 0; i < 30 && !hold_result.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++transaction_now;
        CHECK(server.step(transaction_now, wall + 5, error));
        CHECK(first.poll(transaction_now, error));
        hold_result = first.take_economy_result();
    }
    CHECK(hold_result.has_value());
    CHECK(hold_result->code == acnet::ResultCode::Ok);
    /* A swap: the rod left the pocket rather than being copied out of it. */
    CHECK(first.baseline()->inventory.equipped.item == 0x2203);
    CHECK(first.baseline()->inventory.slots[0].item == 0);
    /* And the other client can see it in their hand. */
    bool held_item_converged = false;
    for (std::uint64_t i = 0; i < 60 && !held_item_converged; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++transaction_now;
        CHECK(server.step(transaction_now, wall + 5, error));
        CHECK(first.poll(transaction_now, error));
        CHECK(second.poll(transaction_now, error));
        second_remotes = second.remote_players();
        held_item_converged = second_remotes.size() == 1 &&
                              second_remotes[0].presentation.equipped_item == 0x2203;
    }
    CHECK(held_item_converged);

    acnet::EncounterRequest encounter;
    encounter.account = 999999; // The server must replace this with the authenticated account.
    encounter.idempotency = {700, 701};
    encounter.expected_inventory_revision = first.baseline()->inventory.revision;
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
    /* Put the rod away again -- the same swap in the other direction, into a
     * slot the catch above cannot have filled -- so there is something in a
     * pocket to drop. */
    acnet::EconomyRequest putaway;
    putaway.type = acnet::EconomyOpType::HoldItem;
    putaway.idempotency = {706, 707};
    putaway.expected_inventory_revision = first.baseline()->inventory.revision;
    putaway.inventory_slot = 5;
    CHECK(first.request(putaway, ++transaction_now, error));
    std::optional<acnet::EconomyResult> putaway_result;
    for (std::uint64_t i = 0; i < 30 && !putaway_result.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++transaction_now;
        CHECK(server.step(transaction_now, wall + 5, error));
        CHECK(first.poll(transaction_now, error));
        putaway_result = first.take_economy_result();
    }
    CHECK(putaway_result.has_value());
    CHECK(putaway_result->code == acnet::ResultCode::Ok);
    CHECK(first.baseline()->inventory.equipped.item == 0);
    CHECK(first.baseline()->inventory.slots[5].item == 0x2203);

    acnet::WorldOperation drop;
    drop.type = acnet::WorldOpType::DropItem;
    drop.account = 999999;
    drop.idempotency = {702, 703};
    drop.tile = {1, transaction_x, transaction_z};
    drop.expected_tile_revision = target->second.revision;
    drop.expected_inventory_revision = first.baseline()->inventory.revision;
    drop.inventory_slot = 5;
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
    CHECK(first.baseline()->inventory.slots[5].item == 0);
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

/* A player who steps indoors and comes back must still look like themselves to
 * everyone who stayed put.
 *
 * Appearance is baseline-owned so the transform snapshot stays under the MTU,
 * and baselines used to reach only the client that joined or transferred. A
 * viewer therefore dropped the traveller's whole track while they were gone and
 * picked them back up from the snapshot alone, with a default-constructed
 * appearance -- wrong gender, wrong face, wrong shirt -- and nothing ever
 * corrected it, because the traveller's appearance had not changed and so no
 * AppearanceUpdate was broadcast. Using a door corrupted how every other player
 * saw you for the rest of the session. */
void appearance_survives_a_zone_round_trip() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-zone-appearance-" + std::to_string(unique));
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
    server_config.invite_key = "zone-appearance-key";
    acserver::TownRuntime server(server_config);
    std::string error;
    constexpr std::int64_t wall = 1700000000;
    CHECK(server.initialize(wall, error));

    acnet::ClientConfig stayer_config;
    stayer_config.server_port = server.bound_port();
    stayer_config.town = server_config.town_id;
    stayer_config.account = 81;
    stayer_config.invite_key = server_config.invite_key;
    acnet::ClientConfig traveller_config = stayer_config;
    traveller_config.account = 82;
    acnet::ClientRuntime stayer(stayer_config);
    acnet::ClientRuntime traveller(traveller_config);
    constexpr std::uint64_t start = 10000;
    CHECK(stayer.start(start, error));
    CHECK(traveller.start(start, error));

    acnet::Transform stayer_local;
    stayer_local.position = {2200.0F, 0.0F, 1000.0F};
    acnet::Transform traveller_local = stayer_local;
    traveller_local.position.x = 2240.0F;

    std::uint64_t now = start;
    const auto pump = [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        acnet::Transform corrected;
        bool has_correction = false;
        CHECK(stayer.frame(++now, 0, 0, 0, 0, {}, {}, stayer_local, corrected, has_correction, error));
        CHECK(traveller.frame(now, 0, 0, 0, 0, {}, {}, traveller_local, corrected, has_correction, error));
        CHECK(server.step(now, wall + 5, error));
        CHECK(stayer.poll(now, error));
        CHECK(traveller.poll(now, error));
    };
    for (std::uint64_t i = 0; i < 240; ++i) pump();
    CHECK(stayer.state() == acnet::ClientConnectionState::Connected);
    CHECK(traveller.state() == acnet::ClientConnectionState::Connected);

    /* Nothing about this appearance is a default, so seeing it later can only
     * mean it survived rather than being coincidentally reconstructed. */
    acnet::AppearanceUpdate distinctive;
    distinctive.appearance.name = {{'T', 'r', 'a', 'v', 'e', 'l', 'e', 'r'}};
    distinctive.appearance.gender = 1;
    distinctive.appearance.face = 6;
    distinctive.appearance.clothing = 0xFE20;
    distinctive.appearance.clothing_index = 0x42;
    CHECK(traveller.update_appearance(distinctive, ++now, error));

    const auto stayer_sees_traveller = [&]() -> std::optional<acnet::RemotePresentation> {
        for (const acnet::RemotePresentation& remote : stayer.remote_players())
            if (remote.account == traveller_config.account) return remote;
        return std::nullopt;
    };

    bool appearance_arrived = false;
    for (std::uint64_t i = 0; i < 300 && !appearance_arrived; ++i) {
        pump();
        const auto seen = stayer_sees_traveller();
        appearance_arrived = seen.has_value() && seen->appearance.gender == 1 &&
                             seen->appearance.face == 6 && seen->appearance.clothing_index == 0x42;
    }
    CHECK(appearance_arrived);

    const auto transfer = [&](std::uint32_t door_id, acnet::ZoneId destination, const acnet::Vec3& arrival) {
        acnet::ZoneTransferRequest request;
        request.door_id = door_id;
        CHECK(traveller.request(request, ++now, error));
        std::optional<acnet::TransferOffer> offer;
        for (std::uint64_t i = 0; i < 300 && !offer.has_value(); ++i) {
            pump();
            offer = traveller.take_transfer_offer();
        }
        CHECK(offer.has_value());
        CHECK(offer->code == acnet::ResultCode::Ok);
        CHECK(offer->destination_zone == destination);
        acnet::ZoneReadyRequest ready;
        ready.token = offer->token;
        ready.destination_transform.position = arrival;
        CHECK(traveller.ready(ready, ++now, error));
        bool arrived = false;
        for (std::uint64_t i = 0; i < 300 && !arrived; ++i) {
            pump();
            arrived = traveller.baseline() != nullptr && traveller.baseline()->zone == destination;
        }
        CHECK(arrived);
        (void)traveller.take_transfer_offer();
    };

    /* Indoors. The stayer must stop drawing them -- a vanished player standing
     * frozen in the doorway would be its own bug. */
    transfer(100, 100, {120.0F, 0.0F, 220.0F});
    bool traveller_left_view = false;
    for (std::uint64_t i = 0; i < 400 && !traveller_left_view; ++i) {
        pump();
        traveller_left_view = !stayer_sees_traveller().has_value();
    }
    CHECK(traveller_left_view);

    /* Back out through the return door, without touching their appearance. */
    transfer(200, 1, {2240.0F, 0.0F, 1000.0F});
    bool traveller_returned = false;
    for (std::uint64_t i = 0; i < 400 && !traveller_returned; ++i) {
        pump();
        traveller_returned = stayer_sees_traveller().has_value();
    }
    CHECK(traveller_returned);

    const auto returned = stayer_sees_traveller();
    CHECK(returned.has_value());
    if (returned->appearance.gender != 1 || returned->appearance.face != 6 ||
        returned->appearance.clothing_index != 0x42) {
        throw TestFailure("traveller returned wearing a default appearance: gender=" +
                          std::to_string(static_cast<int>(returned->appearance.gender)) +
                          " face=" + std::to_string(static_cast<int>(returned->appearance.face)) +
                          " clothing_index=" + std::to_string(returned->appearance.clothing_index));
    }
    CHECK(returned->appearance.name == distinctive.appearance.name);
    CHECK(returned->appearance.revision != 0);

    /* The stayer's own baseline must carry it too, which is the half of the fix
     * that lives on the server: occupants of the destination zone are
     * re-baselined when someone arrives, exactly as they are when someone
     * joins. Retention on the client alone would leave any absence longer than
     * the retention window still broken. */
    CHECK(stayer.baseline() != nullptr);
    bool baseline_has_traveller = false;
    for (const acnet::PlayerSnapshot& player : stayer.baseline()->players) {
        if (player.account != traveller_config.account) continue;
        baseline_has_traveller = player.appearance.gender == 1 && player.appearance.face == 6 &&
                                 player.appearance.clothing_index == 0x42;
    }
    CHECK(baseline_has_traveller);

    CHECK(server.shutdown(error));
}

/* A town can be recorded as created without ever being given a foreground:
 * before the client refused to send one, a bootstrap submitted while the save's
 * field was still blank installed 7680 empty tiles and closed the only path
 * that installs a world. The town then answered every interest window with an
 * all-empty chunk, which the client writes straight over its own field -- the
 * trees, rocks and bulletin board vanish an acre at a time as the player walks
 * up to them, and never come back. A restart must reopen the door. */
void blank_town_bootstrap_is_repairable_after_restart() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-blank-bootstrap-" + std::to_string(unique));
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
    server_config.town_name = "BlankTown";
    server_config.town_seed = 77;
    constexpr std::int64_t wall = 1700000000;
    std::uint64_t now = 1;
    std::string error;

    const auto make_bootstrap = [&](std::uint16_t fill_item) {
        acnet::TownBootstrap bootstrap;
        bootstrap.town_seed = server_config.town_seed;
        bootstrap.land_id = static_cast<std::uint16_t>(0x3000U | (server_config.town_seed & 0xFFU));
        bootstrap.town_name = {{'B', 'l', 'a', 'n', 'k', 'T', 'o', 'w'}};
        bootstrap.appearance.name = {{'P', 'l', 'a', 'y', 'e', 'r', ' ', ' '}};
        bootstrap.appearance.face = 1;
        bootstrap.tiles.resize(acnet::kTownBootstrapTileCount);
        for (auto& tile : bootstrap.tiles) tile.item = fill_item;
        return bootstrap;
    };

    const auto run_until = [&](acserver::TownRuntime& server, acnet::ClientRuntime& client,
                               const std::function<bool()>& done) {
        for (std::uint64_t i = 0; i < 800 && !done(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++now;
            CHECK(server.step(now, wall, error));
            CHECK(client.poll(now, error));
        }
    };

    /* Install a foreground of nothing, exactly as the old client could. */
    {
        acserver::TownRuntime server(server_config);
        CHECK(server.initialize(wall, error));
        acnet::ClientConfig config;
        config.server_port = server.bound_port();
        config.account = 9101;
        acnet::ClientRuntime client(config);
        CHECK(client.start(++now, error));
        run_until(server, client,
                  [&] { return client.state() == acnet::ClientConnectionState::Connected; });
        CHECK(client.state() == acnet::ClientConnectionState::Connected);
        CHECK(!client.town_initialized());

        CHECK(client.submit_town_bootstrap(make_bootstrap(0), ++now, error));
        std::optional<acnet::TownBootstrapResult> result;
        run_until(server, client, [&] {
            result = client.take_town_bootstrap_result();
            return result.has_value();
        });
        CHECK(result.has_value());
        CHECK(result->code == acnet::ResultCode::Ok);
        CHECK(client.town_initialized());
        client.stop(++now);
        for (std::uint64_t i = 0; i < 20 && server.connected_clients() != 0; ++i)
            CHECK(server.step(++now, wall, error));
        CHECK(server.shutdown(error));
    }

    /* Restarting must not carry the empty world forward as authoritative. */
    acserver::TownRuntime restarted(server_config);
    CHECK(restarted.initialize(wall + 30, error));
    acnet::ClientConfig returning_config;
    returning_config.server_port = restarted.bound_port();
    returning_config.account = 9101;
    acnet::ClientRuntime returning(returning_config);
    CHECK(returning.start(++now, error));
    run_until(restarted, returning,
              [&] { return returning.state() == acnet::ClientConnectionState::Connected; });
    CHECK(returning.state() == acnet::ClientConnectionState::Connected);
    CHECK(!returning.town_initialized());

    /* And a real foreground now installs and sticks. */
    CHECK(returning.submit_town_bootstrap(make_bootstrap(0x0804), ++now, error));
    std::optional<acnet::TownBootstrapResult> repaired;
    run_until(restarted, returning, [&] {
        repaired = returning.take_town_bootstrap_result();
        return repaired.has_value();
    });
    CHECK(repaired.has_value());
    CHECK(repaired->code == acnet::ResultCode::Ok);
    CHECK(returning.town_initialized());
    /* The repaired world reaches the client on its next baseline. */
    const auto has_planted_tile = [&] {
        const acnet::ZoneBaseline* baseline = returning.baseline();
        if (baseline == nullptr) return false;
        return std::any_of(baseline->tiles.begin(), baseline->tiles.end(),
                           [](const auto& entry) { return entry.first.zone == 1 && entry.second.item == 0x0804; });
    };
    run_until(restarted, returning, has_planted_tile);
    CHECK(has_planted_tile());
    returning.stop(++now);
    for (std::uint64_t i = 0; i < 20 && restarted.connected_clients() != 0; ++i)
        CHECK(restarted.step(++now, wall + 30, error));
    CHECK(restarted.shutdown(error));
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

    /* A real foreground, not a single tile on an empty field: a restored town
     * whose foreground is essentially empty is treated as never installed, so
     * that a town blank-bootstrapped by the old client can be repaired instead
     * of answering every interest window with an eraser. The fill still differs
     * from the overwrite attempt below, which is what this case is about. */
    auto canonical = make_bootstrap(0x1111, 2);
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

void island_is_an_authoritative_shared_zone() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("acgc-island-runtime-" + std::to_string(unique));
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
    server_config.town_name = "IsleTown";
    server_config.town_seed = 77;
    constexpr std::int64_t wall = 1700000000;
    std::string error;

    /* Island acres 4 and 5 of row kIslandBlockZ, which is the layout the
     * original field generator produces; the client reports it rather than the
     * server assuming it. */
    constexpr std::uint8_t kLeftAcre = 4;
    constexpr std::uint8_t kRightAcre = 5;
    const auto make_bootstrap = [&](bool with_island) {
        acnet::TownBootstrap bootstrap;
        bootstrap.town_seed = server_config.town_seed;
        bootstrap.land_id = 0x304D;
        bootstrap.town_name = {{'I', 's', 'l', 'e', 'T', 'o', 'w', 'n'}};
        bootstrap.appearance.name = {{'P', 'l', 'a', 'y', 'e', 'r', ' ', ' '}};
        bootstrap.appearance.clothing = 0x2401;
        bootstrap.tiles.resize(acnet::kTownBootstrapTileCount);
        if (with_island) {
            bootstrap.island_block_x = {{kLeftAcre, kRightAcre}};
            bootstrap.island_tiles.resize(acnet::kIslandBootstrapTileCount);
            /* A coconut palm in the left acre, unit (2,3). */
            bootstrap.island_tiles[(3U * 16U) + 2U].item = 0x1234;
        }
        return bootstrap;
    };

    acserver::TownRuntime server(server_config);
    CHECK(server.initialize(wall, error));
    /* Before any client reports the acre layout the island has no tiles, and
     * the console must say so rather than claim an empty island is ready. */
    CHECK(!server.island_status().terrain_ready);

    acnet::ClientConfig first_config;
    first_config.server_port = server.bound_port();
    first_config.account = 9201;
    acnet::ClientConfig second_config = first_config;
    second_config.account = 9202;
    acnet::ClientRuntime first(first_config);
    acnet::ClientRuntime second(second_config);
    CHECK(first.start(1000, error));
    CHECK(second.start(1000, error));
    std::uint64_t now = 1000;
    const auto pump = [&](std::uint64_t iterations) {
        for (std::uint64_t i = 0; i < iterations; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++now;
            CHECK(server.step(now, wall, error));
            CHECK(first.poll(now, error));
            CHECK(second.poll(now, error));
        }
    };
    for (std::uint64_t i = 0; i < 300 &&
         (first.state() != acnet::ClientConnectionState::Connected ||
          second.state() != acnet::ClientConnectionState::Connected); ++i) pump(1);
    CHECK(first.state() == acnet::ClientConnectionState::Connected);
    CHECK(second.state() == acnet::ClientConnectionState::Connected);

    /* A first login that could not read the field layout still creates the
     * town; the island simply stays empty until one can. */
    CHECK(first.submit_town_bootstrap(make_bootstrap(false), ++now, error));
    std::optional<acnet::TownBootstrapResult> result;
    for (std::uint64_t i = 0; i < 800 && !result.has_value(); ++i) {
        pump(1);
        result = first.take_town_bootstrap_result();
    }
    CHECK(result.has_value());
    CHECK(result->code == acnet::ResultCode::Ok);
    CHECK(server.town_initialized());
    CHECK(!server.island_status().terrain_ready);

    /* The next login that can read it is adopted, even though the town itself
     * is already bootstrapped -- this is the path a town created before island
     * support takes. */
    CHECK(second.submit_town_bootstrap(make_bootstrap(true), ++now, error));
    result.reset();
    for (std::uint64_t i = 0; i < 800 && !result.has_value(); ++i) {
        pump(1);
        result = second.take_town_bootstrap_result();
    }
    CHECK(result.has_value());
    CHECK(result->code == acnet::ResultCode::Ok);
    acserver::TownRuntime::IslandStatus island = server.island_status();
    CHECK(island.terrain_ready);
    CHECK(island.tiles == acnet::kIslandTileCount);
    CHECK(island.islander_present);
    CHECK(island.outdoor_players == 0);

    /* Island tiles keep global unit coordinates, so the palm is at the acre's
     * own offset in the shared grid rather than at an island-local origin. */
    const auto* palm = server.tile(acnet::kIslandZone,
                                   static_cast<std::int16_t>(kLeftAcre * 16 + 2),
                                   static_cast<std::int16_t>(acnet::kIslandBlockZ * 16 + 3));
    CHECK(palm != nullptr);
    CHECK(palm->item == 0x1234);
    /* A town tile at the same coordinates does not exist: the island acres sit
     * outside the town rectangle, so the two zones cannot collide. */
    CHECK(server.tile(1, static_cast<std::int16_t>(kLeftAcre * 16 + 2),
                      static_cast<std::int16_t>(acnet::kIslandBlockZ * 16 + 3)) == nullptr);

    first.stop(++now);
    second.stop(++now);
    for (std::uint64_t i = 0; i < 20 && server.connected_clients() != 0; ++i)
        CHECK(server.step(++now, wall, error));
    CHECK(server.shutdown(error));

    /* The island survives a restart on the same journal/checkpoint path the
     * town does -- its tiles carry their zone, so nothing special is needed. */
    acserver::TownRuntime restarted(server_config);
    CHECK(restarted.initialize(wall + 30, error));
    island = restarted.island_status();
    CHECK(island.terrain_ready);
    CHECK(island.tiles == acnet::kIslandTileCount);
    palm = restarted.tile(acnet::kIslandZone, static_cast<std::int16_t>(kLeftAcre * 16 + 2),
                          static_cast<std::int16_t>(acnet::kIslandBlockZ * 16 + 3));
    CHECK(palm != nullptr && palm->item == 0x1234);

    /* A later login still reports its island every time it connects. That must
     * not overwrite an island the town has already been played on, the same way
     * a second town bootstrap cannot replace the world. */
    acnet::ClientConfig returning_config;
    returning_config.server_port = restarted.bound_port();
    returning_config.account = 9201;
    acnet::ClientRuntime returning(returning_config);
    CHECK(returning.start(++now, error));
    for (std::uint64_t i = 0; i < 500 && returning.state() != acnet::ClientConnectionState::Connected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++now;
        CHECK(restarted.step(now, wall + 30, error));
        CHECK(returning.poll(now, error));
    }
    CHECK(returning.state() == acnet::ClientConnectionState::Connected);
    auto overwrite = make_bootstrap(true);
    overwrite.island_tiles[(3U * 16U) + 2U].item = 0x7777;
    CHECK(returning.submit_town_bootstrap(std::move(overwrite), ++now, error));
    result.reset();
    for (std::uint64_t i = 0; i < 800 && !result.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++now;
        CHECK(restarted.step(now, wall + 30, error));
        CHECK(returning.poll(now, error));
        result = returning.take_town_bootstrap_result();
    }
    CHECK(result.has_value());
    CHECK(result->code == acnet::ResultCode::Ok);
    palm = restarted.tile(acnet::kIslandZone, static_cast<std::int16_t>(kLeftAcre * 16 + 2),
                          static_cast<std::int16_t>(acnet::kIslandBlockZ * 16 + 3));
    CHECK(palm != nullptr && palm->item == 0x1234);
    returning.stop(++now);
    CHECK(restarted.shutdown(error));
}

void island_cabin_is_a_shared_room() {
    acnet::PlayerDirectory players;
    for (std::uint64_t account = 1; account <= 3; ++account) {
        acnet::PlayerView player;
        player.account = account;
        player.entity = 300 + account;
        player.zone = acnet::kIslandCabinZone;
        player.interaction_eligible = true;
        CHECK(players.upsert(player));
    }
    /* Account 3 is on the shore, not inside the cabin. */
    players.by_account(3)->zone = acnet::kIslandZone;

    acnet::WorldAuthority world(&players);
    for (std::uint64_t account = 1; account <= 3; ++account) {
        acnet::InventoryState inventory;
        inventory.slots[0].item = 0x2001; /* a piece of furniture */
        CHECK(world.set_inventory(account, inventory));
    }
    acnet::HousingAuthority housing(&world, &players);
    CHECK(housing.register_shared_house(acnet::kIslandCabinHouseId, acnet::kIslandCabinZone));
    /* Re-registering is the no-op a restart performs after restoring state. */
    CHECK(housing.register_shared_house(acnet::kIslandCabinHouseId, acnet::kIslandCabinZone));
    CHECK(housing.shared_house_in(acnet::kIslandCabinZone) != nullptr);
    /* A shared house has no owner, so it must not answer an owner lookup and
     * must not consume one of the four resident slots. */
    CHECK(housing.house_for(1) == nullptr);
    CHECK(housing.resident_count() == 0);

    const acnet::HouseState* cabin = housing.house(acnet::kIslandCabinHouseId);
    CHECK(cabin != nullptr && cabin->shared && cabin->owner == 0);
    CHECK(cabin->original_slot == acnet::kSharedHouseSlot);

    /* Anyone standing in the cabin may decorate it -- that presence is the
     * whole authorization, because Save_t.island.cottage belongs to the town. */
    acnet::FurnitureOperation place;
    place.type = acnet::FurnitureOpType::Place;
    place.account = 1;
    place.idempotency = {5, 1};
    place.house_id = acnet::kIslandCabinHouseId;
    place.address = {4, 6, 0, 0};
    place.expected_house_revision = cabin->revision;
    place.expected_inventory_revision = world.inventory(1)->revision;
    place.inventory_slot = 0;
    place.expected_item = 0x2001;
    const acnet::FurnitureResult placed = housing.apply(place);
    CHECK(placed.code == acnet::ResultCode::Ok);
    CHECK(world.inventory(1)->slots[0].item == 0);
    CHECK(housing.apply(place).replayed);

    /* And a second occupant may take it straight back out again: it is not
     * anybody's property. This is the behaviour that makes the cabin usable as
     * shared storage, and it is also the reason presence is enforced. */
    acnet::FurnitureOperation take;
    take.type = acnet::FurnitureOpType::Remove;
    take.account = 2;
    take.idempotency = {5, 2};
    take.house_id = acnet::kIslandCabinHouseId;
    take.address = {4, 6, 0, 0};
    take.expected_house_revision = housing.house(acnet::kIslandCabinHouseId)->revision;
    take.expected_inventory_revision = world.inventory(2)->revision;
    const acnet::FurnitureResult taken = housing.apply(take);
    CHECK(taken.code == acnet::ResultCode::Ok);
    CHECK(taken.item == 0x2001);

    /* Someone outside the cabin cannot reach into it. */
    acnet::FurnitureOperation remote = place;
    remote.account = 3;
    remote.idempotency = {5, 3};
    remote.expected_house_revision = housing.house(acnet::kIslandCabinHouseId)->revision;
    remote.expected_inventory_revision = world.inventory(3)->revision;
    CHECK(housing.apply(remote).code == acnet::ResultCode::OutOfRange);

    /* The cabin is one room. A floor index that only a resident house has is
     * refused rather than silently written somewhere. */
    acnet::FurnitureOperation upstairs = place;
    upstairs.account = 1;
    upstairs.idempotency = {5, 4};
    upstairs.address = {4, 6, 1, 0};
    upstairs.expected_house_revision = housing.house(acnet::kIslandCabinHouseId)->revision;
    upstairs.expected_inventory_revision = world.inventory(1)->revision;
    CHECK(housing.apply(upstairs).code == acnet::ResultCode::Unauthorized);

    /* The baseline carries a shared house by its ownerless (owner, slot) pair,
     * and the decoder has to reconstruct `shared` from it or the client would
     * treat the cabin as a resident house it does not own. */
    acnet::ZoneBaseline baseline;
    baseline.zone = acnet::kIslandCabinZone;
    baseline.revision = 4;
    baseline.inventory.revision = 1;
    baseline.ledger.revision = 1;
    baseline.shop.revision = 1;
    baseline.mailbox.revision = 1;
    baseline.has_house = true;
    baseline.house = *housing.house(acnet::kIslandCabinHouseId);
    std::vector<std::uint8_t> payload;
    CHECK(acnet::encode_baseline(baseline, payload));
    acnet::ZoneBaseline decoded;
    CHECK(acnet::decode_baseline(payload, decoded));
    CHECK(decoded.has_house);
    CHECK(decoded.house.shared);
    CHECK(decoded.house.owner == 0);
    CHECK(decoded.house.original_slot == acnet::kSharedHouseSlot);
    CHECK(decoded.house.house_id == acnet::kIslandCabinHouseId);
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
        {"held items are a conserving swap", held_items_are_a_conserving_server_swap},
        {"player presentation replication", player_presentation_round_trips_and_is_bounded},
        {"runtime journal replay", runtime_replays_uncheckpointed_world_journal},
        {"SQLite WAL metadata", sqlite_metadata_uses_wal_and_migrations},
        {"IANA timezone DST", named_timezone_applies_dst_transitions},
        {"system clock sync", system_clock_sync_overrides_seeded_and_scaled_time},
        {"town configuration", town_configuration_is_loaded_and_validated},
        {"mod sandbox denies escapes", mod_sandbox_denies_escapes},
        {"mod memory cap holds", mod_memory_cap_holds},
        {"mod instruction budget holds", mod_instruction_budget_holds},
        {"mod errors quarantine", mod_errors_quarantine},
        {"mod load order is deterministic", mod_load_order_is_deterministic},
        {"mod manifest is validated", mod_manifest_is_validated},
        {"mod randomness is deterministic", mod_randomness_is_deterministic},
        {"one bad mod does not block the rest", mod_one_bad_mod_does_not_block_the_rest},
        {"mod strings transcode", mod_strings_transcode_to_the_game_codepage},
        {"mod calendar date helpers", mod_calendar_date_helpers_are_correct},
        {"mod holidays resolve per year", mod_holidays_resolve_per_year},
        {"mod holiday specs are validated", mod_holiday_specs_are_validated},
        {"mod holiday windows are bounded", mod_holiday_windows_are_bounded},
        {"mod calendar api registers holidays", mod_calendar_api_registers_holidays},
        {"mod calendar api rejects bad specs", mod_calendar_api_rejects_bad_specs},
        {"mod calendar holidays are namespaced", mod_calendar_holidays_are_namespaced},
        {"mod calendar queries read the town", mod_calendar_queries_read_the_town},
        {"mod calendar effects route through runtime", mod_calendar_effects_route_through_the_runtime},
        {"mod calendar store round trips", mod_calendar_store_round_trips},
        {"mod registration is load time only", mod_registration_is_load_time_only},
        {"mod music defines custom songs", mod_music_defines_custom_songs},
        {"mod music respects the music_box limit", mod_music_respects_the_music_box_limit},
        {"mod music slots are never reused", mod_music_slots_are_never_reused},
        {"mod music grant routes through runtime", mod_music_grant_routes_through_the_runtime},
        {"mod summaries report what is running", mod_summaries_report_what_is_running},
        {"mod summaries are empty without mods", mod_summaries_are_empty_without_mods},
        {"mod calendar drives a real town", mod_calendar_drives_a_real_town},
        {"mod calendar replicates", mod_calendar_replicates},
        {"asset delivery messages round trip", asset_delivery_messages_round_trip},
        {"asset pack store serves content", asset_pack_store_serves_content},
        {"asset delivery serves a real client", asset_delivery_serves_a_real_client},
        {"client network INI", client_network_ini_is_loaded_and_validated},
        {"packet round trip and corruption", packet_round_trip_and_corruption},
        {"protocol rejects truncation/nonfinite", protocol_rejects_truncated_and_nonfinite},
        {"snapshot round trip", snapshot_round_trip},
        {"resident roster replication", resident_roster_replication},
        {"tile delta actor and cause", tile_delta_carries_actor_and_cause},
        {"tile change queue and baseline serial", tile_changes_queue_separately_from_baselines},
        {"stable entity IDs", entity_ids_are_stable_and_not_reused},
        {"sessions and reconnect", sessions_negotiate_capacity_and_reconnect},
        {"snapshot interpolation", interpolation_orders_and_extrapolates},
        {"selective reliability", selective_reliability_tracks_ack_windows},
        {"UDP eight-client handshake", udp_eight_client_handshake_smoke},
        {"multiplayer player queries", multiplayer_player_queries_are_scoped},
        {"client-authoritative movement at 200ms", movement_is_client_authoritative_under_latency},
        {"atomic world transactions", world_transactions_are_atomic_idempotent_and_conserved},
        {"shop prices match the original", shop_prices_match_the_original_tables},
        {"selling pays the generated price", selling_pays_the_generated_price},
        {"selling a selection is atomic", selling_a_selection_is_atomic_and_caps_the_wallet},
        {"shop shelf is the whole shelf", shop_shelf_is_the_whole_shelf},
        {"shop tier is earned and server owned", shop_tier_is_earned_and_server_owned},
        {"turnip market is town state", turnip_market_is_town_state},
        {"shop shelf replicates town-wide", shop_shelf_replicates_town_wide},
        {"museum collection replicates", museum_collection_replicates_and_refuses_duplicates},
        {"NPC state replicates", npc_state_replicates_between_baselines},
        {"economy and escrow trade", economy_and_trade_prevent_value_duplication},
        {"server-authoritative mail and banking", mail_and_banking_are_server_authoritative},
        {"operator gifts survive a restart", operator_gifts_survive_a_restart},
        {"NPC conversation leases", npc_leases_scope_conversations_and_disconnects},
        {"zone handoff and housing", zone_handoffs_and_four_resident_housing_are_safe},
        {"gyroid replication and trade", gyroids_replicate_and_transact},
        {"persistence crash recovery", persistence_recovers_checkpoints_journal_and_gci},
        {"semantic GCI round trip", gci_semantic_conversion_preserves_native_save},
        {"clock jobs and replication", clock_jobs_and_replication_survive_empty_time},
        {"real runtime eight-bot smoke", real_runtime_serves_eight_moving_bots},
        {"production client loopback", production_clients_connect_move_and_render_each_other},
        {"appearance survives a zone round trip", appearance_survives_a_zone_round_trip},
        {"canonical town bootstrap restart", canonical_town_bootstrap_survives_clients_and_restart},
        {"blank town bootstrap repairable", blank_town_bootstrap_is_repairable_after_restart},
        {"island authoritative shared zone", island_is_an_authoritative_shared_zone},
        {"island cabin shared room", island_cabin_is_a_shared_room},
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
