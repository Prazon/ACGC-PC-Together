#include "acnet/c_api.h"

#include "acnet/client.hpp"
#include "acnet/entity_registry.hpp"

#include <algorithm>
#include <exception>
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

extern "C" int acnet_client_submit_town_bootstrap(const uint8_t town_name[8],
                                                    uint16_t land_id,
                                                    const AcNetPlayerAppearance* appearance,
                                                    const AcNetTownBootstrapTile* tiles,
                                                    size_t tile_count) {
    try {
        if (!client || town_name == nullptr || appearance == nullptr || tiles == nullptr ||
            tile_count != acnet::kTownBootstrapTileCount) return 0;
        acnet::TownBootstrap bootstrap;
        bootstrap.town_seed = client->town_seed();
        bootstrap.land_id = land_id;
        std::copy(town_name, town_name + bootstrap.town_name.size(), bootstrap.town_name.begin());
        std::copy(appearance->name, appearance->name + bootstrap.appearance.name.size(),
                  bootstrap.appearance.name.begin());
        bootstrap.appearance.gender = appearance->gender;
        bootstrap.appearance.face = appearance->face;
        bootstrap.appearance.clothing = appearance->clothing;
        bootstrap.appearance.equipped_item = appearance->equipped_item;
        bootstrap.tiles.reserve(tile_count);
        for (std::size_t i = 0; i < tile_count; ++i)
            bootstrap.tiles.push_back({tiles[i].item, tiles[i].buried != 0});
        return client->submit_town_bootstrap(std::move(bootstrap),
                                             acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
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
                                                   uint64_t recipient) {
    try {
        if (!client || operation_type > static_cast<std::uint8_t>(acnet::EconomyOpType::AttachMail)) return 0;
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
        return request.idempotency.valid() &&
               client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_take_economy_result(AcNetEconomyResult* output) {
    try {
        if (!client || output == nullptr) return 0;
        const auto value = client->take_economy_result();
        if (!value.has_value()) return 0;
        *output = {static_cast<std::uint16_t>(value->code), value->inventory_revision,
                   value->auxiliary_revision, value->balance, value->debt, value->bells,
                   value->item, value->inventory_slot, value->mail_id,
                   static_cast<std::uint8_t>(value->replayed)};
        return 1;
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
                                               uint32_t expected_inventory_revision,
                                               uint8_t tool_slot,
                                               uint64_t idempotency_high,
                                               uint64_t idempotency_low) {
    try {
        if (!client || kind > static_cast<uint8_t>(acnet::EncounterKind::Insect)) return 0;
        acnet::EncounterRequest request;
        request.kind = static_cast<acnet::EncounterKind>(kind);
        request.expected_inventory_revision = expected_inventory_revision;
        request.tool_slot = tool_slot;
        request.idempotency = {idempotency_high, idempotency_low};
        return client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_request_encounter_auto(uint8_t kind,
                                                      uint32_t expected_inventory_revision,
                                                      uint8_t tool_slot) {
    try {
        const acnet::IdempotencyKey key = random_idempotency();
        return key.valid() && acnet_client_request_encounter(kind, expected_inventory_revision, tool_slot,
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
        request.address = {x, z, layer};
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

extern "C" int acnet_client_request_zone_transfer(uint32_t door_id) {
    try {
        if (!client) return 0;
        acnet::ZoneTransferRequest request;
        request.door_id = door_id;
        return client->request(request, acnet::client_monotonic_milliseconds(), last_error) ? 1 : 0;
    } catch (...) { capture_exception(); return 0; }
}

extern "C" int acnet_client_zone_ready(uint64_t token_high, uint64_t token_low) {
    try {
        if (!client) return 0;
        acnet::ZoneReadyRequest request;
        request.token = {token_high, token_low};
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
