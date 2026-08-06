#include "acnet/replication.hpp"

#include "acnet/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace acnet {
namespace {

constexpr std::size_t kMaximumBaselineBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumBaselineTiles = 65535;
constexpr std::size_t kMaximumBaselineNpcs = 256;
constexpr std::size_t kMaximumShopEntries = 256;

bool valid_transition(const PlayerSnapshot& player) {
    const std::uint8_t phase = static_cast<std::uint8_t>(player.transition_phase);
    return phase <= static_cast<std::uint8_t>(DoorTransitionPhase::Arriving) &&
           ((phase == 0 && player.transition_door == 0 && player.transition_expires_tick == 0) ||
            (phase != 0 && player.transition_door != 0 && player.transition_expires_tick != 0));
}

bool encode_house(ByteWriter& writer, const HouseState& house) {
    if (house.house_id == 0 || house.owner == 0 || house.original_slot >= kOriginalResidentSlots ||
        house.zone == 0 || house.upgrade_level > kMaximumHouseUpgradeLevel || house.revision == 0 ||
        house.furniture.size() > kMaximumHouseFurniture) return false;
    if (!writer.u64(house.house_id) || !writer.u64(house.owner) || !writer.u8(house.original_slot) ||
        !writer.u32(house.zone) || !writer.u8(house.upgrade_level) || !writer.u32(house.revision) ||
        !writer.u8(house.initialized ? 1 : 0) || !writer.u8(house.main_light_on ? 1 : 0) ||
        !writer.u8(house.basement_light_on ? 1 : 0)) return false;
    for (std::int16_t music : house.music_tracks) {
        if (!writer.i16(music)) return false;
    }
    for (std::uint64_t switches : house.furniture_switches) {
        if (!writer.u64(switches)) return false;
    }
    std::vector<std::pair<FurnitureAddress, ItemSlot>> furniture(house.furniture.begin(), house.furniture.end());
    std::sort(furniture.begin(), furniture.end(), [](const auto& left, const auto& right) {
        if (left.first.floor != right.first.floor) return left.first.floor < right.first.floor;
        if (left.first.layer != right.first.layer) return left.first.layer < right.first.layer;
        if (left.first.z != right.first.z) return left.first.z < right.first.z;
        return left.first.x < right.first.x;
    });
    if (!writer.u16(static_cast<std::uint16_t>(furniture.size()))) return false;
    for (const auto& entry : furniture) {
        if (entry.first.x >= 16 || entry.first.z >= 16 || entry.first.floor >= kHouseFloorCount ||
            entry.first.layer >= kHouseLayerCount || entry.second.item == 0 ||
            !writer.u8(entry.first.x) || !writer.u8(entry.first.z) || !writer.u8(entry.first.floor) ||
            !writer.u8(entry.first.layer) || !writer.u16(entry.second.item) ||
            !writer.u8(entry.second.condition)) return false;
    }
    return true;
}

bool decode_house(ByteReader& reader, HouseState& house) {
    std::uint8_t initialized;
    std::uint8_t main_light;
    std::uint8_t basement_light;
    std::uint16_t count;
    if (!reader.u64(house.house_id) || !reader.u64(house.owner) || !reader.u8(house.original_slot) ||
        !reader.u32(house.zone) || !reader.u8(house.upgrade_level) || !reader.u32(house.revision) ||
        !reader.u8(initialized) || !reader.u8(main_light) || !reader.u8(basement_light) ||
        initialized > 1 || main_light > 1 || basement_light > 1) return false;
    for (std::int16_t& music : house.music_tracks) {
        if (!reader.i16(music)) return false;
    }
    for (std::uint64_t& switches : house.furniture_switches) {
        if (!reader.u64(switches)) return false;
    }
    if (!reader.u16(count) || count > kMaximumHouseFurniture || house.house_id == 0 || house.owner == 0 ||
        house.original_slot >= kOriginalResidentSlots || house.zone == 0 ||
        house.upgrade_level > kMaximumHouseUpgradeLevel ||
        house.revision == 0) return false;
    house.initialized = initialized != 0;
    house.main_light_on = main_light != 0;
    house.basement_light_on = basement_light != 0;
    house.furniture.clear();
    for (std::uint16_t i = 0; i < count; ++i) {
        FurnitureAddress address;
        ItemSlot item;
        if (!reader.u8(address.x) || !reader.u8(address.z) || !reader.u8(address.floor) ||
            !reader.u8(address.layer) || !reader.u16(item.item) || !reader.u8(item.condition) ||
            address.x >= 16 || address.z >= 16 || address.floor >= kHouseFloorCount ||
            address.layer >= kHouseLayerCount || item.item == 0 ||
            !house.furniture.emplace(address, item).second) return false;
    }
    return true;
}

bool encode_transform(ByteWriter& writer, const Transform& value) {
    return writer.f32(value.position.x) && writer.f32(value.position.y) && writer.f32(value.position.z) &&
           writer.f32(value.velocity.x) && writer.f32(value.velocity.y) && writer.f32(value.velocity.z) &&
           writer.i16(value.yaw) && writer.u16(value.action);
}

bool decode_transform(ByteReader& reader, Transform& value) {
    return reader.f32(value.position.x) && reader.f32(value.position.y) && reader.f32(value.position.z) &&
           reader.f32(value.velocity.x) && reader.f32(value.velocity.y) && reader.f32(value.velocity.z) &&
           reader.i16(value.yaw) && reader.u16(value.action) && finite(value.position) && finite(value.velocity);
}

bool encode_appearance(ByteWriter& writer, const PlayerAppearance& appearance) {
    return appearance.gender <= 2 && appearance.face < 8 &&
           writer.bytes(appearance.name.data(), appearance.name.size()) && writer.u8(appearance.gender) &&
           writer.u8(appearance.face) && writer.u16(appearance.clothing) && writer.u16(appearance.equipped_item);
}

bool decode_appearance(ByteReader& reader, PlayerAppearance& appearance) {
    return reader.bytes(appearance.name.data(), appearance.name.size()) && reader.u8(appearance.gender) &&
           reader.u8(appearance.face) && reader.u16(appearance.clothing) &&
           reader.u16(appearance.equipped_item) && appearance.gender <= 2 && appearance.face < 8;
}

Revision next_revision(Revision revision) {
    return revision == std::numeric_limits<Revision>::max() ? 1 : revision + 1;
}

} // namespace

