#include "acnet/c_api.h"

#include "acnet/client.hpp"
#include "acnet/entity_registry.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <string>

namespace {

std::unique_ptr<acnet::ClientRuntime> client;
acnet::EntityRegistry actor_entities(1);
std::string last_error;
std::uint64_t client_account = 0;

AcNetClientStatus translate_state(acnet::ClientConnectionState state) {
    switch (state) {
        case acnet::ClientConnectionState::Offline: return ACNET_OFFLINE;
        case acnet::ClientConnectionState::Connecting: return ACNET_CONNECTING;
        case acnet::ClientConnectionState::Connected: return ACNET_CONNECTED;
        case acnet::ClientConnectionState::Reconnecting: return ACNET_RECONNECTING;
        case acnet::ClientConnectionState::Rejected: return ACNET_REJECTED;
        case acnet::ClientConnectionState::Failed: return ACNET_FAILED;
    }
    return ACNET_FAILED;
}

acnet::Transform from_c(const AcNetTransform& source) {
    acnet::Transform result;
    result.position = {source.x, source.y, source.z};
    result.velocity = {source.velocity_x, source.velocity_y, source.velocity_z};
    result.yaw = source.yaw;
    result.action = source.action;
    return result;
}

AcNetTransform to_c(const acnet::Transform& source) {
    return {source.position.x,
            source.position.y,
            source.position.z,
            source.velocity.x,
            source.velocity.y,
            source.velocity.z,
            source.yaw,
            source.action};
}

void appearance_from_c(const AcNetPlayerAppearance& source,
                       acnet::PlayerAppearance& appearance,
                       acnet::CustomPattern& pattern) {
    std::copy(std::begin(source.name), std::end(source.name), appearance.name.begin());
    appearance.gender = source.gender;
    appearance.face = source.face;
    appearance.clothing = source.clothing;
    appearance.equipped_item = source.equipped_item;
    appearance.clothing_index = source.clothing_index;
    appearance.revision = source.appearance_revision;
    pattern.present = source.pattern_present != 0;
    pattern.palette = source.pattern_palette;
    std::copy(std::begin(source.pattern_texture), std::end(source.pattern_texture), pattern.texture.begin());
}

void capture_exception() {
    try {
        throw;
    } catch (const std::exception& exception) {
        last_error = exception.what();
    } catch (...) {
        last_error = "unknown netcode exception";
    }
}

acnet::IdempotencyKey random_idempotency() {
    acnet::IdempotencyKey key;
    if (!acnet::secure_random(reinterpret_cast<std::uint8_t*>(&key), sizeof(key))) return {};
    if (!key.valid()) key.low = 1;
    return key;
}

} // namespace

extern "C" int acnet_client_start(const char* host,
                                   uint16_t port,
                                   uint64_t town_id,
                                   uint64_t account_id,
                                   uint64_t build_id,
                                   const char* invite_key) {
    try {
        if (client || host == nullptr || host[0] == '\0') {
            last_error = "net client is already running or host is empty";
            return 0;
        }
        acnet::ClientConfig config;
        config.server_host = host;
        config.server_port = port;
        config.town = town_id;
        config.account = account_id;
        config.build_id = build_id;
        if (invite_key != nullptr) config.invite_key = invite_key;
        auto candidate = std::make_unique<acnet::ClientRuntime>(config);
        if (!candidate->start(acnet::client_monotonic_milliseconds(), last_error)) return 0;
        client_account = account_id;
        client = std::move(candidate);
        return 1;
    } catch (...) {
        capture_exception();
        return 0;
    }
}

extern "C" void acnet_client_stop(void) {
    try {
        if (client) client->stop(acnet::client_monotonic_milliseconds());
        client.reset();
        client_account = 0;
    } catch (...) {
        capture_exception();
    }
}

