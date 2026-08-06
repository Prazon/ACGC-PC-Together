#include "acnet/messages.hpp"

#include "acnet/fragmentation.hpp"
#include "acnet/protocol.hpp"

#include <limits>

namespace acnet {
namespace {

bool idempotency(ByteWriter& writer, const IdempotencyKey& key) {
    return writer.u64(key.high) && writer.u64(key.low);
}

bool idempotency(ByteReader& reader, IdempotencyKey& key) {
    return reader.u64(key.high) && reader.u64(key.low);
}

bool tile(ByteWriter& writer, const TileAddress& value) {
    return writer.u32(value.zone) && writer.i16(value.x) && writer.i16(value.z);
}

bool tile(ByteReader& reader, TileAddress& value) {
    return reader.u32(value.zone) && reader.i16(value.x) && reader.i16(value.z);
}

bool vec3(ByteWriter& writer, const Vec3& value) {
    return writer.f32(value.x) && writer.f32(value.y) && writer.f32(value.z);
}

bool vec3(ByteReader& reader, Vec3& value) {
    return reader.f32(value.x) && reader.f32(value.y) && reader.f32(value.z) && finite(value);
}

bool valid_result(std::uint16_t value) {
    return value <= static_cast<std::uint16_t>(ResultCode::InternalError);
}

bool result(ByteWriter& writer, ResultCode value) {
    return writer.u16(static_cast<std::uint16_t>(value));
}

bool result(ByteReader& reader, ResultCode& value) {
    std::uint16_t raw;
    if (!reader.u16(raw) || !valid_result(raw)) return false;
    value = static_cast<ResultCode>(raw);
    return true;
}

bool appearance(ByteWriter& writer, const PlayerAppearance& value) {
    return value.gender <= 2 && value.face < 8 &&
           writer.bytes(value.name.data(), value.name.size()) && writer.u8(value.gender) &&
           writer.u8(value.face) && writer.u16(value.clothing) && writer.u16(value.equipped_item);
}

bool appearance(ByteReader& reader, PlayerAppearance& value) {
    return reader.bytes(value.name.data(), value.name.size()) && reader.u8(value.gender) &&
           reader.u8(value.face) && reader.u16(value.clothing) && reader.u16(value.equipped_item) &&
           value.gender <= 2 && value.face < 8;
}

} // namespace

bool encode(const TownBootstrap& value, std::vector<std::uint8_t>& output) {
    if (value.town_seed == 0 || (value.land_id & 0xFF00U) != 0x3000U ||
        value.tiles.size() != kTownBootstrapTileCount) return false;
    ByteWriter writer(kMaximumTransferBytes);
    if (!writer.u32(value.town_seed) || !writer.u16(value.land_id) ||
        !writer.bytes(value.town_name.data(), value.town_name.size()) || !appearance(writer, value.appearance) ||
        !writer.u32(static_cast<std::uint32_t>(value.tiles.size()))) return false;
    for (const TownBootstrapTile& tile_value : value.tiles) {
        if (!writer.u16(tile_value.item) || !writer.u8(tile_value.buried ? 1 : 0)) return false;
    }
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, TownBootstrap& value) {
    if (input.size() > kMaximumTransferBytes) return false;
    ByteReader reader(input);
    std::uint32_t count;
    if (!reader.u32(value.town_seed) || !reader.u16(value.land_id) ||
        !reader.bytes(value.town_name.data(), value.town_name.size()) || !appearance(reader, value.appearance) ||
        !reader.u32(count) || value.town_seed == 0 || (value.land_id & 0xFF00U) != 0x3000U ||
        count != kTownBootstrapTileCount) return false;
    value.tiles.clear();
    value.tiles.resize(count);
    for (TownBootstrapTile& tile_value : value.tiles) {
        std::uint8_t buried;
        if (!reader.u16(tile_value.item) || !reader.u8(buried) || buried > 1) return false;
        tile_value.buried = buried != 0;
    }
    return reader.finished();
}

bool encode(const TownBootstrapResult& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!result(writer, value.code) || !writer.u32(value.revision) ||
        !writer.u8(value.initialized ? 1 : 0)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, TownBootstrapResult& value) {
    ByteReader reader(input);
    std::uint8_t initialized;
    if (!result(reader, value.code) || !reader.u32(value.revision) || !reader.u8(initialized) ||
        initialized > 1 || !reader.finished()) return false;
    value.initialized = initialized != 0;
    return true;
}

bool encode(const WorldOperation& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (static_cast<std::uint8_t>(value.type) > static_cast<std::uint8_t>(WorldOpType::FillHole) ||
        !writer.u8(static_cast<std::uint8_t>(value.type)) || !writer.u64(value.account) ||
        !idempotency(writer, value.idempotency) || !tile(writer, value.tile) ||
        !writer.u32(value.expected_tile_revision) || !writer.u32(value.expected_inventory_revision) ||
        !writer.u8(value.inventory_slot) || !writer.u8(value.tool_slot) || !writer.u16(value.expected_item)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, WorldOperation& value) {
    ByteReader reader(input);
    std::uint8_t type;
    if (!reader.u8(type) || type > static_cast<std::uint8_t>(WorldOpType::FillHole) ||
        !reader.u64(value.account) || !idempotency(reader, value.idempotency) || !tile(reader, value.tile) ||
        !reader.u32(value.expected_tile_revision) || !reader.u32(value.expected_inventory_revision) ||
        !reader.u8(value.inventory_slot) || !reader.u8(value.tool_slot) ||
        !reader.u16(value.expected_item) || !reader.finished()) return false;
    value.type = static_cast<WorldOpType>(type);
    return true;
}

bool encode(const WorldResult& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!result(writer, value.code) || !idempotency(writer, value.idempotency) || !tile(writer, value.tile) ||
        !writer.u32(value.tile_revision) || !writer.u32(value.inventory_revision) ||
        !writer.u16(value.transferred_item) || !writer.u8(value.inventory_slot) ||
        !writer.u8(value.replayed ? 1 : 0)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, WorldResult& value) {
    ByteReader reader(input);
    std::uint8_t replayed;
    if (!result(reader, value.code) || !idempotency(reader, value.idempotency) || !tile(reader, value.tile) ||
        !reader.u32(value.tile_revision) || !reader.u32(value.inventory_revision) ||
        !reader.u16(value.transferred_item) || !reader.u8(value.inventory_slot) || !reader.u8(replayed) ||
        replayed > 1 || !reader.finished()) return false;
    value.replayed = replayed != 0;
    return true;
}

bool encode(const EconomyRequest& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (static_cast<std::uint8_t>(value.type) > static_cast<std::uint8_t>(EconomyOpType::AttachMail) ||
        !writer.u8(static_cast<std::uint8_t>(value.type)) || !writer.u64(value.account) ||
        !idempotency(writer, value.idempotency) || !writer.u32(value.expected_inventory_revision) ||
        !writer.u32(value.expected_aux_revision) || !writer.u32(value.shop_index) ||
        !writer.u8(value.inventory_slot) || !writer.u16(value.expected_item) || !writer.u64(value.amount) ||
        !writer.u64(value.recipient)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, EconomyRequest& value) {
    ByteReader reader(input);
    std::uint8_t type;
    if (!reader.u8(type) || type > static_cast<std::uint8_t>(EconomyOpType::AttachMail) ||
        !reader.u64(value.account) || !idempotency(reader, value.idempotency) ||
        !reader.u32(value.expected_inventory_revision) || !reader.u32(value.expected_aux_revision) ||
        !reader.u32(value.shop_index) || !reader.u8(value.inventory_slot) ||
        !reader.u16(value.expected_item) || !reader.u64(value.amount) || !reader.u64(value.recipient) ||
        !reader.finished()) return false;
    value.type = static_cast<EconomyOpType>(type);
    return true;
}

bool encode(const EconomyResult& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!result(writer, value.code) || !idempotency(writer, value.idempotency) ||
        !writer.u32(value.inventory_revision) || !writer.u32(value.auxiliary_revision) ||
        !writer.u64(value.balance) || !writer.u64(value.debt) || !writer.u32(value.bells) ||
        !writer.u16(value.item) || !writer.u8(value.inventory_slot) || !writer.u64(value.mail_id) ||
        !writer.u8(value.replayed ? 1 : 0)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, EconomyResult& value) {
    ByteReader reader(input);
    std::uint8_t replayed;
    if (!result(reader, value.code) || !idempotency(reader, value.idempotency) ||
        !reader.u32(value.inventory_revision) || !reader.u32(value.auxiliary_revision) ||
        !reader.u64(value.balance) || !reader.u64(value.debt) || !reader.u32(value.bells) ||
        !reader.u16(value.item) || !reader.u8(value.inventory_slot) || !reader.u64(value.mail_id) ||
        !reader.u8(replayed) || replayed > 1 || !reader.finished()) return false;
    value.replayed = replayed != 0;
    return true;
}

bool encode(const TradeRequest& value, std::vector<std::uint8_t>& output) {
    if (value.slots.size() > kInventorySlots ||
        static_cast<std::uint8_t>(value.action) > static_cast<std::uint8_t>(TradeAction::Cancel)) return false;
    ByteWriter writer;
    if (!writer.u8(static_cast<std::uint8_t>(value.action)) || !writer.u64(value.trade_id) ||
        !writer.u64(value.account) || !writer.u64(value.other_account) ||
        !writer.u32(value.expected_trade_revision) || !writer.u8(static_cast<std::uint8_t>(value.slots.size())) ||
        !writer.bytes(value.slots.data(), value.slots.size())) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, TradeRequest& value) {
    ByteReader reader(input);
    std::uint8_t action;
    std::uint8_t count;
    if (!reader.u8(action) || action > static_cast<std::uint8_t>(TradeAction::Cancel) ||
        !reader.u64(value.trade_id) || !reader.u64(value.account) || !reader.u64(value.other_account) ||
        !reader.u32(value.expected_trade_revision) || !reader.u8(count) || count > kInventorySlots) return false;
    value.slots.resize(count);
    if (!reader.bytes(value.slots.data(), value.slots.size()) || !reader.finished()) return false;
    value.action = static_cast<TradeAction>(action);
    return true;
}

bool encode(const TradeResult& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!result(writer, value.code) || !writer.u64(value.trade_id) || !writer.u32(value.trade_revision) ||
        !writer.u32(value.inventory_revision) || !writer.u8(value.finalized ? 1 : 0)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, TradeResult& value) {
    ByteReader reader(input);
    std::uint8_t finalized;
    if (!result(reader, value.code) || !reader.u64(value.trade_id) || !reader.u32(value.trade_revision) ||
        !reader.u32(value.inventory_revision) || !reader.u8(finalized) || finalized > 1 || !reader.finished()) return false;
    value.finalized = finalized != 0;
    return true;
}

bool encode(const ConversationRequest& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (static_cast<std::uint8_t>(value.action) > static_cast<std::uint8_t>(ConversationAction::End) ||
        !writer.u8(static_cast<std::uint8_t>(value.action)) || !writer.u64(value.account) ||
        !writer.u64(value.npc) || !writer.u32(value.lease_id) || !writer.u16(value.choice)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, ConversationRequest& value) {
    ByteReader reader(input);
    std::uint8_t action;
    if (!reader.u8(action) || action > static_cast<std::uint8_t>(ConversationAction::End) ||
        !reader.u64(value.account) || !reader.u64(value.npc) || !reader.u32(value.lease_id) ||
        !reader.u16(value.choice) || !reader.finished()) return false;
    value.action = static_cast<ConversationAction>(action);
    return true;
}

bool encode(const ConversationResult& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!result(writer, value.code) || !writer.u64(value.npc) || !writer.u32(value.lease_id) ||
        !writer.u32(value.dialogue_node) || !writer.u32(value.revision) || !writer.u32(value.expires_tick) ||
        !writer.u8(value.completed ? 1 : 0)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, ConversationResult& value) {
    ByteReader reader(input);
    std::uint8_t completed;
    if (!result(reader, value.code) || !reader.u64(value.npc) || !reader.u32(value.lease_id) ||
        !reader.u32(value.dialogue_node) || !reader.u32(value.revision) || !reader.u32(value.expires_tick) ||
        !reader.u8(completed) || completed > 1 || !reader.finished()) return false;
    value.completed = completed != 0;
    return true;
}

bool encode(const ZoneTransferRequest& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!writer.u64(value.account) || !writer.u32(value.door_id)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, ZoneTransferRequest& value) {
    ByteReader reader(input);
    return reader.u64(value.account) && reader.u32(value.door_id) && reader.finished();
}

bool encode(const TransferOffer& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!result(writer, value.code) || !writer.u64(value.account) || !writer.u32(value.source_zone) ||
        !writer.u32(value.destination_zone) || !vec3(writer, value.destination_position) ||
        !writer.u64(value.token.high) || !writer.u64(value.token.low) || !writer.u32(value.expires_tick) ||
        !writer.u32(value.baseline_revision)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, TransferOffer& value) {
    ByteReader reader(input);
    return result(reader, value.code) && reader.u64(value.account) && reader.u32(value.source_zone) &&
           reader.u32(value.destination_zone) && vec3(reader, value.destination_position) &&
           reader.u64(value.token.high) && reader.u64(value.token.low) && reader.u32(value.expires_tick) &&
           reader.u32(value.baseline_revision) && reader.finished();
}

bool encode(const ZoneReadyRequest& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!writer.u64(value.account) || !writer.u64(value.token.high) || !writer.u64(value.token.low)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, ZoneReadyRequest& value) {
    ByteReader reader(input);
    return reader.u64(value.account) && reader.u64(value.token.high) && reader.u64(value.token.low) && reader.finished();
}

bool encode(const FurnitureOperation& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (static_cast<std::uint8_t>(value.type) > static_cast<std::uint8_t>(FurnitureOpType::Remove) ||
        !writer.u8(static_cast<std::uint8_t>(value.type)) || !writer.u64(value.account) ||
        !idempotency(writer, value.idempotency) || !writer.u64(value.house_id) || !writer.u8(value.address.x) ||
        !writer.u8(value.address.z) || !writer.u8(value.address.layer) || !writer.u32(value.expected_house_revision) ||
        !writer.u32(value.expected_inventory_revision) || !writer.u8(value.inventory_slot) ||
        !writer.u16(value.expected_item)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, FurnitureOperation& value) {
    ByteReader reader(input);
    std::uint8_t type;
    if (!reader.u8(type) || type > static_cast<std::uint8_t>(FurnitureOpType::Remove) ||
        !reader.u64(value.account) || !idempotency(reader, value.idempotency) || !reader.u64(value.house_id) ||
        !reader.u8(value.address.x) || !reader.u8(value.address.z) || !reader.u8(value.address.layer) ||
        !reader.u32(value.expected_house_revision) || !reader.u32(value.expected_inventory_revision) ||
        !reader.u8(value.inventory_slot) || !reader.u16(value.expected_item) || !reader.finished()) return false;
    value.type = static_cast<FurnitureOpType>(type);
    return true;
}

bool encode(const FurnitureResult& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!result(writer, value.code) || !idempotency(writer, value.idempotency) || !writer.u64(value.house_id) ||
        !writer.u32(value.house_revision) || !writer.u32(value.inventory_revision) ||
        !writer.u8(value.inventory_slot) || !writer.u16(value.item) || !writer.u8(value.replayed ? 1 : 0)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, FurnitureResult& value) {
    ByteReader reader(input);
    std::uint8_t replayed;
    if (!result(reader, value.code) || !idempotency(reader, value.idempotency) || !reader.u64(value.house_id) ||
        !reader.u32(value.house_revision) || !reader.u32(value.inventory_revision) ||
        !reader.u8(value.inventory_slot) || !reader.u16(value.item) || !reader.u8(replayed) ||
        replayed > 1 || !reader.finished()) return false;
    value.replayed = replayed != 0;
    return true;
}

bool encode(const EncounterRequest& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (static_cast<std::uint8_t>(value.kind) > static_cast<std::uint8_t>(EncounterKind::Insect) ||
        !writer.u64(value.account) || !idempotency(writer, value.idempotency) ||
        !writer.u8(static_cast<std::uint8_t>(value.kind)) || !writer.u32(value.expected_inventory_revision) ||
        !writer.u8(value.tool_slot)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, EncounterRequest& value) {
    ByteReader reader(input);
    std::uint8_t kind;
    if (!reader.u64(value.account) || !idempotency(reader, value.idempotency) || !reader.u8(kind) ||
        kind > static_cast<std::uint8_t>(EncounterKind::Insect) ||
        !reader.u32(value.expected_inventory_revision) || !reader.u8(value.tool_slot) || !reader.finished()) return false;
    value.kind = static_cast<EncounterKind>(kind);
    return true;
}

bool encode(const EncounterResult& value, std::vector<std::uint8_t>& output) {
    ByteWriter writer;
    if (!result(writer, value.code) || !idempotency(writer, value.idempotency) ||
        !writer.u32(value.inventory_revision) || !writer.u16(value.item) || !writer.u8(value.inventory_slot) ||
        !writer.u32(value.next_allowed_tick) || !writer.u8(value.caught ? 1 : 0) ||
        !writer.u8(value.replayed ? 1 : 0)) return false;
    output = writer.data();
    return true;
}

bool decode(const std::vector<std::uint8_t>& input, EncounterResult& value) {
    ByteReader reader(input);
    std::uint8_t caught;
    std::uint8_t replayed;
    if (!result(reader, value.code) || !idempotency(reader, value.idempotency) ||
        !reader.u32(value.inventory_revision) || !reader.u16(value.item) || !reader.u8(value.inventory_slot) ||
        !reader.u32(value.next_allowed_tick) || !reader.u8(caught) || !reader.u8(replayed) || caught > 1 ||
        replayed > 1 || !reader.finished()) return false;
    value.caught = caught != 0;
    value.replayed = replayed != 0;
    return true;
}

bool encode_deltas(const std::vector<ReplicationDelta>& value, std::vector<std::uint8_t>& output) {
    if (value.size() > 1024) return false;
    ByteWriter writer(kMaximumTransferBytes);
    if (!writer.u16(static_cast<std::uint16_t>(value.size()))) return false;
    for (const ReplicationDelta& delta : value) {
        if (static_cast<std::uint8_t>(delta.kind) > static_cast<std::uint8_t>(ResourceKind::Event) ||
            delta.payload.size() > 65535 || !writer.u32(delta.revision) ||
            !writer.u8(static_cast<std::uint8_t>(delta.kind)) || !writer.u32(delta.zone) ||
            !writer.u64(delta.target_account) || !writer.u64(delta.entity) ||
            !writer.u8(delta.reliable ? 1 : 0) || !writer.u8(delta.has_position ? 1 : 0) ||
            !vec3(writer, delta.position) || !writer.u16(static_cast<std::uint16_t>(delta.payload.size())) ||
            !writer.bytes(delta.payload.data(), delta.payload.size())) return false;
    }
    output = writer.data();
    return true;
}

bool decode_deltas(const std::vector<std::uint8_t>& input, std::vector<ReplicationDelta>& value) {
    if (input.size() > kMaximumTransferBytes) return false;
    ByteReader reader(input);
    std::uint16_t count;
    if (!reader.u16(count) || count > 1024) return false;
    value.clear();
    value.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        ReplicationDelta delta;
        std::uint8_t kind;
        std::uint8_t reliable;
        std::uint8_t has_position;
        std::uint16_t size;
        if (!reader.u32(delta.revision) || !reader.u8(kind) || kind > static_cast<std::uint8_t>(ResourceKind::Event) ||
            !reader.u32(delta.zone) || !reader.u64(delta.target_account) || !reader.u64(delta.entity) ||
            !reader.u8(reliable) || !reader.u8(has_position) || reliable > 1 || has_position > 1 ||
            !vec3(reader, delta.position) || !reader.u16(size) || size > reader.remaining()) return false;
        delta.kind = static_cast<ResourceKind>(kind);
        delta.reliable = reliable != 0;
        delta.has_position = has_position != 0;
        delta.payload.resize(size);
        if (!reader.bytes(delta.payload.data(), size)) return false;
        value.push_back(std::move(delta));
    }
    return reader.finished();
}

} // namespace acnet