bool encode_baseline(const ZoneBaseline& baseline, std::vector<std::uint8_t>& output) {
    if (baseline.zone == 0 || baseline.revision == 0 || baseline.tiles.size() > kMaximumBaselineTiles ||
        baseline.players.size() > kMaxPlayersPerZone || baseline.npcs.size() > kMaximumBaselineNpcs ||
        baseline.inventory.revision == 0 || baseline.ledger.revision == 0 || baseline.shop.revision == 0 ||
        baseline.shop.stock.size() > kMaximumShopEntries || (baseline.house_light_mask & 0xF0U) != 0 ||
        baseline.town_capacity == 0 || baseline.town_population > baseline.town_capacity ||
        (baseline.has_house && baseline.house.zone != baseline.zone)) return false;
    ByteWriter writer(kMaximumBaselineBytes);
    if (!writer.u32(baseline.server_tick) || !writer.u32(baseline.revision) || !writer.u32(baseline.zone) ||
        !writer.u64(static_cast<std::uint64_t>(baseline.town_unix_seconds)) || !writer.u8(baseline.weather) ||
        !writer.u8(baseline.weather_intensity) || !writer.u8(baseline.town_population) ||
        !writer.u8(baseline.town_capacity) || !writer.u8(baseline.house_light_mask) ||
        !writer.u8(baseline.has_house ? 1 : 0) ||
        !writer.u32(static_cast<std::uint32_t>(baseline.tiles.size())) ||
        !writer.u16(static_cast<std::uint16_t>(baseline.players.size())) ||
        !writer.u16(static_cast<std::uint16_t>(baseline.npcs.size())) ||
        !writer.u32(baseline.inventory.revision) || !writer.u32(baseline.inventory.bells)) return false;
    for (const ItemSlot& slot : baseline.inventory.slots) {
        if (!writer.u16(slot.item) || !writer.u8(slot.condition)) return false;
    }
    if (!writer.u32(baseline.ledger.revision) || !writer.u64(baseline.ledger.bank_balance) ||
        !writer.u64(baseline.ledger.debt) || !writer.u32(baseline.shop.revision) ||
        !writer.u16(static_cast<std::uint16_t>(baseline.shop.stock.size()))) return false;
    for (const ShopEntry& entry : baseline.shop.stock) {
        if (!writer.u16(entry.item) || !writer.u32(entry.price) || !writer.u16(entry.quantity)) return false;
    }
    if (baseline.has_house && !encode_house(writer, baseline.house)) return false;
    for (const auto& entry : baseline.tiles) {
        if (!writer.i16(entry.first.x) || !writer.i16(entry.first.z) || !writer.u32(entry.second.revision) ||
            !writer.u16(entry.second.item) || !writer.u8(entry.second.condition) ||
            !writer.u8(static_cast<std::uint8_t>(entry.second.terrain)) ||
            !writer.u8(entry.second.buried ? 1 : 0) || !writer.u8(entry.second.placed_furniture ? 1 : 0)) return false;
    }
    for (const PlayerSnapshot& player : baseline.players) {
        if (!valid_transition(player)) return false;
        if (!writer.u64(player.entity) || !writer.u64(player.account) || !writer.u32(player.zone) ||
            !writer.u32(player.acknowledged_input) || !encode_transform(writer, player.transform) ||
            !encode_appearance(writer, player.appearance) ||
            !writer.u8(static_cast<std::uint8_t>(player.transition_phase)) ||
            !writer.u32(player.transition_door) || !writer.u32(player.transition_expires_tick)) return false;
    }
    for (const NpcState& npc : baseline.npcs) {
        if (!writer.u64(npc.entity) || !writer.u32(npc.zone) || !writer.u32(npc.revision) ||
            !writer.u16(npc.schedule_state) || !writer.u16(npc.animation) || !writer.u16(npc.emotion) ||
            !writer.u64(npc.destination) || !encode_transform(writer, npc.transform)) return false;
    }
    output = writer.data();
    return true;
}