extern "C" int acnet_client_poll(void) {
    try {
        return !client || client->poll(acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) {
        capture_exception();
        return 0;
    }
}

extern "C" int acnet_client_frame(int16_t stick_x,
                                   int16_t stick_y,
                                   uint16_t buttons,
                                   uint16_t action,
                                   AcNetTransform* local_transform) {
    try {
        if (!client || local_transform == nullptr) return 1;
        acnet::Transform corrected;
        bool has_correction = false;
        if (!client->frame(acnet::client_monotonic_milliseconds(),
                           stick_x,
                           stick_y,
                           buttons,
                           action,
                           from_c(*local_transform),
                           corrected,
                           has_correction,
                           last_error)) return 0;
        if (has_correction) *local_transform = to_c(corrected);
        return 1;
    } catch (...) {
        capture_exception();
        return 0;
    }
}

extern "C" size_t acnet_client_remote_players(AcNetRemotePlayer* output, size_t capacity) {
    try {
        if (!client) return 0;
        const auto remotes = client->remote_players();
        const std::size_t count = std::min<std::size_t>(capacity, remotes.size());
        if (output != nullptr) {
            for (std::size_t i = 0; i < count; ++i) {
                output[i].entity_id = remotes[i].entity;
                output[i].account_id = remotes[i].account;
                output[i].zone_id = remotes[i].zone;
                output[i].transform = to_c(remotes[i].transform);
                std::copy(remotes[i].appearance.name.begin(), remotes[i].appearance.name.end(), output[i].name);
                output[i].gender = remotes[i].appearance.gender;
                output[i].face = remotes[i].appearance.face;
                output[i].clothing = remotes[i].appearance.clothing;
                output[i].equipped_item = remotes[i].appearance.equipped_item;
                output[i].clothing_index = remotes[i].appearance.clothing_index;
                output[i].appearance_revision = remotes[i].appearance.revision;
                output[i].pattern_present = remotes[i].pattern.present ? 1 : 0;
                output[i].pattern_palette = remotes[i].pattern.palette;
                std::copy(remotes[i].pattern.texture.begin(), remotes[i].pattern.texture.end(),
                          output[i].pattern_texture);
                output[i].transition_phase = static_cast<std::uint8_t>(remotes[i].transition_phase);
                output[i].transition_door = remotes[i].transition_door;
                output[i].transition_expires_tick = remotes[i].transition_expires_tick;
            }
        }
        return output == nullptr ? remotes.size() : count;
    } catch (...) {
        capture_exception();
        return 0;
    }
}

extern "C" size_t acnet_client_baseline_tiles(AcNetTileState* output, size_t capacity) {
    try {
        if (!client || client->baseline() == nullptr) return 0;
        const auto& tiles = client->baseline()->tiles;
        const std::size_t count = std::min<std::size_t>(capacity, tiles.size());
        if (output != nullptr) {
            for (std::size_t i = 0; i < count; ++i) {
                output[i] = {tiles[i].first.zone, tiles[i].first.x, tiles[i].first.z,
                             tiles[i].second.revision, tiles[i].second.item, tiles[i].second.condition,
                             static_cast<std::uint8_t>(tiles[i].second.terrain),
                             static_cast<std::uint8_t>(tiles[i].second.buried),
                             static_cast<std::uint8_t>(tiles[i].second.placed_furniture)};
            }
        }
        return output == nullptr ? tiles.size() : count;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_tile(uint32_t zone_id, int16_t x, int16_t z, AcNetTileState* output) {
    try {
        if (!client || client->baseline() == nullptr || output == nullptr) return 0;
        const auto& tiles = client->baseline()->tiles;
        const auto found = std::find_if(tiles.begin(), tiles.end(), [&](const auto& entry) {
            return entry.first.zone == zone_id && entry.first.x == x && entry.first.z == z;
        });
        if (found == tiles.end()) return 0;
        *output = {found->first.zone, found->first.x, found->first.z, found->second.revision,
                   found->second.item, found->second.condition,
                   static_cast<std::uint8_t>(found->second.terrain),
                   static_cast<std::uint8_t>(found->second.buried),
                   static_cast<std::uint8_t>(found->second.placed_furniture)};
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" uint32_t acnet_client_baseline_revision(void) {
    return client ? client->baseline_revision() : 0;
}

extern "C" uint32_t acnet_client_baseline_zone(void) {
    return client && client->baseline() != nullptr ? client->baseline()->zone : 0;
}

extern "C" uint8_t acnet_client_house_light_mask(void) {
    return client ? client->house_light_mask() : 0;
}

extern "C" int acnet_client_house(AcNetHouseState* output) {
    try {
        if (!client || client->baseline() == nullptr || !client->baseline()->has_house || output == nullptr) return 0;
        const acnet::HouseState& house = client->baseline()->house;
        output->house_id = house.house_id;
        output->owner_account_id = house.owner;
        output->zone_id = house.zone;
        output->revision = house.revision;
        output->original_slot = house.original_slot;
        output->upgrade_level = house.upgrade_level;
        output->initialized = house.initialized ? 1 : 0;
        output->main_light_on = house.main_light_on ? 1 : 0;
        output->basement_light_on = house.basement_light_on ? 1 : 0;
        for (std::size_t i = 0; i < house.music_tracks.size(); ++i) output->music_tracks[i] = house.music_tracks[i];
        for (std::size_t i = 0; i < house.furniture_switches.size(); ++i)
            output->furniture_switches[i] = house.furniture_switches[i];
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" size_t acnet_client_house_furniture(AcNetHouseFurniture* output, size_t capacity) {
    try {
        if (!client || client->baseline() == nullptr || !client->baseline()->has_house) return 0;
        const auto& furniture = client->baseline()->house.furniture;
        if (output == nullptr) return furniture.size();
        std::vector<std::pair<acnet::FurnitureAddress, acnet::ItemSlot>> ordered(furniture.begin(), furniture.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            if (left.first.floor != right.first.floor) return left.first.floor < right.first.floor;
            if (left.first.layer != right.first.layer) return left.first.layer < right.first.layer;
            if (left.first.z != right.first.z) return left.first.z < right.first.z;
            return left.first.x < right.first.x;
        });
        const std::size_t count = std::min(capacity, ordered.size());
        for (std::size_t i = 0; i < count; ++i) {
            output[i] = {ordered[i].first.x, ordered[i].first.z, ordered[i].first.floor,
                         ordered[i].first.layer, ordered[i].second.item, ordered[i].second.condition};
        }
        return count;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" size_t acnet_client_inventory(AcNetItemSlot* output, size_t capacity) {
    try {
        if (!client || client->baseline() == nullptr) return 0;
        const auto& slots = client->baseline()->inventory.slots;
        const std::size_t count = std::min<std::size_t>(capacity, slots.size());
        if (output != nullptr) {
            for (std::size_t i = 0; i < count; ++i) output[i] = {slots[i].item, slots[i].condition};
        }
        return output == nullptr ? slots.size() : count;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" uint32_t acnet_client_inventory_revision(void) {
    return client && client->baseline() != nullptr ? client->baseline()->inventory.revision : 0;
}

extern "C" uint32_t acnet_client_bells(void) {
    return client && client->baseline() != nullptr ? client->baseline()->inventory.bells : 0;
}

extern "C" uint64_t acnet_client_bank_balance(void) {
    return client && client->baseline() != nullptr ? client->baseline()->ledger.bank_balance : 0;
}

extern "C" uint64_t acnet_client_debt(void) {
    return client && client->baseline() != nullptr ? client->baseline()->ledger.debt : 0;
}

extern "C" int64_t acnet_client_town_time(void) {
    return client ? client->estimated_town_time(acnet::client_monotonic_milliseconds()) : 0;
}

extern "C" uint8_t acnet_client_weather(void) {
    return client && client->baseline() != nullptr ? client->baseline()->weather : 0;
}

extern "C" uint8_t acnet_client_weather_intensity(void) {
    return client && client->baseline() != nullptr ? client->baseline()->weather_intensity : 0;
}

extern "C" uint32_t acnet_client_town_seed(void) {
    return client ? client->town_seed() : 0;
}

extern "C" uint16_t acnet_client_town_land_id(void) {
    return client ? client->town_land_id() : 0;
}

extern "C" uint8_t acnet_client_resident_slot(void) {
    return client ? client->resident_slot() : 0xFF;
}

extern "C" int acnet_client_town_initialized(void) {
    return client && client->town_initialized() ? 1 : 0;
}

extern "C" size_t acnet_client_town_name(uint8_t* output, size_t capacity) {
    if (!client) return 0;
    const auto& name = client->town_name();
    const std::size_t count = std::min<std::size_t>(capacity, name.size());
    if (output != nullptr) std::copy(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(count), output);
    return output == nullptr ? name.size() : count;
}

extern "C" int acnet_client_town_population(uint8_t* population, uint8_t* capacity) {
    if (!client) return 0;
    const std::uint8_t current = client->town_population();
    if (current == 0) return 0; /* not reported */
    if (population != nullptr) *population = current;
    if (capacity != nullptr) *capacity = client->town_capacity();
    return 1;
}

extern "C" int acnet_client_submit_town_bootstrap(const uint8_t town_name[8],
                                                    uint16_t land_id,
                                                    const AcNetPlayerAppearance* appearance,
                                                    const AcNetTownBootstrapTile* tiles,
                                                    size_t tile_count,
                                                    const AcNetTownBootstrapTile* island_tiles,
                                                    size_t island_tile_count,
                                                    uint8_t island_block_x0,
                                                    uint8_t island_block_x1) {
    try {
        if (!client || town_name == nullptr || appearance == nullptr || tiles == nullptr ||
            tile_count != acnet::kTownBootstrapTileCount) return 0;
        /* The island section is optional: a client that could not read the acre
         * layout yet sends none and a later login supplies it. */
        const bool has_island = island_tiles != nullptr &&
                                island_tile_count == acnet::kIslandBootstrapTileCount &&
                                island_block_x0 < acnet::kFieldBlockXCount &&
                                island_block_x1 < acnet::kFieldBlockXCount &&
                                island_block_x0 < island_block_x1;
        acnet::TownBootstrap bootstrap;
        bootstrap.town_seed = client->town_seed();
        bootstrap.land_id = land_id;
        std::copy(town_name, town_name + bootstrap.town_name.size(), bootstrap.town_name.begin());
        appearance_from_c(*appearance, bootstrap.appearance, bootstrap.pattern);
        bootstrap.tiles.reserve(tile_count);
        for (std::size_t i = 0; i < tile_count; ++i)
            bootstrap.tiles.push_back({tiles[i].item, tiles[i].buried != 0});
        if (has_island) {
            bootstrap.island_block_x = {island_block_x0, island_block_x1};
            bootstrap.island_tiles.reserve(island_tile_count);
            for (std::size_t i = 0; i < island_tile_count; ++i)
                bootstrap.island_tiles.push_back({island_tiles[i].item, island_tiles[i].buried != 0});
        }
        return client->submit_town_bootstrap(std::move(bootstrap),
                                             acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_update_appearance(const AcNetPlayerAppearance* appearance) {
    try {
        if (!client || appearance == nullptr) return 0;
        acnet::AppearanceUpdate update;
        appearance_from_c(*appearance, update.appearance, update.pattern);
        /* Revisions are assigned by the authority, never accepted from the
         * presentation client. */
        update.appearance.revision = 0;
        return client->update_appearance(std::move(update), acnet::client_monotonic_milliseconds(),
                                         last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_town_bootstrap_result(uint16_t* result_code,
                                                         uint32_t* revision,
                                                         uint8_t* initialized) {
    try {
        if (!client || result_code == nullptr || revision == nullptr || initialized == nullptr) return 0;
        const auto value = client->take_town_bootstrap_result();
        if (!value.has_value()) return 0;
        *result_code = static_cast<std::uint16_t>(value->code);
        *revision = value->revision;
        *initialized = value->initialized ? 1 : 0;
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_world(uint8_t operation_type,
                                           uint32_t zone_id,
                                           int16_t x,
                                           int16_t z,
                                           uint32_t expected_tile_revision,
                                           uint32_t expected_inventory_revision,
                                           uint8_t inventory_slot,
                                           uint16_t expected_item,
                                           uint64_t idempotency_high,
                                           uint64_t idempotency_low) {
    try {
        if (!client || operation_type > static_cast<uint8_t>(acnet::WorldOpType::FillHole)) return 0;
        acnet::WorldOperation request;
        request.type = static_cast<acnet::WorldOpType>(operation_type);
        request.tile = {zone_id, x, z};
        request.expected_tile_revision = expected_tile_revision;
        request.expected_inventory_revision = expected_inventory_revision;
        request.inventory_slot = inventory_slot;
        request.expected_item = expected_item;
        request.idempotency = {idempotency_high, idempotency_low};
        return client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_world_result(AcNetWorldResult* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_world_result();
        if (!value.has_value()) return 0;
        output->result_code = static_cast<uint16_t>(value->code);
        output->idempotency_high = value->idempotency.high;
        output->idempotency_low = value->idempotency.low;
        output->zone_id = value->tile.zone;
        output->x = value->tile.x;
        output->z = value->tile.z;
        output->tile_revision = value->tile_revision;
        output->inventory_revision = value->inventory_revision;
        output->transferred_item = value->transferred_item;
        output->inventory_slot = value->inventory_slot;
        output->replayed = value->replayed ? 1 : 0;
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_world_auto(uint8_t operation_type,
                                                 uint32_t zone_id,
                                                 int16_t x,
                                                 int16_t z,
                                                 uint32_t expected_tile_revision,
                                                 uint32_t expected_inventory_revision,
                                                 uint8_t inventory_slot,
                                                 uint16_t expected_item) {
    try {
        const acnet::IdempotencyKey key = random_idempotency();
        if (!key.valid()) return 0;
        return acnet_client_request_world(operation_type, zone_id, x, z, expected_tile_revision,
                                          expected_inventory_revision, inventory_slot, expected_item,
                                          key.high, key.low);
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_world_with_tool_auto(uint8_t operation_type,
                                                            uint32_t zone_id,
                                                            int16_t x,
                                                            int16_t z,
                                                            uint32_t expected_tile_revision,
                                                            uint32_t expected_inventory_revision,
                                                            uint8_t inventory_slot,
                                                            uint8_t tool_slot,
                                                            uint16_t expected_item) {
    try {
        if (!client || operation_type > static_cast<uint8_t>(acnet::WorldOpType::FillHole)) return 0;
        acnet::WorldOperation request;
        request.type = static_cast<acnet::WorldOpType>(operation_type);
        request.idempotency = random_idempotency();
        request.tile = {zone_id, x, z};
        request.expected_tile_revision = expected_tile_revision;
        request.expected_inventory_revision = expected_inventory_revision;
        request.inventory_slot = inventory_slot;
        request.tool_slot = tool_slot;
        request.expected_item = expected_item;
        return request.idempotency.valid() &&
               client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_economy_auto(uint8_t operation_type,
                                                   uint32_t expected_inventory_revision,
                                                   uint32_t expected_aux_revision,
                                                   uint32_t shop_index,
                                                   uint8_t inventory_slot,
                                                   uint16_t expected_item,
                                                   uint64_t amount,
                                                   uint64_t recipient,
                                                   uint64_t mail_id) {
    try {
        if (!client || operation_type > acnet::kMaximumClientEconomyOp) return 0;
        acnet::EconomyRequest request;
        request.type = static_cast<acnet::EconomyOpType>(operation_type);
        request.idempotency = random_idempotency();
        request.expected_inventory_revision = expected_inventory_revision;
        request.expected_aux_revision = expected_aux_revision;
        request.shop_index = shop_index;
        request.inventory_slot = inventory_slot;
        request.expected_item = expected_item;
        request.amount = amount;
        request.recipient = recipient;
        request.mail_id = mail_id;
        return request.idempotency.valid() &&
               client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_economy_result(AcNetEconomyResult* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_economy_result();
        if (!value.has_value()) return 0;
        *output = {static_cast<std::uint16_t>(value->code), static_cast<std::uint8_t>(value->type),
                   value->inventory_revision,
                   value->auxiliary_revision, value->balance, value->debt, value->bells,
                   value->item, value->inventory_slot, value->mail_id,
                   static_cast<std::uint8_t>(value->replayed)};
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" uint32_t acnet_client_bank_revision(void) {
    try {
        return client == nullptr || client->baseline() == nullptr ? 0 : client->baseline()->ledger.revision;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" uint32_t acnet_client_mailbox_revision(void) {
    try {
        return client == nullptr || client->baseline() == nullptr ? 0 : client->mailbox().revision;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" size_t acnet_client_mail(AcNetMailRecord* output, size_t capacity) {
    try {
        if (!client || client->baseline() == nullptr || (capacity != 0 && output == nullptr)) return 0;
        const std::vector<acnet::MailRecord>& letters = client->mail();
        const std::size_t count = letters.size() < capacity ? letters.size() : capacity;
        for (std::size_t i = 0; i < count; ++i) {
            const acnet::MailContent& content = letters[i].content;
            output[i].id = letters[i].id;
            output[i].sender = letters[i].sender;
            output[i].recipient = letters[i].recipient;
            output[i].attachment = letters[i].attachment;
            output[i].revision = letters[i].revision;
            output[i].location = static_cast<std::uint8_t>(letters[i].location);
            output[i].font = content.font;
            output[i].mail_type = content.mail_type;
            output[i].paper_type = content.paper_type;
            output[i].header_back_start = content.header_back_start;
            std::memcpy(output[i].sender_name, content.sender_name.data(), content.sender_name.size());
            std::memcpy(output[i].header, content.header.data(), content.header.size());
            std::memcpy(output[i].body, content.body.data(), content.body.size());
            std::memcpy(output[i].footer, content.footer.data(), content.footer.size());
        }
        return count;
    } catch (...) { capture_exception(); return 0; }
}

namespace {

int request_mail_operation(acnet::EconomyOpType type, uint64_t mail_id) {
    if (!client || client->baseline() == nullptr || mail_id == 0) return 0;
    return acnet_client_request_economy_auto(static_cast<std::uint8_t>(type),
                                             client->baseline()->inventory.revision,
                                             client->mailbox().revision, 0, 0, 0, 0, 0, mail_id);
}

} // namespace

extern "C" int acnet_client_take_mail(uint64_t mail_id) {
    try {
        return request_mail_operation(acnet::EconomyOpType::TakeMail, mail_id);
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_claim_mail(uint64_t mail_id) {
    try {
        return request_mail_operation(acnet::EconomyOpType::ClaimMail, mail_id);
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_discard_mail(uint64_t mail_id) {
    try {
        return request_mail_operation(acnet::EconomyOpType::DiscardMail, mail_id);
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_trade(uint8_t action,
                                            uint64_t trade_id,
                                            uint64_t other_account,
                                            uint32_t expected_trade_revision,
                                            const uint8_t* slots,
                                            size_t slot_count) {
    try {
        if (!client || action > static_cast<std::uint8_t>(acnet::TradeAction::Cancel) ||
            slot_count > acnet::kInventorySlots || (slot_count != 0 && slots == nullptr)) return 0;
        acnet::TradeRequest request;
        request.action = static_cast<acnet::TradeAction>(action);
        request.trade_id = trade_id;
        request.other_account = other_account;
        request.expected_trade_revision = expected_trade_revision;
        if (slot_count != 0) request.slots.assign(slots, slots + slot_count);
        return client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_trade_result(AcNetTradeResult* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_trade_result();
        if (!value.has_value()) return 0;
        *output = {static_cast<std::uint16_t>(value->code), value->trade_id, value->trade_revision,
                   value->inventory_revision, static_cast<std::uint8_t>(value->finalized)};
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_conversation(uint8_t action,
                                                   uint64_t npc_entity,
                                                   uint32_t lease_id,
                                                   uint16_t choice) {
    try {
        if (!client || action > static_cast<std::uint8_t>(acnet::ConversationAction::End)) return 0;
        acnet::ConversationRequest request;
        request.action = static_cast<acnet::ConversationAction>(action);
        request.npc = npc_entity;
        request.lease_id = lease_id;
        request.choice = choice;
        return client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_conversation_result(AcNetConversationResult* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_conversation_result();
        if (!value.has_value()) return 0;
        *output = {static_cast<std::uint16_t>(value->code), value->npc, value->lease_id,
                   value->dialogue_node, value->revision, value->expires_tick,
                   static_cast<std::uint8_t>(value->completed)};
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_encounter(uint8_t kind,
                                               uint16_t species,
                                               uint32_t expected_inventory_revision,
                                               uint8_t tool_slot,
                                               uint64_t idempotency_high,
                                               uint64_t idempotency_low) {
    try {
        if (!client || kind > static_cast<uint8_t>(acnet::EncounterKind::Insect)) return 0;
        acnet::EncounterRequest request;
        request.kind = static_cast<acnet::EncounterKind>(kind);
        request.species = species;
        request.expected_inventory_revision = expected_inventory_revision;
        request.tool_slot = tool_slot;
        request.idempotency = {idempotency_high, idempotency_low};
        return client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_encounter_auto(uint8_t kind,
                                                      uint16_t species,
                                                      uint32_t expected_inventory_revision,
                                                      uint8_t tool_slot) {
    try {
        const acnet::IdempotencyKey key = random_idempotency();
        return key.valid() && acnet_client_request_encounter(kind, species, expected_inventory_revision, tool_slot,
                                                              key.high, key.low);
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_encounter_result(AcNetEncounterResult* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_encounter_result();
        if (!value.has_value()) return 0;
        output->result_code = static_cast<uint16_t>(value->code);
        output->inventory_revision = value->inventory_revision;
        output->item = value->item;
        output->inventory_slot = value->inventory_slot;
        output->caught = value->caught ? 1 : 0;
        output->replayed = value->replayed ? 1 : 0;
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_furniture_auto(uint8_t operation_type,
                                                     uint64_t house_id,
                                                     uint8_t x,
                                                     uint8_t z,
                                                     uint8_t layer,
                                                     uint32_t expected_house_revision,
                                                     uint32_t expected_inventory_revision,
                                                     uint8_t inventory_slot,
                                                     uint16_t expected_item) {
    try {
        if (!client || operation_type > static_cast<std::uint8_t>(acnet::FurnitureOpType::Remove)) return 0;
        acnet::FurnitureOperation request;
        request.type = static_cast<acnet::FurnitureOpType>(operation_type);
        request.idempotency = random_idempotency();
        request.house_id = house_id;
        request.address = {x, z, 0, layer};
        request.expected_house_revision = expected_house_revision;
        request.expected_inventory_revision = expected_inventory_revision;
        request.inventory_slot = inventory_slot;
        request.expected_item = expected_item;
        return request.idempotency.valid() &&
               client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_furniture_result(AcNetFurnitureResult* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_furniture_result();
        if (!value.has_value()) return 0;
        *output = {static_cast<std::uint16_t>(value->code), value->house_id, value->house_revision,
                   value->inventory_revision, value->inventory_slot, value->item,
                   static_cast<std::uint8_t>(value->replayed)};
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_submit_house_update(uint64_t house_id,
                                                  uint32_t expected_house_revision,
                                                  uint8_t upgrade_level,
                                                  uint8_t main_light_on,
                                                  uint8_t basement_light_on,
                                                  const int16_t music_tracks[3],
                                                  const uint64_t furniture_switches[12],
                                                  const AcNetHouseFurniture* furniture,
                                                  size_t furniture_count) {
    try {
        if (!client || house_id == 0 || expected_house_revision == 0 ||
            upgrade_level > acnet::kMaximumHouseUpgradeLevel ||
            main_light_on > 1 || basement_light_on > 1 || music_tracks == nullptr || furniture_switches == nullptr ||
            furniture_count > acnet::kMaximumHouseFurniture || (furniture_count != 0 && furniture == nullptr)) return 0;
        acnet::HouseUpdate update;
        update.idempotency = random_idempotency();
        update.house_id = house_id;
        update.expected_house_revision = expected_house_revision;
        update.upgrade_level = upgrade_level;
        update.main_light_on = main_light_on != 0;
        update.basement_light_on = basement_light_on != 0;
        std::copy_n(music_tracks, update.music_tracks.size(), update.music_tracks.begin());
        std::copy_n(furniture_switches, update.furniture_switches.size(), update.furniture_switches.begin());
        for (std::size_t i = 0; i < furniture_count; ++i) {
            acnet::FurnitureAddress address{furniture[i].x, furniture[i].z, furniture[i].floor, furniture[i].layer};
            acnet::ItemSlot item{furniture[i].item, furniture[i].condition};
            if (address.x >= 16 || address.z >= 16 || address.floor >= acnet::kHouseFloorCount ||
                address.layer >= acnet::kHouseLayerCount || item.item == 0 ||
                !update.furniture.emplace(address, item).second) return 0;
        }
        return update.idempotency.valid() &&
               client->request(std::move(update), acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_house_update_result(AcNetHouseUpdateResult* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_house_update_result();
        if (!value.has_value()) return 0;
        *output = {static_cast<std::uint16_t>(value->code), value->house_id, value->house_revision,
                   static_cast<std::uint8_t>(value->replayed)};
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_zone_transfer(uint32_t door_id) {
    try {
        if (!client) return 0;
        acnet::ZoneTransferRequest request;
        request.door_id = door_id;
        return client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_zone_ready(uint64_t token_high,
                                        uint64_t token_low,
                                        const AcNetTransform* destination_transform) {
    try {
        if (!client || destination_transform == nullptr) return 0;
        acnet::ZoneReadyRequest request;
        request.token = {token_high, token_low};
        request.destination_transform = from_c(*destination_transform);
        return client->ready(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_transfer_offer(AcNetTransferOffer* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_transfer_offer();
        if (!value.has_value()) return 0;
        *output = {static_cast<std::uint16_t>(value->code), value->source_zone,
                   value->destination_zone, value->destination_position.x,
                   value->destination_position.y, value->destination_position.z,
                   value->token.high, value->token.low, value->expires_tick,
                   value->baseline_revision};
        return 1;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" AcNetClientStatus acnet_client_status(void) {
    return client ? translate_state(client->state()) : ACNET_OFFLINE;
}

extern "C" uint64_t acnet_client_account(void) {
    return client_account;
}

extern "C" uint64_t acnet_client_entity(void) {
    return client ? client->local_entity() : 0;
}

extern "C" uint32_t acnet_client_server_tick(void) {
    return client ? client->server_tick() : 0;
}

extern "C" const char* acnet_client_last_error(void) {
    return last_error.c_str();
}

extern "C" uint64_t acnet_actor_created(const void* actor, int16_t profile, int16_t scene) {
    try {
        if (actor == nullptr) return 0;
        const auto id = actor_entities.add(reinterpret_cast<std::uintptr_t>(actor),
                                           static_cast<std::uint32_t>(static_cast<std::uint16_t>(profile)),
                                           static_cast<std::uint32_t>(static_cast<std::uint16_t>(scene)),
                                           1);
        return id;
    } catch (...) {
        capture_exception();
        return 0;
    }
}

extern "C" void acnet_actor_destroyed(const void* actor) {
    try {
        if (actor != nullptr) actor_entities.remove_by_key(reinterpret_cast<std::uintptr_t>(actor));
    } catch (...) {
        capture_exception();
    }
}

extern "C" uint64_t acnet_actor_entity(const void* actor) {
    if (actor == nullptr) return 0;
    const acnet::EntityRecord* record = actor_entities.by_key(reinterpret_cast<std::uintptr_t>(actor));
    return record == nullptr ? 0 : record->id;
}

extern "C" void acnet_scene_loaded(int16_t scene) {
    (void)scene;
}