bool decode_baseline(const std::vector<std::uint8_t>& input, ZoneBaseline& baseline) {
    if (input.size() > kMaximumBaselineBytes) return false;
    ByteReader reader(input);
    std::uint64_t town_time;
    std::uint32_t tile_count;
    std::uint16_t player_count;
    std::uint16_t npc_count;
    std::uint16_t shop_count;
    std::uint8_t has_house;
    if (!reader.u32(baseline.server_tick) || !reader.u32(baseline.revision) || !reader.u32(baseline.zone) ||
        !reader.u64(town_time) || !reader.u8(baseline.weather) || !reader.u8(baseline.weather_intensity) ||
        !reader.u8(baseline.town_population) || !reader.u8(baseline.town_capacity) ||
        baseline.town_capacity == 0 || baseline.town_population > baseline.town_capacity ||
        !reader.u8(baseline.house_light_mask) || !reader.u8(has_house) || has_house > 1 ||
        (baseline.house_light_mask & 0xF0U) != 0 ||
        !reader.u32(tile_count) || !reader.u16(player_count) || !reader.u16(npc_count) || baseline.zone == 0 ||
        baseline.revision == 0 || tile_count > kMaximumBaselineTiles || player_count > kMaxPlayersPerZone ||
        npc_count > kMaximumBaselineNpcs || town_time > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return false;
    if (!reader.u32(baseline.inventory.revision) || !reader.u32(baseline.inventory.bells) ||
        baseline.inventory.revision == 0) return false;
    for (ItemSlot& slot : baseline.inventory.slots) {
        if (!reader.u16(slot.item) || !reader.u8(slot.condition)) return false;
    }
    if (!reader.u32(baseline.ledger.revision) || !reader.u64(baseline.ledger.bank_balance) ||
        !reader.u64(baseline.ledger.debt) || !reader.u32(baseline.shop.revision) ||
        !reader.u16(shop_count) || baseline.ledger.revision == 0 || baseline.shop.revision == 0 ||
        shop_count > kMaximumShopEntries) return false;
    baseline.shop.stock.clear();
    baseline.shop.stock.resize(shop_count);
    for (ShopEntry& entry : baseline.shop.stock) {
        if (!reader.u16(entry.item) || !reader.u32(entry.price) || !reader.u16(entry.quantity)) return false;
    }
    baseline.has_house = has_house != 0;
    baseline.house = {};
    if (baseline.has_house && (!decode_house(reader, baseline.house) || baseline.house.zone != baseline.zone)) return false;
    baseline.town_unix_seconds = static_cast<std::int64_t>(town_time);
    baseline.tiles.clear();
    baseline.players.clear();
    baseline.npcs.clear();
    baseline.tiles.reserve(tile_count);
    for (std::uint32_t i = 0; i < tile_count; ++i) {
        TileAddress address;
        address.zone = baseline.zone;
        TileState tile;
        std::uint8_t terrain;
        std::uint8_t buried;
        std::uint8_t placed;
        if (!reader.i16(address.x) || !reader.i16(address.z) || !reader.u32(tile.revision) ||
            !reader.u16(tile.item) || !reader.u8(tile.condition) || !reader.u8(terrain) ||
            !reader.u8(buried) || !reader.u8(placed) || terrain > static_cast<std::uint8_t>(TerrainState::Planted) ||
            buried > 1 || placed > 1 || tile.revision == 0) return false;
        tile.terrain = static_cast<TerrainState>(terrain);
        tile.buried = buried != 0;
        tile.placed_furniture = placed != 0;
        baseline.tiles.emplace_back(address, tile);
    }
    baseline.players.reserve(player_count);
    for (std::uint16_t i = 0; i < player_count; ++i) {
        PlayerSnapshot player;
        std::uint8_t transition_phase;
        if (!reader.u64(player.entity) || !reader.u64(player.account) || !reader.u32(player.zone) ||
            !reader.u32(player.acknowledged_input) || !decode_transform(reader, player.transform) ||
            !decode_appearance(reader, player.appearance) || !reader.u8(transition_phase) ||
            !reader.u32(player.transition_door) || !reader.u32(player.transition_expires_tick) ||
            transition_phase > static_cast<std::uint8_t>(DoorTransitionPhase::Arriving) ||
            player.entity == 0 || player.account == 0 || player.zone != baseline.zone) return false;
        player.transition_phase = static_cast<DoorTransitionPhase>(transition_phase);
        if (!valid_transition(player)) return false;
        baseline.players.push_back(player);
    }
    baseline.npcs.reserve(npc_count);
    for (std::uint16_t i = 0; i < npc_count; ++i) {
        NpcState npc;
        if (!reader.u64(npc.entity) || !reader.u32(npc.zone) || !reader.u32(npc.revision) ||
            !reader.u16(npc.schedule_state) || !reader.u16(npc.animation) || !reader.u16(npc.emotion) ||
            !reader.u64(npc.destination) || !decode_transform(reader, npc.transform) || npc.entity == 0 ||
            npc.zone != baseline.zone || npc.revision == 0) return false;
        baseline.npcs.push_back(npc);
    }
    return reader.finished();
}

bool encode_tile_delta(const TileStateDelta& delta, std::vector<std::uint8_t>& output) {
    if (delta.address.zone == 0 || delta.state.revision == 0 ||
        static_cast<std::uint8_t>(delta.state.terrain) > static_cast<std::uint8_t>(TerrainState::Planted)) return false;
    ByteWriter writer(32);
    if (!writer.u32(delta.address.zone) || !writer.i16(delta.address.x) || !writer.i16(delta.address.z) ||
        !writer.u32(delta.state.revision) || !writer.u16(delta.state.item) || !writer.u8(delta.state.condition) ||
        !writer.u8(static_cast<std::uint8_t>(delta.state.terrain)) ||
        !writer.u8(delta.state.buried ? 1 : 0) || !writer.u8(delta.state.placed_furniture ? 1 : 0)) return false;
    output = writer.data();
    return true;
}

bool decode_tile_delta(const std::vector<std::uint8_t>& input, TileStateDelta& delta) {
    ByteReader reader(input);
    std::uint8_t terrain;
    std::uint8_t buried;
    std::uint8_t placed;
    if (!reader.u32(delta.address.zone) || !reader.i16(delta.address.x) || !reader.i16(delta.address.z) ||
        !reader.u32(delta.state.revision) || !reader.u16(delta.state.item) ||
        !reader.u8(delta.state.condition) || !reader.u8(terrain) || !reader.u8(buried) ||
        !reader.u8(placed) || !reader.finished() || delta.address.zone == 0 || delta.state.revision == 0 ||
        terrain > static_cast<std::uint8_t>(TerrainState::Planted) || buried > 1 || placed > 1) return false;
    delta.state.terrain = static_cast<TerrainState>(terrain);
    delta.state.buried = buried != 0;
    delta.state.placed_furniture = placed != 0;
    return true;
}

bool encode_town_delta(const TownOccupancy& occupancy, std::vector<std::uint8_t>& output) {
    if (occupancy.capacity == 0 || occupancy.population > occupancy.capacity) return false;
    ByteWriter writer(2);
    if (!writer.u8(occupancy.population) || !writer.u8(occupancy.capacity)) return false;
    output = writer.data();
    return true;
}

bool decode_town_delta(const std::vector<std::uint8_t>& input, TownOccupancy& occupancy) {
    ByteReader reader(input);
    if (!reader.u8(occupancy.population) || !reader.u8(occupancy.capacity)) return false;
    return occupancy.capacity != 0 && occupancy.population <= occupancy.capacity;
}

DeltaLog::DeltaLog(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {}

Revision DeltaLog::append(ReplicationDelta delta) {
    revision_ = next_revision(revision_);
    delta.revision = revision_;
    deltas_.push_back(std::move(delta));
    while (deltas_.size() > capacity_) deltas_.pop_front();
    return revision_;
}

bool DeltaLog::relevant(const ReplicationDelta& delta, const InterestContext& interest) {
    if (delta.target_account != 0 && delta.target_account != interest.account) return false;
    if (delta.kind == ResourceKind::Clock || delta.kind == ResourceKind::Weather ||
        delta.kind == ResourceKind::Town) return true; /* town-wide: not zone or distance scoped */
    if (delta.zone != 0 && delta.zone != interest.zone) return false;
    if (!interest.exterior || !delta.has_position || delta.reliable) return true;
    const float dx = delta.position.x - interest.position.x;
    const float dy = delta.position.y - interest.position.y;
    const float dz = delta.position.z - interest.position.z;
    return dx * dx + dy * dy + dz * dz <= interest.radius * interest.radius;
}

DeltaQueryResult DeltaLog::since(Revision after, const InterestContext& interest, std::size_t maximum) const {
    DeltaQueryResult result;
    result.newest_revision = revision_;
    if (maximum == 0) return result;
    if (!deltas_.empty() && after != 0 && after < deltas_.front().revision - 1) {
        result.requires_baseline = true;
        return result;
    }
    for (const ReplicationDelta& delta : deltas_) {
        if (delta.revision > after && relevant(delta, interest)) {
            result.deltas.push_back(delta);
            if (result.deltas.size() >= maximum) break;
        }
    }
    return result;
}

ZoneBaseline build_baseline(ZoneId zone,
                            Tick tick,
                            Revision revision,
                            std::int64_t town_unix_seconds,
                            std::uint8_t weather,
                            std::uint8_t weather_intensity,
                            const WorldAuthority& world,
                            const PlayerDirectory& players,
                            const NpcAuthority& npcs) {
    ZoneBaseline result;
    result.server_tick = tick;
    result.revision = revision;
    result.zone = zone;
    result.town_unix_seconds = town_unix_seconds;
    result.weather = weather;
    result.weather_intensity = weather_intensity;
    result.tiles = world.tiles_in_zone(zone);
    for (const PlayerView* player : players.query_zone(zone, kMaxPlayersPerZone)) {
        PlayerSnapshot snapshot;
        snapshot.entity = player->entity;
        snapshot.account = player->account;
        snapshot.zone = player->zone;
        snapshot.transform = player->transform;
        snapshot.appearance = player->appearance;
        result.players.push_back(snapshot);
    }
    result.npcs = npcs.zone_snapshot(zone);
    return result;
}

} // namespace acnet
