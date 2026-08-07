#include "acserver/town_runtime.hpp"

#include "acnet/protocol.hpp"
#include "acserver/gci.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <utility>
#include <unordered_set>
#include <vector>

namespace acserver {

namespace {

/* Transaction-log operation codes for operator gifts. They sit above the
 * MessageType values the player-driven rows use so the two cannot be confused
 * when auditing town.db. */
constexpr std::uint16_t kAdminGrantBellsOperation = 0xA001;
constexpr std::uint16_t kAdminSendMailOperation = 0xA002;

acnet::Revision advance_revision(acnet::Revision revision) {
    return revision == std::numeric_limits<acnet::Revision>::max() ? 1 : revision + 1;
}

void advance_planted_tile(acnet::TileState& tile) {
    if (tile.item >= 0x083C && tile.item <= 0x0844) {
        tile.item = static_cast<std::uint16_t>(tile.item + 9);
        tile.terrain = acnet::TerrainState::Normal;
        return;
    }
    const auto grow_group = [&](std::uint16_t first, std::uint16_t mature) {
        if (tile.item < first || tile.item >= mature) return false;
        ++tile.item;
        tile.terrain = tile.item == mature ? acnet::TerrainState::Tree : acnet::TerrainState::Planted;
        return true;
    };
    if (grow_group(0x0800, 0x0804) || grow_group(0x0805, 0x0809) ||
        grow_group(0x080D, 0x0811) || grow_group(0x0815, 0x0819) ||
        grow_group(0x081D, 0x0821) || grow_group(0x0825, 0x0829) ||
        grow_group(0x082D, 0x0831) || grow_group(0x0832, 0x0836) ||
        grow_group(0x0837, 0x083B) || grow_group(0x084F, 0x0853) ||
        grow_group(0x0854, 0x0858) || grow_group(0x085D, 0x0861) ||
        grow_group(0x0863, 0x0868)) return;
    tile.item = 0;
    tile.terrain = acnet::TerrainState::Normal;
}

bool blocking_structure_item(std::uint16_t item) {
    return (item >= 0x5800 && item <= 0x5852) || (item >= 0xF0F3 && item <= 0xF0FA) ||
           item == 0xF0FF || item == 0xF101 || item == 0xF120;
}

acnet::PlayerAppearance default_appearance(acnet::AccountId account) {
    acnet::PlayerAppearance appearance;
    appearance.name[0] = 'P';
    std::uint64_t value = account;
    for (std::size_t i = 0; i < 7; ++i) {
        appearance.name[7 - i] = static_cast<std::uint8_t>('0' + value % 10);
        value /= 10;
    }
    appearance.gender = static_cast<std::uint8_t>(account & 1U);
    appearance.face = static_cast<std::uint8_t>(account % 8U);
    appearance.clothing = static_cast<std::uint16_t>(0x2400U + account % 0xFFU);
    appearance.clothing_index = static_cast<std::uint16_t>(appearance.clothing - 0x2400U);
    appearance.revision = 1;
    return appearance;
}

acnet::Vec3 resident_spawn(std::uint8_t slot) {
    return {2200.0F + static_cast<float>(slot & 1U) * 40.0F,
            0.0F,
            1000.0F + static_cast<float>(slot >> 1U) * 40.0F};
}

bool legacy_train_spawn(const acnet::Transform& transform) {
    return std::fabs(transform.position.x - 1980.0F) < 0.5F &&
           std::fabs(transform.position.z - 780.0F) < 0.5F;
}

bool valid_transition_transform(const acnet::Transform& transform) {
    constexpr float limit = 100000.0F;
    return acnet::finite(transform.position) && acnet::finite(transform.velocity) &&
           std::fabs(transform.position.x) <= limit && std::fabs(transform.position.y) <= limit &&
           std::fabs(transform.position.z) <= limit;
}

acnet::TerrainState terrain_for_bootstrap_item(std::uint16_t item) {
    if ((item >= 0x0011 && item <= 0x005B)) return acnet::TerrainState::Hole;
    if ((item >= 0x0001 && item <= 0x0004) || (item >= 0x0070 && item <= 0x0077) ||
        (item >= 0x007B && item <= 0x007E)) return acnet::TerrainState::Stump;
    if ((item >= 0x005E && item <= 0x0061) || item == 0x0069 ||
        (item >= 0x0078 && item <= 0x0082)) return acnet::TerrainState::Tree;
    if (item < 0x0800 || item > 0x0869) return acnet::TerrainState::Normal;
    const std::uint16_t offset = static_cast<std::uint16_t>(item - 0x0800);
    if ((offset <= 3) || (offset >= 5 && offset <= 8) || (offset >= 13 && offset <= 16) ||
        (offset >= 21 && offset <= 24) || (offset >= 29 && offset <= 32) ||
        (offset >= 37 && offset <= 40) || (offset >= 45 && offset <= 48) ||
        (offset >= 50 && offset <= 53) || (offset >= 55 && offset <= 58) ||
        (offset >= 60 && offset <= 68) || offset == 78 || (offset >= 79 && offset <= 82) ||
        (offset >= 84 && offset <= 87) || offset == 92 || (offset >= 93 && offset <= 96) ||
        offset == 98 || (offset >= 99 && offset <= 102) || offset == 105)
        return acnet::TerrainState::Planted;
    return acnet::TerrainState::Tree;
}

} // namespace

std::uint64_t monotonic_milliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
}

std::int64_t wall_unix_seconds() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

std::size_t TownRuntime::connected_residents() const {
    std::size_t count = 0;
    for (const auto& connection : connections_) {
        const auto account = accounts_.find(connection.second.account);
        if (account != accounts_.end() && account->second.kind == acnet::PlayerKind::Resident) ++count;
    }
    return count;
}

std::size_t TownRuntime::connected_visitors() const {
    return connected_clients() - connected_residents();
}

std::size_t TownRuntime::registered_residents() const {
    return static_cast<std::size_t>(std::count_if(accounts_.begin(), accounts_.end(), [](const auto& account) {
        return account.second.kind == acnet::PlayerKind::Resident;
    }));
}

TownRuntime::IslandStatus TownRuntime::island_status() const {
    IslandStatus status;
    status.tiles = world_.tiles_in_zone(acnet::kIslandZone).size();
    status.terrain_ready = status.tiles != 0;
    for (const auto& entry : connections_) {
        const acnet::PlayerView* viewer = players_.by_account(entry.second.account);
        if (viewer == nullptr) continue;
        if (viewer->zone == acnet::kIslandZone) ++status.outdoor_players;
        else if (viewer->zone == acnet::kIslandCabinZone) ++status.cabin_players;
        else if (viewer->zone == acnet::kIslandNpcHouseZone) ++status.islander_house_players;
    }
    if (const acnet::HouseState* cabin = housing_.house(acnet::kIslandCabinHouseId)) {
        status.cabin_furniture = cabin->furniture.size();
    }
    for (const auto& entry : npcs_.all_npcs()) {
        if (entry.second.zone == acnet::kIslandZone) status.islander_present = true;
    }
    return status;
}

std::vector<RuntimePlayerStatus> TownRuntime::player_statuses() const {
    std::vector<RuntimePlayerStatus> statuses;
    statuses.reserve(accounts_.size());
    for (const auto& entry : accounts_) {
        RuntimePlayerStatus status;
        status.account = entry.first;
        status.zone = entry.second.zone;
        status.resident_slot = entry.second.resident_slot;
        status.resident = entry.second.kind == acnet::PlayerKind::Resident;
        status.connected = std::any_of(connections_.begin(), connections_.end(), [&](const auto& connection) {
            return connection.second.account == entry.first;
        });
        for (std::uint8_t character : entry.second.appearance.name) {
            if (character < 32 || character > 126) break;
            status.name.push_back(static_cast<char>(character));
        }
        while (!status.name.empty() && status.name.back() == ' ') status.name.pop_back();
        if (status.name.empty()) status.name = "(creating character)";
        statuses.push_back(std::move(status));
    }
    std::sort(statuses.begin(), statuses.end(), [](const RuntimePlayerStatus& left,
                                                   const RuntimePlayerStatus& right) {
        if (left.resident != right.resident) return left.resident > right.resident;
        if (left.resident && left.resident_slot != right.resident_slot)
            return left.resident_slot < right.resident_slot;
        return left.account < right.account;
    });
    return statuses;
}

std::vector<RuntimeEvent> TownRuntime::recent_events() const {
    return std::vector<RuntimeEvent>(recent_events_.begin(), recent_events_.end());
}

void TownRuntime::record_event(std::string message) {
    recent_events_.push_back({next_event_sequence_++, wall_unix_seconds(), std::move(message)});
    while (recent_events_.size() > 12) recent_events_.pop_front();
}

std::uint8_t TownRuntime::house_light_mask() const {
    std::uint8_t mask = 0;
    for (const auto& entry : housing_.houses()) {
        const acnet::HouseState& house = entry.second;
        if (house.original_slot < acnet::kOriginalResidentSlots && house.main_light_on)
            mask |= static_cast<std::uint8_t>(1U << house.original_slot);
    }
    return mask;
}

TownRuntime::TownRuntime(TownRuntimeConfig config)
    : config_(std::move(config)),
      entities_(1),
      sessions_(acnet::SessionConfig{config_.capacity,
                                     acnet::kProtocolVersion,
                                     acnet::kProtocolVersion,
                                     config_.build_id,
                                     0,
                                     config_.connection_timeout_ms},
                0),
      movement_(),
      world_(&players_),
      /* shop zone/position/radius, museum zone, post office zone, mailbox zone
       * (0 = any: letters are read at the recipient's own mailbox), trade radius. */
      economy_(&world_, &players_, acnet::EconomyConfig{2, {30.0F, 0.0F, 30.0F}, 220.0F, 4, 3, 0, 120.0F}),
      encounters_(&players_, &world_),
      npcs_(&players_),
      zones_(&players_),
      housing_(&world_, &players_),
      deltas_(8192),
      clock_(config_.clock),
      persistence_(config_.data_directory),
      database_(config_.data_directory) {
    if (config_.tick_rate == 0) config_.tick_rate = 60;
    if (config_.snapshot_rate == 0 || config_.snapshot_rate > config_.tick_rate) config_.snapshot_rate = 15;
    if (config_.capacity == 0) config_.capacity = 1;
    movement_.set_collision_validator([this](acnet::ZoneId zone, const acnet::Vec3&, const acnet::Vec3& to) {
        if (zone != 1) return std::abs(to.x) <= 2000.0F && std::abs(to.z) <= 2000.0F;
        if (to.x < 640.0F || to.x >= 3840.0F || to.z < 640.0F || to.z >= 4480.0F) return false;
        const acnet::TileAddress address{zone,
                                         static_cast<std::int16_t>(std::floor(to.x / 40.0F)),
                                         static_cast<std::int16_t>(std::floor(to.z / 40.0F))};
        const acnet::TileState* tile = world_.tile(address);
        const bool structure = tile != nullptr && blocking_structure_item(tile->item);
        return tile != nullptr && !structure && tile->terrain != acnet::TerrainState::Tree &&
               tile->terrain != acnet::TerrainState::Stump && !tile->placed_furniture;
    });
}

bool TownRuntime::configure_zone_topology(std::string& error) {
    struct Building {
        std::uint16_t first_item;
        std::uint16_t last_item;
        std::uint16_t first_dummy;
        std::uint16_t last_dummy;
        acnet::ZoneId zone;
        std::uint32_t enter_door;
        std::uint32_t exit_door;
        acnet::Vec3 fallback;
    };
    const std::array<Building, 9> buildings{{
        {0x5804, 0x5807, 0xF0F7, 0xF0FA, 2, 10, 11, {1980.0F, 0.0F, 780.0F}},
        {0x5808, 0x5808, 0xF0FF, 0xF0FF, 3, 20, 21, {1900.0F, 0.0F, 780.0F}},
        {0x584A, 0x584A, 0xF120, 0xF120, 4, 30, 31, {2060.0F, 0.0F, 780.0F}},
        {0x584D, 0x584D, 0xF0FF, 0xF0FF, 5, 40, 41, {1980.0F, 0.0F, 860.0F}},
        {0x580C, 0x580C, 0xF101, 0xF101, 6, 50, 51, {1980.0F, 0.0F, 700.0F}},
        {0x5800, 0x5800, 0xF0F3, 0xF0F3, 100, 100, 200, {1820.0F, 0.0F, 860.0F}},
        {0x5801, 0x5801, 0xF0F4, 0xF0F4, 101, 101, 201, {1900.0F, 0.0F, 860.0F}},
        {0x5802, 0x5802, 0xF0F5, 0xF0F5, 102, 102, 202, {2060.0F, 0.0F, 860.0F}},
        {0x5803, 0x5803, 0xF0F6, 0xF0F6, 103, 103, 203, {2140.0F, 0.0F, 860.0F}},
    }};
    const auto tiles = world_.tiles_in_zone(1);
    for (const Building& building : buildings) {
        acnet::Vec3 outside = building.fallback;
        const auto found = std::find_if(tiles.begin(), tiles.end(), [&](const auto& entry) {
            return (entry.second.item >= building.first_item && entry.second.item <= building.last_item) ||
                   (entry.second.item >= building.first_dummy && entry.second.item <= building.last_dummy);
        });
        if (found != tiles.end()) {
            outside = {(static_cast<float>(found->first.x) + 0.5F) * 40.0F,
                       0.0F,
                       (static_cast<float>(found->first.z) + 0.5F) * 40.0F};
        }
        acnet::DoorDefinition enter;
        enter.id = building.enter_door;
        enter.source_zone = 1;
        enter.destination_zone = building.zone;
        enter.source_position = outside;
        enter.destination_position = {0.0F, 0.0F, 0.0F};
        enter.interaction_radius = found == tiles.end() ? 100.0F : 260.0F;
        acnet::DoorDefinition leave;
        leave.id = building.exit_door;
        leave.source_zone = building.zone;
        leave.destination_zone = 1;
        leave.source_position = {0.0F, 0.0F, 0.0F};
        leave.destination_position = outside;
        leave.interaction_radius = 500.0F;
        if (!zones_.set_door(enter) || !zones_.set_door(leave)) {
            error = "failed to configure authoritative building doors";
            return false;
        }
        if (building.zone == 2) {
            acnet::DoorDefinition compatibility = enter;
            compatibility.id = 1;
            if (!zones_.set_door(compatibility)) {
                error = "failed to configure compatibility shop door";
                return false;
            }
        }
    }
    if (!configure_island_topology(error)) return false;
    return true;
}

bool TownRuntime::install_island_tiles(const acnet::TownBootstrap& request, std::string& error) {
    /* Island tiles keep their global unit coordinates so the original client
     * helpers route a write to Save_t.island.fgblock without translation; only
     * the zone tells them apart from the town. */
    if (request.island_tiles.size() != acnet::kIslandTileCount) return true;
    if (!world_.tiles_in_zone(acnet::kIslandZone).empty()) return true;
    std::size_t index = 0;
    for (std::int16_t block = 0; block < acnet::kIslandBlockCount; ++block) {
        const std::int16_t block_x = static_cast<std::int16_t>(request.island_block_x[block]);
        for (std::int16_t unit_z = 0; unit_z < acnet::kBlockUnits; ++unit_z) {
            for (std::int16_t unit_x = 0; unit_x < acnet::kBlockUnits; ++unit_x, ++index) {
                const acnet::TownBootstrapTile& source = request.island_tiles[index];
                acnet::TileState tile;
                tile.item = source.item;
                tile.buried = source.buried;
                tile.terrain = terrain_for_bootstrap_item(source.item);
                if (!world_.set_tile({acnet::kIslandZone,
                                      static_cast<std::int16_t>(block_x * acnet::kBlockUnits + unit_x),
                                      static_cast<std::int16_t>(acnet::kIslandBlockZ * acnet::kBlockUnits + unit_z)},
                                     tile)) {
                    error = "failed to install the island foreground";
                    return false;
                }
            }
        }
    }
    /* One islander, shared by the town, standing in the island exterior. The
     * conversation lease keeps two visitors from talking to them at once. */
    acnet::NpcState islander;
    islander.entity = 1001;
    islander.zone = acnet::kIslandZone;
    const auto tiles = world_.tiles_in_zone(acnet::kIslandZone);
    if (!tiles.empty()) {
        islander.transform.position = {(static_cast<float>(tiles.front().first.x) + 8.5F) * 40.0F, 0.0F,
                                       (static_cast<float>(tiles.front().first.z) + 8.5F) * 40.0F};
    }
    if (npcs_.npc(islander.entity) == nullptr && !npcs_.add_npc(islander)) {
        error = "failed to initialize the islander";
        return false;
    }
    return true;
}

bool TownRuntime::configure_island_topology(std::string& error) {
    /* Island acres sit at block row kIslandBlockZ, outside the town's own tile
     * rectangle, so their centre is derived from whichever acre columns the
     * client reported. Until it has, the ferry still works: none of these
     * positions is validated -- request_transfer deliberately trusts the
     * client's scene transition and constrains only the source zone. */
    const auto island_tiles = world_.tiles_in_zone(acnet::kIslandZone);
    acnet::Vec3 island_landing{2600.0F, 0.0F, 5220.0F};
    if (!island_tiles.empty()) {
        std::int64_t sum_x = 0;
        std::int64_t sum_z = 0;
        for (const auto& entry : island_tiles) {
            sum_x += entry.first.x;
            sum_z += entry.first.z;
        }
        const auto count = static_cast<std::int64_t>(island_tiles.size());
        island_landing = {(static_cast<float>(sum_x) / static_cast<float>(count) + 0.5F) * 40.0F, 0.0F,
                          (static_cast<float>(sum_z) / static_cast<float>(count) + 0.5F) * 40.0F};
    }
    /* The dock is a town acre the server has no map of; the ferry leaves from
     * wherever the player boarded, so this is only a return hint. */
    const acnet::Vec3 dock{2600.0F, 0.0F, 4300.0F};
    const struct {
        std::uint32_t id;
        acnet::ZoneId source;
        acnet::ZoneId destination;
        acnet::Vec3 source_position;
        acnet::Vec3 destination_position;
    } doors[] = {
        {acnet::kFerryToIslandDoor, 1, acnet::kIslandZone, dock, island_landing},
        {acnet::kFerryToTownDoor, acnet::kIslandZone, 1, island_landing, dock},
        {acnet::kIslandCabinEnterDoor, acnet::kIslandZone, acnet::kIslandCabinZone, island_landing, {}},
        {acnet::kIslandCabinLeaveDoor, acnet::kIslandCabinZone, acnet::kIslandZone, {}, island_landing},
        {acnet::kIslandNpcHouseEnterDoor, acnet::kIslandZone, acnet::kIslandNpcHouseZone, island_landing, {}},
        {acnet::kIslandNpcHouseLeaveDoor, acnet::kIslandNpcHouseZone, acnet::kIslandZone, {}, island_landing},
    };
    for (const auto& entry : doors) {
        acnet::DoorDefinition door;
        door.id = entry.id;
        door.source_zone = entry.source;
        door.destination_zone = entry.destination;
        door.source_position = entry.source_position;
        door.destination_position = entry.destination_position;
        door.interaction_radius = 500.0F;
        if (!zones_.set_door(door)) {
            error = "failed to configure island doors";
            return false;
        }
    }
    return true;
}

bool TownRuntime::initialize(std::int64_t wall_seconds, std::string& error) {
    error.clear();
    if (initialized_ || config_.town_id == 0 || wall_seconds < 0) {
        error = "invalid runtime initialization";
        return false;
    }
    /* A blank invite key is a supported configuration: the town runs open, with
     * no invite proof demanded and no session encryption. Refusing to start was
     * worse - it stopped a host who wanted exactly that. The caller warns. */
    if (!persistence_.initialize(error)) return false;
    if (!database_.initialize(error)) return false;
    const auto checkpoint = persistence_.load_latest_checkpoint(error);
    if (!error.empty()) return false;
    acnet::ZoneState exterior;
    exterior.id = 1;
    exterior.kind = acnet::ZoneKind::Exterior;
    exterior.capacity = config_.capacity;
    if (!zones_.add_zone(exterior)) {
        error = "failed to create exterior zone";
        return false;
    }
    for (acnet::ZoneId id = 2; id <= 6; ++id) {
        acnet::ZoneState public_zone;
        public_zone.id = id;
        public_zone.kind = acnet::ZoneKind::PublicInterior;
        public_zone.capacity = config_.capacity;
        if (!zones_.add_zone(public_zone)) {
            error = "failed to initialize public interior zones";
            return false;
        }
    }
    acnet::NpcState shopkeeper;
    shopkeeper.entity = 1000;
    shopkeeper.zone = 2;
    shopkeeper.transform.position = {30.0F, 0.0F, 30.0F};
    acnet::ShopState shop;
    /* The town's rarity permutation is seeded from the town seed so a restart
     * before the first checkpoint reproduces the same shop, and the daily job
     * rolls the shelf from it. A checkpoint or journal replay overwrites both
     * further down, so this only ever establishes a brand-new town. */
    {
        std::mt19937_64 shop_random(config_.town_seed);
        const auto draw = [&shop_random]() -> std::uint64_t { return shop_random(); };
        acnet::shop_randomise_priorities(shop_stock_, draw);
        shop.stock = acnet::roll_shop_stock(shop_stock_, draw);
    }
    if (!npcs_.add_npc(shopkeeper)) {
        error = "failed to initialize public interior";
        return false;
    }
    for (std::uint8_t slot = 0; slot < acnet::kOriginalResidentSlots; ++slot) {
        acnet::ZoneState house_zone;
        house_zone.id = 100 + slot;
        house_zone.kind = acnet::ZoneKind::ResidentHouse;
        house_zone.capacity = config_.capacity;
        if (!zones_.add_zone(house_zone)) {
            error = "failed to initialize resident house zones";
            return false;
        }
    }
    /* The island and its two interiors. The original game reaches the island by
     * a Kapp'n ferry that never changes scene, so there is no door animation to
     * key a transfer off; the client notices its acre kind changed and asks for
     * the ferry door, and the exterior/island split is what makes that legal. */
    {
        acnet::ZoneState island;
        island.id = acnet::kIslandZone;
        island.kind = acnet::ZoneKind::Island;
        island.capacity = config_.capacity;
        acnet::ZoneState cabin;
        cabin.id = acnet::kIslandCabinZone;
        cabin.kind = acnet::ZoneKind::PublicInterior;
        cabin.capacity = config_.capacity;
        acnet::ZoneState islander_house;
        islander_house.id = acnet::kIslandNpcHouseZone;
        islander_house.kind = acnet::ZoneKind::PublicInterior;
        islander_house.capacity = config_.capacity;
        if (!zones_.add_zone(island) || !zones_.add_zone(cabin) || !zones_.add_zone(islander_house)) {
            error = "failed to initialize island zones";
            return false;
        }
    }
    /* Save_t.island.cottage belongs to the town, not to a resident: all four
     * original residents share one cabin. Registering it as a shared house
     * keeps furniture on the same journalled, revisioned transaction path as
     * every other room while letting whoever is standing in it decorate. */
    if (!housing_.register_shared_house(acnet::kIslandCabinHouseId, acnet::kIslandCabinZone)) {
        error = "failed to initialize the island cabin";
        return false;
    }
    /* The original outdoor foreground is five by six 16x16-acre blocks,
     * occupying global unit coordinates x=16..95 and z=16..111. */
    for (std::int16_t z = 16; z < 112; ++z) {
        for (std::int16_t x = 16; x < 96; ++x) {
            if (!world_.set_tile({1, x, z}, {})) {
                error = "failed to initialize exterior terrain";
                return false;
            }
        }
    }
    economy_.set_shop(shop);
    std::uint64_t checkpoint_sequence = 0;
    if (checkpoint.has_value()) {
        checkpoint_sequence = checkpoint->sequence;
        std::string state_error;
        if (!decode_state(checkpoint->payload, state_error) && !clock_.decode_state(checkpoint->payload)) {
            error = "latest checkpoint is invalid: " + state_error;
            return false;
        }
    } else if (!clock_.initialize(wall_seconds)) {
        error = "failed to initialize town clock";
        return false;
    }
    std::vector<std::uint8_t> latest_state;
    ReplayReport replay;
    if (!persistence_.replay(checkpoint_sequence,
                             [&](const JournalRecord& record) {
                                 if (record.type >= 100 && record.type < 200) latest_state = record.payload;
                                 return true;
                             }, replay, error)) return false;
    if (!latest_state.empty() && !decode_state(latest_state, error)) return false;
    if (!configure_zone_topology(error)) return false;
    constexpr std::int64_t hour_seconds = 60 * 60;
    constexpr std::int64_t day_seconds = 24 * hour_seconds;
    const std::int64_t town_time = clock_.state().town_unix_seconds;
    ScheduledJob hourly;
    hourly.name = "npc-schedules";
    hourly.interval_seconds = hour_seconds;
    hourly.next_due = ((town_time / hour_seconds) + 1) * hour_seconds;
    hourly.maximum_catchups = 48;
    if (!clock_.add_job(hourly, [this](const ScheduledJob&, std::int64_t due) {
            const std::uint16_t schedule = static_cast<std::uint16_t>((due / 3600) % 24);
            for (const auto& entry : npcs_.all_npcs()) {
                acnet::NpcState* npc = npcs_.npc(entry.first);
                if (npc == nullptr) continue;
                npc->schedule_state = schedule;
                npc->revision = advance_revision(npc->revision);
            }
            for (auto& connection : connections_) connection.second.has_exterior_chunk = false;
            ++metrics_.hourly_jobs;
            background_error_.clear();
            return commit_state(121, background_error_);
        })) {
        error = "failed to register hourly town jobs";
        return false;
    }
    ScheduledJob daily;
    daily.name = "daily-renewal";
    daily.interval_seconds = day_seconds;
    daily.next_due = ((town_time / day_seconds) + 1) * day_seconds;
    daily.maximum_catchups = 64;
    if (!clock_.add_job(daily, [this](const ScheduledJob&, std::int64_t) {
            /* Roll a fresh shelf rather than restocking yesterday's, which is
             * what the original store does at the day boundary. */
            acnet::ShopState shop = economy_.shop();
            shop.revision = advance_revision(shop.revision);
            std::uint64_t entropy = 0;
            if (!acnet::secure_random(reinterpret_cast<std::uint8_t*>(&entropy), sizeof(entropy)))
                entropy = static_cast<std::uint64_t>(shop.revision) * 6364136223846793005ULL;
            std::mt19937_64 shop_random(entropy);
            shop.stock = acnet::roll_shop_stock(shop_stock_,
                                                [&shop_random]() -> std::uint64_t { return shop_random(); });
            economy_.set_shop(shop);
            const auto tiles = world_.tiles_in_zone(1);
            for (const auto& entry : tiles) {
                if (entry.second.terrain != acnet::TerrainState::Planted) continue;
                acnet::TileState grown = entry.second;
                advance_planted_tile(grown);
                grown.revision = advance_revision(grown.revision);
                world_.set_tile(entry.first, grown);
                acnet::ReplicationDelta delta;
                delta.kind = acnet::ResourceKind::Tile;
                delta.zone = 1;
                delta.has_position = true;
                delta.position = {entry.first.x * 40.0F, 0.0F, entry.first.z * 40.0F};
                if (acnet::encode_tile_delta({entry.first, grown}, delta.payload)) deltas_.append(std::move(delta));
            }
            for (auto& connection : connections_) connection.second.has_exterior_chunk = false;
            ++metrics_.daily_jobs;
            background_error_.clear();
            if (!commit_state(122, background_error_)) return false;
            return database_.audit(0, "daily_renewal", "shops, growth, weather and events",
                                   wall_unix_seconds(), background_error_);
        })) {
        error = "failed to register daily town jobs";
        return false;
    }
    last_clock_minute_ = clock_.state().town_unix_seconds / 60;
    last_weather_ = clock_.state().weather;
    last_weather_intensity_ = clock_.state().weather_intensity;
    if (!socket_.open(config_.port, error, "0.0.0.0")) return false;
    if (!database_.audit(0, "server_started", "town=" + std::to_string(config_.town_id),
                         wall_seconds, error)) return false;
    initialized_ = true;
    record_event("Server started on UDP port " + std::to_string(socket_.bound_port()));
    if (!town_bootstrapped_) record_event("Waiting for the first resident to create the town world");
    std::cout << "{\"event\":\"server_started\",\"town\":" << config_.town_id
              << ",\"port\":" << socket_.bound_port() << ",\"clean_previous\":"
              << (persistence_.previous_shutdown_was_clean() ? "true" : "false") << "}\n";
    return true;
}

std::string TownRuntime::endpoint_key(const std::string& host, std::uint16_t port) {
    return host + ":" + std::to_string(port);
}

bool TownRuntime::allow_hello(const std::string& endpoint, std::uint64_t now_ms) {
    Connection::RateBucket& bucket = hello_rates_[endpoint];
    constexpr double rate = 2.0;
    constexpr double burst = 6.0;
    if (bucket.updated_ms == 0) bucket.tokens = burst;
    else if (now_ms >= bucket.updated_ms)
        bucket.tokens = std::min(burst, bucket.tokens + (now_ms - bucket.updated_ms) * rate / 1000.0);
    bucket.updated_ms = now_ms;
    if (bucket.tokens < 1.0) return false;
    bucket.tokens -= 1.0;
    return true;
}

bool TownRuntime::allow_message(Connection& connection, acnet::MessageType type, std::uint64_t now_ms) {
    double rate = 20.0;
    double burst = 40.0;
    if (type == acnet::MessageType::InputCommand) { rate = 60.0; burst = 120.0; }
    else if (type == acnet::MessageType::Ping || type == acnet::MessageType::Pong) { rate = 4.0; burst = 8.0; }
    else if (type == acnet::MessageType::AppearanceUpdate) {
        /* Each accepted one journals and re-baselines every connection, and a
         * baseline is up to two acres of tiles plus a 512-byte pattern. Changing
         * clothes is a once-in-a-while action, so this is deliberately the
         * tightest bucket on the server. */
        rate = 1.0; burst = 4.0;
    }
    else if (type == acnet::MessageType::WorldRequest || type == acnet::MessageType::InventoryRequest ||
             type == acnet::MessageType::TradeRequest || type == acnet::MessageType::FurnitureRequest ||
             type == acnet::MessageType::HouseUpdate ||
             type == acnet::MessageType::EncounterRequest || type == acnet::MessageType::ConversationRequest ||
             type == acnet::MessageType::ZoneTransferRequest || type == acnet::MessageType::ZoneReady) {
        rate = 10.0; burst = 20.0;
    }
    Connection::RateBucket& bucket = connection.rate_buckets[static_cast<std::uint16_t>(type)];
    if (bucket.updated_ms == 0) bucket.tokens = burst;
    else if (now_ms >= bucket.updated_ms)
        bucket.tokens = std::min(burst, bucket.tokens + (now_ms - bucket.updated_ms) * rate / 1000.0);
    bucket.updated_ms = now_ms;
    if (bucket.tokens < 1.0) return false;
    bucket.tokens -= 1.0;
    return true;
}

void TownRuntime::deactivate_player(acnet::AccountId account, acnet::Tick tick) {
    acnet::PlayerView* player = players_.by_account(account);
    if (player == nullptr) return;
    auto stored = accounts_.find(account);
    if (stored != accounts_.end()) {
        stored->second.zone = player->zone;
        stored->second.transform = player->transform;
    }
    npcs_.release_player(account);
    zones_.leave(account, tick);
    movement_.remove_player(account);
    players_.remove(account);
    door_transitions_.erase(account);
}

bool TownRuntime::handle_hello(const acnet::Datagram& datagram,
                               const acnet::DecodedPacket& packet,
                               std::uint64_t monotonic_ms,
                               std::string& error) {
    const auto add_town_identity = [this](acnet::ServerHello& reply, acnet::AccountId account) {
        reply.town_seed = config_.town_seed;
        reply.town_land_id = static_cast<std::uint16_t>(0x3000U | (config_.town_seed & 0xFFU));
        reply.town_name.fill(static_cast<std::uint8_t>(' '));
        std::copy_n(config_.town_name.begin(), std::min(config_.town_name.size(), reply.town_name.size()),
                    reply.town_name.begin());
        reply.town_initialized = town_bootstrapped_;
        const auto stored_account = accounts_.find(account);
        if (stored_account != accounts_.end() && stored_account->second.kind == acnet::PlayerKind::Resident)
            reply.resident_slot = stored_account->second.resident_slot;
    };
    if (!allow_hello(endpoint_key(datagram.host, datagram.port), monotonic_ms)) {
        ++metrics_.rejected_packets;
        record_event("Rate-limited connection attempt from " + endpoint_key(datagram.host, datagram.port));
        return true;
    }
    acnet::ClientHello hello;
    if (!acnet::decode(packet.payload, hello) || hello.town != config_.town_id) {
        ++metrics_.rejected_packets;
        return true;
    }
    if (!config_.invite_key.empty()) {
        const auto expected = acnet::invite_proof(hello, config_.invite_key);
        if (!acnet::constant_time_equal(expected.data(), hello.invite_proof.data(), expected.size())) {
            ++metrics_.rejected_packets;
            record_event("Rejected connection from " + endpoint_key(datagram.host, datagram.port) +
                         " (invalid invite key)");
            return true;
        }
    }
    bool banned = false;
    if (!database_.is_banned(hello.account, banned, error)) return false;
    if (banned) {
        ++metrics_.rejected_packets;
        record_event("Rejected banned account " + std::to_string(hello.account));
        return true;
    }
    /* A ClientHello may be retransmitted before its reliable ServerHello is
     * observed. Treat an exact same-endpoint/same-nonce retry as idempotent;
     * accepting it through SessionTable again would incorrectly report an
     * account conflict. */
    const auto endpoint = endpoints_.find(endpoint_key(datagram.host, datagram.port));
    if (endpoint != endpoints_.end()) {
        const auto connection_it = connections_.find(endpoint->second);
        acnet::Session* active = sessions_.find(endpoint->second);
        if (connection_it != connections_.end() && active != nullptr && active->account == hello.account &&
            active->town == hello.town && active->client_nonce == hello.client_nonce) {
            Connection& connection = connection_it->second;
            connection.last_received_ms = monotonic_ms;
            connection.reliability.receive(packet.header);
            acnet::ServerHello duplicate_reply;
            duplicate_reply.result = acnet::ResultCode::Ok;
            duplicate_reply.session = active->id;
            duplicate_reply.player_entity = active->player_entity;
            duplicate_reply.server_tick = movement_.current_tick();
            duplicate_reply.server_nonce = active->server_nonce;
            duplicate_reply.reconnect_token = active->reconnect_token;
            duplicate_reply.reconnect_token_size = acnet::kReconnectTokenBytes;
            add_town_identity(duplicate_reply, hello.account);
            if (!config_.invite_key.empty())
                duplicate_reply.server_proof = acnet::server_proof(duplicate_reply, hello.client_nonce,
                                                                    config_.invite_key);
            std::vector<std::uint8_t> duplicate_payload;
            if (!acnet::encode(duplicate_reply, duplicate_payload)) {
                error = "failed to serialize duplicate server hello";
                return false;
            }
            return send_payload(connection,
                                acnet::MessageType::ServerHello,
                                acnet::Channel::Control,
                                duplicate_payload,
                                monotonic_ms,
                                error);
        }
        ++metrics_.rejected_packets;
        return true;
    }
    const auto stored = accounts_.find(hello.account);
    const bool known_account = stored != accounts_.end();
    const bool reconnecting = known_account;
    acnet::EntityId entity = known_account ? stored->second.entity
                                           : entities_.add(static_cast<std::uintptr_t>(hello.account), 0, 1, 1);
    acnet::ServerHello response = sessions_.accept(hello, entity, movement_.current_tick(), monotonic_ms);
    if (response.result == acnet::ResultCode::Ok && players_.by_account(hello.account) == nullptr) {
        if (!known_account) {
            AccountState account;
            account.entity = entity;
            std::array<bool, acnet::kOriginalResidentSlots> occupied{};
            for (const auto& existing : accounts_) {
                if (existing.second.kind == acnet::PlayerKind::Resident &&
                    existing.second.resident_slot < occupied.size()) occupied[existing.second.resident_slot] = true;
            }
            const auto free_resident = std::find(occupied.begin(), occupied.end(), false);
            account.kind = free_resident != occupied.end() ? acnet::PlayerKind::Resident
                                                           : acnet::PlayerKind::Visitor;
            account.zone = 1;
            account.appearance = default_appearance(hello.account);
            if (account.kind == acnet::PlayerKind::Resident) {
                account.resident_slot = static_cast<std::uint8_t>(std::distance(occupied.begin(), free_resident));
                account.transform.position = resident_spawn(account.resident_slot);
                if (!housing_.register_resident(account.resident_slot, hello.account, 100 + account.resident_slot)) {
                    error = "failed to allocate resident house";
                    return false;
                }
            } else account.transform.position = resident_spawn(0);
            accounts_[hello.account] = account;
            acnet::InventoryState inventory;
            inventory.bells = 1000;
            /* A rod in the pocket, hands empty: holding it is a transaction the
             * player makes, not a state they start in. */
            inventory.slots[0].item = 0x2203;
            if (!world_.register_inventory(hello.account, inventory) ||
                !economy_.register_account(hello.account, {})) {
                error = "failed to create persistent account";
                return false;
            }
            if (!database_.record_account(hello.account, wall_unix_seconds(), error)) return false;
        }
        bool migrated_spawn = false;
        AccountState& account = accounts_.at(hello.account);
        if (known_account && account.kind == acnet::PlayerKind::Resident && legacy_train_spawn(account.transform)) {
            account.transform.position = resident_spawn(account.resident_slot);
            migrated_spawn = true;
            record_event("Migrated resident account " + std::to_string(hello.account) +
                         " from the legacy train-track spawn");
        }
        acnet::PlayerView player;
        player.account = hello.account;
        player.entity = account.entity;
        player.zone = account.zone;
        player.kind = account.kind;
        player.transform = account.transform;
        player.appearance = account.appearance;
        player.pattern = account.pattern;
        /* Whatever this account was holding when it last logged out: the hand
         * survives a session because the inventory does. */
        if (const acnet::InventoryState* stored = world_.inventory(hello.account)) {
            player.presentation.equipped_item = stored->equipped.item;
        }
        if (!players_.upsert(player) ||
            !movement_.add_player(player.account, player.entity, player.zone, player.transform) ||
            !zones_.join(player.account, player.zone, player.transform.position, movement_.current_tick())) {
            response.result = acnet::ResultCode::InternalError;
            sessions_.close(response.session);
            players_.remove(player.account);
            movement_.remove_player(player.account);
        } else if ((!known_account || migrated_spawn) && !commit_state(100, error)) {
            return false;
        }
    }

    add_town_identity(response, hello.account);

    Connection connection;
    if (response.result == acnet::ResultCode::Ok) {
        const auto old = connections_.find(response.session);
        if (old != connections_.end()) endpoints_.erase(endpoint_key(old->second.host, old->second.port));
        connection.session = response.session;
        connection.account = hello.account;
        connection.host = datagram.host;
        connection.port = datagram.port;
        connection.last_received_ms = monotonic_ms;
        connection.reliability.receive(packet.header);
        if (!config_.invite_key.empty()) {
            connection.session_keys = acnet::derive_session_keys(hello, response, config_.invite_key);
            connection.encryption_active = true;
        }
        if (reconnecting) ++metrics_.reconnects;
    }

    if (!config_.invite_key.empty())
        response.server_proof = acnet::server_proof(response, hello.client_nonce, config_.invite_key);
    std::vector<std::uint8_t> payload;
    if (!acnet::encode(response, payload)) {
        error = "failed to serialize server hello";
        return false;
    }
    acnet::PacketHeader header;
    if (response.result == acnet::ResultCode::Ok) {
        header = connection.reliability.make_header(acnet::MessageType::ServerHello,
                                                    acnet::Channel::Control,
                                                    response.session);
    } else {
        header.message_type = acnet::MessageType::ServerHello;
        header.channel = acnet::Channel::Control;
        header.flags = acnet::PacketReliable;
        header.sequence = 1;
        header.acknowledged_sequence = packet.header.sequence;
    }
    std::vector<std::uint8_t> bytes;
    if (!acnet::encode_packet(header, payload, bytes, error) ||
        !socket_.send(datagram.host, datagram.port, bytes, error)) return false;
    ++metrics_.packets_sent;
    if (response.result == acnet::ResultCode::Ok) {
        connection.reliability.track_sent(header, bytes, monotonic_ms);
        endpoints_[endpoint_key(connection.host, connection.port)] = connection.session;
        connections_[connection.session] = std::move(connection);
        const AccountState& connected_account = accounts_.at(hello.account);
        const std::string role = connected_account.kind == acnet::PlayerKind::Resident
            ? "resident slot " + std::to_string(static_cast<unsigned>(connected_account.resident_slot) + 1U)
            : "visitor";
        record_event(std::string(reconnecting ? "Reconnected " : "Connected ") + role + " account " +
                     std::to_string(hello.account) + " from " + endpoint_key(datagram.host, datagram.port));
        if (!database_.record_account(hello.account, wall_unix_seconds(), error) ||
            !database_.record_session(response.session, hello.account, true, wall_unix_seconds(), error)) return false;
        /* Appearance is baseline-owned so transform snapshots stay under the
         * MTU. Refresh every viewer when a player joins; otherwise existing
         * clients would know the new transform but not the new presentation. */
        for (auto& connected : connections_) {
            if (!send_baseline(connected.second, monotonic_ms, error)) return false;
        }
    }
    return true;
}

bool TownRuntime::send_payload(Connection& connection,
                               acnet::MessageType type,
                               acnet::Channel channel,
                               const std::vector<std::uint8_t>& payload,
                               std::uint64_t monotonic_ms,
                               std::string& error) {
    if (payload.size() > acnet::kMaxPlaintextPayloadBytes) {
        std::vector<std::vector<std::uint8_t>> fragments;
        if (!acnet::split_fragments(payload, connection.next_transfer_id++, fragments)) {
            error = "failed to fragment server payload";
            return false;
        }
        if (connection.next_transfer_id == 0) connection.next_transfer_id = 1;
        for (const auto& fragment : fragments) {
            acnet::PacketHeader header = connection.reliability.make_header(type, acnet::Channel::Bulk,
                                                                            connection.session);
            header.flags = static_cast<std::uint8_t>(header.flags | acnet::PacketFragment);
            std::vector<std::uint8_t> wire_payload = fragment;
            if (connection.encryption_active && type != acnet::MessageType::ServerHello) {
                header.flags = static_cast<std::uint8_t>(header.flags | acnet::PacketEncrypted);
                if (!acnet::seal_payload(header, connection.session_keys.server_to_client,
                                         fragment, wire_payload)) {
                    error = "failed to encrypt server fragment";
                    return false;
                }
            }
            std::vector<std::uint8_t> bytes;
            if (!acnet::encode_packet(header, wire_payload, bytes, error) ||
                !socket_.send(connection.host, connection.port, bytes, error)) return false;
            connection.reliability.track_sent(header, bytes, monotonic_ms);
            ++metrics_.packets_sent;
        }
    } else {
        acnet::PacketHeader header = connection.reliability.make_header(type, channel, connection.session);
        std::vector<std::uint8_t> wire_payload = payload;
        if (connection.encryption_active && type != acnet::MessageType::ServerHello) {
            header.flags = static_cast<std::uint8_t>(header.flags | acnet::PacketEncrypted);
            if (!acnet::seal_payload(header, connection.session_keys.server_to_client,
                                     payload, wire_payload)) {
                error = "failed to encrypt server payload";
                return false;
            }
        }
        std::vector<std::uint8_t> bytes;
        if (!acnet::encode_packet(header, wire_payload, bytes, error) ||
            !socket_.send(connection.host, connection.port, bytes, error)) return false;
        connection.reliability.track_sent(header, bytes, monotonic_ms);
        ++metrics_.packets_sent;
    }
    return true;
}

bool TownRuntime::send_ack(Connection& connection, acnet::Channel channel, std::string& error) {
    acnet::PacketHeader header = connection.reliability.make_header(acnet::MessageType::Pong, channel,
                                                                    connection.session);
    header.flags = acnet::PacketAckOnly;
    std::vector<std::uint8_t> payload;
    if (connection.encryption_active) {
        header.flags = static_cast<std::uint8_t>(header.flags | acnet::PacketEncrypted);
        if (!acnet::seal_payload(header, connection.session_keys.server_to_client, {}, payload)) {
            error = "failed to encrypt acknowledgement";
            return false;
        }
    }
    std::vector<std::uint8_t> bytes;
    if (!acnet::encode_packet(header, payload, bytes, error) ||
        !socket_.send(connection.host, connection.port, bytes, error)) return false;
    ++metrics_.packets_sent;
    return true;
}

/* Baselines only reach a client on join and zone transfer, so the count would
 * otherwise be stale for most of a session. Published from the tick rather
 * than from the join/leave paths so timeouts and kicks are covered too. */
void TownRuntime::publish_population_change() {
    const acnet::TownOccupancy occupancy = current_occupancy();
    if (occupancy.population == last_published_population_ &&
        occupancy.capacity == last_published_capacity_) {
        return;
    }
    acnet::ReplicationDelta delta;
    delta.kind = acnet::ResourceKind::Town;
    delta.zone = 0; /* town-wide, not scoped to the viewer's zone */
    delta.reliable = true;
    if (!acnet::encode_town_delta(occupancy, delta.payload)) return;
    deltas_.append(std::move(delta));
    last_published_population_ = occupancy.population;
    last_published_capacity_ = occupancy.capacity;
}

/* Zone-scoped, so it reaches exactly the viewers whose interest set already
 * contains this player and who therefore have a remote actor to apply it to. */
void TownRuntime::publish_presentation(const acnet::PlayerView& player) {
    acnet::PlayerPresentationDelta presentation;
    presentation.account = player.account;
    presentation.entity = player.entity;
    presentation.presentation = player.presentation;
    acnet::ReplicationDelta delta;
    delta.kind = acnet::ResourceKind::Player;
    delta.zone = player.zone;
    delta.entity = player.entity;
    delta.reliable = true;
    if (!acnet::encode_player_delta(presentation, delta.payload)) return;
    deltas_.append(std::move(delta));
}

/* The hand is inventory state, so the presentation mirror is refreshed from the
 * authority rather than tracked alongside it. */
void TownRuntime::refresh_equipped_item(acnet::AccountId account) {
    acnet::PlayerView* player = players_.by_account(account);
    const acnet::InventoryState* inventory = world_.inventory(account);
    if (player == nullptr || inventory == nullptr ||
        player->presentation.equipped_item == inventory->equipped.item) {
        return;
    }
    player->presentation.equipped_item = inventory->equipped.item;
    publish_presentation(*player);
}

/* Resident identity changes rarely -- a slot is claimed on a first login, and a
 * GCI import can rewrite all four at once -- but a client that never leaves the
 * town zone would otherwise keep its join-time roster forever. Polled from the
 * tick like the population count so every mutation path is covered without
 * each one having to remember to publish. */
void TownRuntime::publish_resident_change() {
    const acnet::ResidentRoster roster = current_roster();
    if (roster_published_ && roster == last_published_roster_) return;
    acnet::ReplicationDelta delta;
    delta.kind = acnet::ResourceKind::Resident;
    delta.zone = 0; /* town-wide, not scoped to the viewer's zone */
    delta.reliable = true;
    if (!acnet::encode_resident_delta(roster, delta.payload)) return;
    deltas_.append(std::move(delta));
    last_published_roster_ = roster;
    roster_published_ = true;
}

bool TownRuntime::publish_mail_change(const acnet::MailRecord& record, bool removed, std::string& error) {
    const acnet::MailboxState* mailbox = economy_.mailbox(record.recipient);
    if (mailbox == nullptr) { error = "mailbox is missing for the recipient"; return false; }
    acnet::MailDelta mail;
    mail.account = record.recipient;
    mail.mailbox_revision = mailbox->revision;
    mail.removed = removed;
    mail.record = record;
    acnet::ReplicationDelta delta;
    delta.kind = acnet::ResourceKind::Mail;
    delta.zone = 0; /* the addressee gets it wherever they are standing */
    delta.target_account = record.recipient;
    delta.reliable = true;
    if (!acnet::encode_mail_delta(mail, delta.payload)) { error = "failed to serialize mailbox delta"; return false; }
    deltas_.append(std::move(delta));
    return true;
}

acnet::TownOccupancy TownRuntime::current_occupancy() const {
    acnet::TownOccupancy occupancy;
    occupancy.population = static_cast<std::uint8_t>(std::min<std::size_t>(connected_clients(), 255U));
    /* Capacity stays >= population so the codec invariant holds whatever the
     * operator configured. */
    occupancy.capacity = static_cast<std::uint8_t>(
        std::clamp<std::size_t>(config_.capacity, occupancy.population == 0 ? 1U : occupancy.population, 255U));
    return occupancy;
}

/* Ownership of the four original houses, read from the persistent account table
 * rather than the connected-player directory: a resident who is logged out
 * still owns their house, and the client has no other way to learn that. */
acnet::ResidentRoster TownRuntime::current_roster() const {
    acnet::ResidentRoster roster;
    for (const auto& entry : accounts_) {
        const AccountState& state = entry.second;
        if (state.kind != acnet::PlayerKind::Resident ||
            state.resident_slot >= roster.slots.size()) continue;
        acnet::ResidentIdentity& resident = roster.slots[state.resident_slot];
        resident.account = entry.first;
        resident.name = state.appearance.name;
        /* The wire rejects a gender the game cannot represent rather than
         * letting it reach mMP_ResidentInfo_c.sex. */
        resident.gender = state.appearance.gender > 2 ? 0 : state.appearance.gender;
        resident.occupied = true;
    }
    return roster;
}

bool TownRuntime::send_baseline(Connection& connection,
                                std::uint64_t monotonic_ms,
                                std::string& error) {
    const acnet::PlayerView* viewer = players_.by_account(connection.account);
    if (viewer == nullptr) {
        error = "baseline viewer is missing";
        return false;
    }
    const acnet::Revision revision = std::max<acnet::Revision>(1, deltas_.current_revision());
    const ClockState& clock = clock_.state();
    acnet::ZoneBaseline baseline = acnet::build_baseline(
        viewer->zone, movement_.current_tick(), revision, clock.town_unix_seconds,
        static_cast<std::uint8_t>(clock.weather), clock.weather_intensity, world_, players_, npcs_);
    const acnet::InventoryState* inventory = world_.inventory(connection.account);
    const acnet::AccountLedger* ledger = economy_.ledger(connection.account);
    if (inventory == nullptr || ledger == nullptr) {
        error = "baseline account state is missing";
        return false;
    }
    baseline.inventory = *inventory;
    baseline.ledger = *ledger;
    const acnet::MailboxState* mailbox = economy_.mailbox(connection.account);
    if (mailbox != nullptr) baseline.mailbox = *mailbox;
    baseline.mail = economy_.mail_for(connection.account);
    baseline.shop = economy_.shop();
    /* Town-wide occupancy, which the viewer's interest set cannot show. */
    const acnet::TownOccupancy occupancy = current_occupancy();
    baseline.town_population = occupancy.population;
    baseline.town_capacity = occupancy.capacity;
    baseline.residents = current_roster();
    baseline.house_light_mask = house_light_mask();
    if (viewer->zone >= 100 && viewer->zone < 100 + acnet::kOriginalResidentSlots) {
        for (const auto& entry : housing_.houses()) {
            if (entry.second.zone != viewer->zone) continue;
            baseline.has_house = true;
            baseline.house = entry.second;
            break;
        }
    } else if (const acnet::HouseState* shared = housing_.shared_house_in(viewer->zone)) {
        /* The island cabin. Ownerless, so every occupant gets it in their
         * baseline and any of them may redecorate it. */
        baseline.has_house = true;
        baseline.house = *shared;
    }
    if (viewer->zone == 1) {
        const std::int16_t center_x = static_cast<std::int16_t>(viewer->transform.position.x / 40.0F);
        const std::int16_t center_z = static_cast<std::int16_t>(viewer->transform.position.z / 40.0F);
        std::int16_t start_x = static_cast<std::int16_t>(center_x - 8);
        std::int16_t start_z = static_cast<std::int16_t>(center_z - 8);
        if (!baseline.tiles.empty()) {
            auto x_bounds = std::minmax_element(baseline.tiles.begin(), baseline.tiles.end(),
                [](const auto& left, const auto& right) { return left.first.x < right.first.x; });
            auto z_bounds = std::minmax_element(baseline.tiles.begin(), baseline.tiles.end(),
                [](const auto& left, const auto& right) { return left.first.z < right.first.z; });
            const std::int16_t min_x = x_bounds.first->first.x;
            const std::int16_t max_x = x_bounds.second->first.x;
            const std::int16_t min_z = z_bounds.first->first.z;
            const std::int16_t max_z = z_bounds.second->first.z;
            start_x = std::clamp(start_x, min_x,
                                 static_cast<std::int16_t>(std::max<int>(min_x, max_x - 15)));
            start_z = std::clamp(start_z, min_z,
                                 static_cast<std::int16_t>(std::max<int>(min_z, max_z - 15)));
        }
        baseline.tiles.erase(std::remove_if(baseline.tiles.begin(), baseline.tiles.end(),
            [&](const auto& tile) {
                return tile.first.x < start_x || tile.first.x >= start_x + 16 ||
                       tile.first.z < start_z || tile.first.z >= start_z + 16;
            }), baseline.tiles.end());
        connection.baseline_start_x = start_x;
        connection.baseline_start_z = start_z;
        connection.has_exterior_chunk = true;
    } else {
        connection.has_exterior_chunk = false;
    }
    std::vector<std::uint8_t> payload;
    if (!acnet::encode_baseline(baseline, payload)) {
        error = "failed to serialize zone baseline";
        return false;
    }
    if (!send_payload(connection, acnet::MessageType::Baseline, acnet::Channel::Bulk,
                      payload, monotonic_ms, error)) return false;
    connection.last_delta_revision = deltas_.current_revision();
    return true;
}

bool TownRuntime::refresh_interest_chunk(Connection& connection,
                                         std::uint64_t monotonic_ms,
                                         std::string& error) {
    const acnet::PlayerView* viewer = players_.by_account(connection.account);
    if (viewer == nullptr || viewer->zone != 1) return true;
    const std::int16_t x = static_cast<std::int16_t>(viewer->transform.position.x / 40.0F);
    const std::int16_t z = static_cast<std::int16_t>(viewer->transform.position.z / 40.0F);
    if (!connection.has_exterior_chunk || x < connection.baseline_start_x + 2 ||
        x >= connection.baseline_start_x + 14 || z < connection.baseline_start_z + 2 ||
        z >= connection.baseline_start_z + 14) return send_baseline(connection, monotonic_ms, error);
    return true;
}

bool TownRuntime::send_deltas(Connection& connection,
                              std::uint64_t monotonic_ms,
                              std::string& error) {
    const acnet::PlayerView* viewer = players_.by_account(connection.account);
    if (viewer == nullptr) return true;
    acnet::InterestContext interest;
    interest.account = connection.account;
    interest.zone = viewer->zone;
    interest.position = viewer->transform.position;
    interest.exterior = viewer->zone == 1;
    const acnet::DeltaQueryResult query = deltas_.since(connection.last_delta_revision, interest, 128);
    if (query.requires_baseline) return send_baseline(connection, monotonic_ms, error);
    if (query.deltas.empty()) {
        connection.last_delta_revision = query.newest_revision;
        return true;
    }
    std::vector<std::uint8_t> payload;
    if (!acnet::encode_deltas(query.deltas, payload) ||
        !send_payload(connection, acnet::MessageType::ReplicationDeltas, acnet::Channel::Events,
                      payload, monotonic_ms, error)) return false;
    connection.last_delta_revision = query.deltas.back().revision;
    return true;
}

bool TownRuntime::dispatch(Connection& connection,
                           acnet::DecodedPacket packet,
                           std::uint64_t monotonic_ms,
                           std::string& error) {
    if ((packet.header.flags & acnet::PacketFragment) != 0) {
        acnet::Fragment fragment;
        if (!acnet::decode_fragment(packet.payload, fragment)) {
            ++metrics_.malformed_packets;
            return true;
        }
        const auto complete = connection.fragments.accept(fragment, monotonic_ms);
        if (!complete.has_value()) return true;
        packet.payload = *complete;
    }
    switch (packet.header.message_type) {
        case acnet::MessageType::TownBootstrap: {
            acnet::TownBootstrap request;
            acnet::TownBootstrapResult result;
            const auto account = accounts_.find(connection.account);
            if (!acnet::decode(packet.payload, request)) {
                ++metrics_.malformed_packets;
                return true;
            }
            std::array<std::uint8_t, 8> expected_name;
            expected_name.fill(static_cast<std::uint8_t>(' '));
            std::copy_n(config_.town_name.begin(), std::min(config_.town_name.size(), expected_name.size()),
                        expected_name.begin());
            const std::uint16_t expected_land_id =
                static_cast<std::uint16_t>(0x3000U | (config_.town_seed & 0xFFU));
            if (account == accounts_.end() || account->second.kind != acnet::PlayerKind::Resident ||
                request.town_seed != config_.town_seed || request.land_id != expected_land_id ||
                request.town_name != expected_name) {
                result.code = acnet::ResultCode::Unauthorized;
            } else {
                AccountState& state = account->second;
                request.appearance.revision = state.appearance.revision == std::numeric_limits<acnet::Revision>::max()
                                                  ? 1
                                                  : state.appearance.revision + 1;
                state.appearance = request.appearance;
                state.pattern = request.pattern;
                if (acnet::PlayerView* player = players_.by_account(connection.account)) {
                    player->appearance = request.appearance;
                    player->pattern = request.pattern;
                }
                const bool initialized_now = !town_bootstrapped_;
                if (initialized_now) {
                    std::size_t index = 0;
                    for (std::int16_t block_z = 0; block_z < 6; ++block_z) {
                        for (std::int16_t block_x = 0; block_x < 5; ++block_x) {
                            for (std::int16_t unit_z = 0; unit_z < 16; ++unit_z) {
                                for (std::int16_t unit_x = 0; unit_x < 16; ++unit_x, ++index) {
                                    const acnet::TownBootstrapTile& source = request.tiles[index];
                                    acnet::TileState tile;
                                    tile.item = source.item;
                                    tile.buried = source.buried;
                                    tile.terrain = terrain_for_bootstrap_item(source.item);
                                    if (!world_.set_tile({1,
                                                         static_cast<std::int16_t>((block_x + 1) * 16 + unit_x),
                                                         static_cast<std::int16_t>((block_z + 1) * 16 + unit_z)},
                                                         tile)) {
                                        error = "failed to install initial town foreground";
                                        return false;
                                    }
                                }
                            }
                        }
                    }
                    town_bootstrapped_ = true;
                    if (!install_island_tiles(request, error) || !configure_zone_topology(error) ||
                        !commit_state(111, error) ||
                        !database_.audit(connection.account, "town_bootstrap", config_.town_name,
                                         wall_unix_seconds(), error)) return false;
                } else {
                    /* A town created before the island had a zone reaches this
                     * branch with an empty island. The next login that can read
                     * the acre layout fills it in; install_island_tiles is a
                     * no-op once the island already has tiles. */
                    const bool island_installed = world_.tiles_in_zone(acnet::kIslandZone).empty() &&
                                                  !request.island_tiles.empty();
                    if (!install_island_tiles(request, error)) return false;
                    if (island_installed && !configure_zone_topology(error)) return false;
                    if (!commit_state(112, error)) return false;
                    if (island_installed)
                        record_event("Island terrain adopted from resident account " +
                                     std::to_string(connection.account));
                }
                result.code = acnet::ResultCode::Ok;
                result.revision = std::max<acnet::Revision>(1, deltas_.current_revision());
                result.initialized = true;
                if (initialized_now)
                    record_event("Town world created by resident account " + std::to_string(connection.account));
                else
                    record_event("Character appearance saved for account " + std::to_string(connection.account));
            }
            std::vector<std::uint8_t> response;
            if (!acnet::encode(result, response) ||
                !send_payload(connection, acnet::MessageType::TownBootstrapResult,
                              acnet::Channel::Control, response, monotonic_ms, error)) return false;
            if (result.code == acnet::ResultCode::Ok) {
                for (auto& connected : connections_) {
                    connected.second.has_exterior_chunk = false;
                    if (!send_baseline(connected.second, monotonic_ms, error)) return false;
                }
            }
            return true;
        }
        case acnet::MessageType::AppearanceUpdate: {
            acnet::AppearanceUpdate request;
            acnet::AppearanceResult result;
            const auto account = accounts_.find(connection.account);
            if (!acnet::decode(packet.payload, request)) {
                ++metrics_.malformed_packets;
                return true;
            }
            /* A re-baseline is only warranted when something visible actually
             * moved. Clients resend their appearance whenever any captured
             * field changes, and an unchanged resend used to cost every
             * connection a full baseline. */
            bool appearance_changed = false;
            if (account == accounts_.end()) {
                result.code = acnet::ResultCode::Unauthorized;
            } else {
                AccountState& state = account->second;
                const acnet::Revision revision = state.appearance.revision;
                appearance_changed = state.appearance.name != request.appearance.name ||
                                     state.appearance.gender != request.appearance.gender ||
                                     state.appearance.face != request.appearance.face ||
                                     state.appearance.clothing != request.appearance.clothing ||
                                     state.appearance.clothing_index != request.appearance.clothing_index ||
                                     state.pattern.present != request.pattern.present ||
                                     state.pattern.palette != request.pattern.palette ||
                                     (request.pattern.present && state.pattern.texture != request.pattern.texture);
                result.code = acnet::ResultCode::Ok;
                if (!appearance_changed) {
                    /* Confirm against the revision the sender already has so it
                     * stops resending, but touch neither storage nor the wire. */
                    result.revision = revision;
                } else {
                    request.appearance.revision =
                        revision == std::numeric_limits<acnet::Revision>::max() ? 1 : revision + 1;
                    state.appearance = request.appearance;
                    state.pattern = request.pattern;
                    if (acnet::PlayerView* player = players_.by_account(connection.account)) {
                        player->appearance = request.appearance;
                        player->pattern = request.pattern;
                    }
                    if (!commit_state(113, error)) return false;
                    result.revision = request.appearance.revision;
                    record_event("Appearance updated for account " + std::to_string(connection.account));
                }
            }
            std::vector<std::uint8_t> response;
            if (!acnet::encode(result, response) ||
                !send_payload(connection, acnet::MessageType::AppearanceResult,
                              acnet::Channel::Transactions, response, monotonic_ms, error)) return false;
            if (result.code == acnet::ResultCode::Ok && appearance_changed) {
                for (auto& connected : connections_) {
                    if (!send_baseline(connected.second, monotonic_ms, error)) return false;
                }
            }
            return true;
        }
        case acnet::MessageType::InputCommand: {
            acnet::InputCommand input;
            if (!acnet::decode(packet.payload, input) ||
                movement_.submit(connection.account, input) != acnet::ResultCode::Ok) ++metrics_.rejected_packets;
            return true;
        }
        case acnet::MessageType::Ping:
            return send_payload(connection, acnet::MessageType::Pong, acnet::Channel::Control,
                                packet.payload, monotonic_ms, error);
        case acnet::MessageType::WorldRequest: {
            acnet::WorldOperation request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            request.account = connection.account;
            const acnet::WorldResult result = world_.apply(request, movement_.current_tick());
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(result, payload)) { error = "failed to serialize world result"; return false; }
            if (result.code == acnet::ResultCode::Ok && !result.replayed) {
                if (!commit_transaction(connection.account,
                                        static_cast<std::uint16_t>(acnet::MessageType::WorldRequest),
                                        result.code, error)) return false;
                acnet::ReplicationDelta delta;
                delta.kind = acnet::ResourceKind::Tile;
                delta.zone = result.tile.zone;
                delta.has_position = true;
                delta.position = {result.tile.x * 40.0F, 0.0F, result.tile.z * 40.0F};
                const acnet::TileState* tile = world_.tile(result.tile);
                /* Naming the actor and the operation is what lets a viewer
                 * animate the change instead of popping the new state in: a
                 * drop arcs out of that player's hand, everything else does
                 * not. */
                if (tile == nullptr ||
                    !acnet::encode_tile_delta({result.tile, *tile, connection.account,
                                               acnet::tile_change_cause(request.type)}, delta.payload)) {
                    error = "failed to serialize tile delta";
                    return false;
                }
                deltas_.append(std::move(delta));
            }
            return send_payload(connection, acnet::MessageType::WorldResult, acnet::Channel::Transactions,
                                payload, monotonic_ms, error);
        }
        case acnet::MessageType::InventoryRequest: {
            acnet::EconomyRequest request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            request.account = connection.account;
            /* A discard destroys the letter, so its contents are captured
             * before the transaction in order to describe the removal. */
            acnet::MailRecord discarded;
            if (request.type == acnet::EconomyOpType::DiscardMail) {
                const acnet::MailRecord* letter = economy_.mail(request.mail_id);
                if (letter != nullptr) discarded = *letter;
            }
            const acnet::EconomyResult result = economy_.apply(request);
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(result, payload)) { error = "failed to serialize economy result"; return false; }
            if (result.code == acnet::ResultCode::Ok && !result.replayed) {
                if (!commit_transaction(connection.account,
                                        static_cast<std::uint16_t>(acnet::MessageType::InventoryRequest),
                                        result.code, error)) return false;
                /* A letter changes a mailbox the sender does not own, so the
                 * recipient is told directly rather than waiting for their next
                 * baseline; a claim confirms the letter left the claimer's. */
                if (request.type == acnet::EconomyOpType::AttachMail ||
                    request.type == acnet::EconomyOpType::TakeMail ||
                    request.type == acnet::EconomyOpType::ClaimMail) {
                    /* Delivering, taking, and claiming all leave the letter in
                     * existence, so the recipient is sent its current state. */
                    const acnet::MailRecord* letter = economy_.mail(result.mail_id);
                    if (letter == nullptr) { error = "letter is missing after a mail transaction"; return false; }
                    if (!publish_mail_change(*letter, false, error)) return false;
                } else if (request.type == acnet::EconomyOpType::DiscardMail) {
                    if (!publish_mail_change(discarded, true, error)) return false;
                }
                /* Holding, and only holding, changes what onlookers see in this
                 * player's hand. */
                if (request.type == acnet::EconomyOpType::HoldItem) refresh_equipped_item(connection.account);
                acnet::ReplicationDelta delta;
                delta.kind = request.type == acnet::EconomyOpType::Donate ? acnet::ResourceKind::Event
                                                                          : acnet::ResourceKind::Shop;
                delta.zone = 0;
                delta.target_account = request.type == acnet::EconomyOpType::Buy ||
                                               request.type == acnet::EconomyOpType::Sell
                                           ? 0 : connection.account;
                delta.payload = payload;
                deltas_.append(std::move(delta));
            }
            return send_payload(connection, acnet::MessageType::InventoryResult, acnet::Channel::Transactions,
                                payload, monotonic_ms, error);
        }
        case acnet::MessageType::TradeRequest: {
            acnet::TradeRequest request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            request.account = connection.account;
            acnet::TradeResult result;
            switch (request.action) {
                case acnet::TradeAction::Create:
                    result = economy_.create_trade(request.trade_id, request.account, request.other_account);
                    break;
                case acnet::TradeAction::UpdateOffer:
                    result = economy_.update_trade_offer(request.trade_id, request.account,
                                                         request.expected_trade_revision, request.slots);
                    break;
                case acnet::TradeAction::Confirm:
                    result = economy_.confirm_trade(request.trade_id, request.account,
                                                    request.expected_trade_revision);
                    break;
                case acnet::TradeAction::Cancel:
                    result.trade_id = request.trade_id;
                    result.code = economy_.cancel_trade(request.trade_id) ? acnet::ResultCode::Ok
                                                                         : acnet::ResultCode::NotFound;
                    break;
            }
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(result, payload)) { error = "failed to serialize trade result"; return false; }
            if (result.code == acnet::ResultCode::Ok && result.finalized &&
                !commit_transaction(connection.account,
                                    static_cast<std::uint16_t>(acnet::MessageType::TradeRequest),
                                    result.code, error)) return false;
            if (result.code == acnet::ResultCode::Ok) {
                acnet::ReplicationDelta delta;
                delta.kind = acnet::ResourceKind::Event;
                delta.target_account = request.other_account;
                delta.payload = payload;
                deltas_.append(std::move(delta));
            }
            return send_payload(connection, acnet::MessageType::TradeResult, acnet::Channel::Transactions,
                                payload, monotonic_ms, error);
        }
        case acnet::MessageType::ConversationRequest: {
            acnet::ConversationRequest request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            request.account = connection.account;
            acnet::ConversationResult result;
            if (request.action == acnet::ConversationAction::Begin) {
                result = npcs_.request_conversation(request.account, request.npc, movement_.current_tick());
            } else if (request.action == acnet::ConversationAction::Advance) {
                result = npcs_.advance_conversation(request.account, request.npc, request.lease_id,
                                                    request.choice, movement_.current_tick());
            } else {
                result.npc = request.npc;
                result.lease_id = request.lease_id;
                result.completed = true;
                result.code = npcs_.release_conversation(request.account, request.npc, request.lease_id)
                                  ? acnet::ResultCode::Ok : acnet::ResultCode::NotFound;
            }
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(result, payload)) { error = "failed to serialize conversation result"; return false; }
            return send_payload(connection, acnet::MessageType::ConversationResult, acnet::Channel::Transactions,
                                payload, monotonic_ms, error);
        }
        case acnet::MessageType::ZoneTransferRequest: {
            acnet::ZoneTransferRequest request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            acnet::TransferOffer offer = zones_.request_transfer(connection.account, request.door_id,
                                                                 movement_.current_tick());
            offer.baseline_revision = deltas_.current_revision();
            if (offer.code == acnet::ResultCode::Ok) {
                const acnet::PlayerView* player = players_.by_account(connection.account);
                if (player != nullptr) {
                    DoorTransition transition;
                    transition.account = connection.account;
                    transition.door_id = request.door_id;
                    transition.source_zone = offer.source_zone;
                    transition.destination_zone = offer.destination_zone;
                    transition.source_transform = player->transform;
                    transition.expires_tick = offer.expires_tick;
                    door_transitions_[connection.account] = transition;
                }
            }
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(offer, payload)) { error = "failed to serialize transfer offer"; return false; }
            return send_payload(connection, acnet::MessageType::ZoneTransferOffer, acnet::Channel::Transactions,
                                payload, monotonic_ms, error);
        }
        case acnet::MessageType::ZoneReady: {
            acnet::ZoneReadyRequest request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            acnet::TransferOffer reply;
            reply.account = connection.account;
            const acnet::TransferReservation* reservation = zones_.reservation(connection.account);
            std::uint32_t transition_door = 0;
            if (reservation != nullptr) {
                reply = reservation->offer;
                transition_door = reservation->door_id;
            }
            reply.code = valid_transition_transform(request.destination_transform)
                           ? zones_.acknowledge_ready(connection.account, request.token, movement_.current_tick())
                           : acnet::ResultCode::OutOfRange;
            if (reply.code == acnet::ResultCode::Ok) {
                acnet::PlayerView* player = players_.by_account(connection.account);
                if (player == nullptr ||
                    !movement_.teleport(connection.account, player->zone, request.destination_transform)) {
                    error = "failed to synchronize zone transfer";
                    return false;
                }
                player->transform = request.destination_transform;
                if (!commit_transaction(connection.account,
                                        static_cast<std::uint16_t>(acnet::MessageType::ZoneReady),
                                        reply.code, error)) return false;
                DoorTransition& transition = door_transitions_[connection.account];
                transition.account = connection.account;
                transition.door_id = transition_door;
                transition.source_zone = reply.source_zone;
                transition.destination_zone = reply.destination_zone;
                transition.ready = true;
                transition.expires_tick = movement_.current_tick() + config_.tick_rate * 2U;
            }
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(reply, payload) ||
                !send_payload(connection, acnet::MessageType::ZoneTransferOffer, acnet::Channel::Transactions,
                              payload, monotonic_ms, error)) return false;
            return reply.code != acnet::ResultCode::Ok || send_baseline(connection, monotonic_ms, error);
        }
        case acnet::MessageType::FurnitureRequest: {
            acnet::FurnitureOperation request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            request.account = connection.account;
            const acnet::FurnitureResult result = housing_.apply(request);
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(result, payload)) { error = "failed to serialize furniture result"; return false; }
            if (result.code == acnet::ResultCode::Ok && !result.replayed) {
                if (!commit_transaction(connection.account,
                                        static_cast<std::uint16_t>(acnet::MessageType::FurnitureRequest),
                                        result.code, error)) return false;
                acnet::ReplicationDelta delta;
                delta.kind = acnet::ResourceKind::House;
                delta.target_account = connection.account;
                delta.payload = payload;
                deltas_.append(std::move(delta));
            }
            return send_payload(connection, acnet::MessageType::FurnitureResult, acnet::Channel::Transactions,
                                payload, monotonic_ms, error);
        }
        case acnet::MessageType::HouseUpdate: {
            acnet::HouseUpdate request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            request.account = connection.account;
            const acnet::HouseUpdateResult result = housing_.replace_contents(request);
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(result, payload)) { error = "failed to serialize house update result"; return false; }
            if (result.code == acnet::ResultCode::Ok && !result.replayed &&
                !commit_transaction(connection.account,
                                    static_cast<std::uint16_t>(acnet::MessageType::HouseUpdate),
                                    result.code, error)) return false;
            if (!send_payload(connection, acnet::MessageType::HouseUpdateResult,
                              acnet::Channel::Transactions, payload, monotonic_ms, error)) return false;
            const acnet::HouseState* house = housing_.house(result.house_id);
            if (house == nullptr) return true;
            for (auto& active : connections_) {
                const acnet::PlayerView* viewer = players_.by_account(active.second.account);
                if (viewer != nullptr && viewer->zone == house->zone &&
                    !send_baseline(active.second, monotonic_ms, error)) return false;
            }
            return true;
        }
        case acnet::MessageType::EncounterRequest: {
            acnet::EncounterRequest request;
            if (!acnet::decode(packet.payload, request)) { ++metrics_.malformed_packets; return true; }
            request.account = connection.account;
            const ClockState& clock = clock_.state();
            const acnet::EncounterResult result = encounters_.resolve(
                request, movement_.current_tick(), clock.town_unix_seconds,
                static_cast<std::uint8_t>(clock.weather));
            std::vector<std::uint8_t> payload;
            if (!acnet::encode(result, payload)) { error = "failed to serialize encounter result"; return false; }
            if (result.code == acnet::ResultCode::Ok && result.caught && !result.replayed &&
                !commit_transaction(connection.account,
                                    static_cast<std::uint16_t>(acnet::MessageType::EncounterRequest),
                                    result.code, error)) return false;
            return send_payload(connection, acnet::MessageType::EncounterResult, acnet::Channel::Transactions,
                                payload, monotonic_ms, error);
        }
        default:
            ++metrics_.rejected_packets;
            return true;
    }
}

bool TownRuntime::handle_datagram(const acnet::Datagram& datagram,
                                  std::uint64_t monotonic_ms,
                                  std::string& error) {
    acnet::DecodedPacket packet;
    if (!acnet::decode_packet(datagram.bytes.data(), datagram.bytes.size(), packet, error)) {
        ++metrics_.malformed_packets;
        error.clear();
        return true;
    }
    if (packet.header.message_type == acnet::MessageType::ClientHello && packet.header.session == 0) {
        return handle_hello(datagram, packet, monotonic_ms, error);
    }
    const auto connection_it = connections_.find(packet.header.session);
    if (connection_it == connections_.end() || connection_it->second.host != datagram.host ||
        connection_it->second.port != datagram.port) {
        ++metrics_.rejected_packets;
        return true;
    }
    Connection& connection = connection_it->second;
    connection.last_received_ms = monotonic_ms;
    if (connection.encryption_active) {
        std::vector<std::uint8_t> plaintext;
        if ((packet.header.flags & acnet::PacketEncrypted) == 0 ||
            !acnet::open_payload(packet.header, connection.session_keys.client_to_server,
                                 packet.payload, plaintext)) {
            ++metrics_.rejected_packets;
            return true;
        }
        packet.payload = std::move(plaintext);
    } else if ((packet.header.flags & acnet::PacketEncrypted) != 0) {
        ++metrics_.rejected_packets;
        return true;
    }
    if (connection.reliability.receive(packet.header) != acnet::ReceiveDisposition::New) return true;
    if ((packet.header.flags & acnet::PacketAckOnly) != 0) return true;
    if ((packet.header.flags & acnet::PacketReliable) != 0 &&
        !send_ack(connection, packet.header.channel, error)) return false;
    if (!allow_message(connection, packet.header.message_type, monotonic_ms)) {
        ++metrics_.rejected_packets;
        return true;
    }
    if (packet.header.message_type == acnet::MessageType::Disconnect) {
        record_event("Account " + std::to_string(connection.account) + " disconnected cleanly");
        sessions_.disconnect(connection.session, monotonic_ms);
        deactivate_player(connection.account, movement_.current_tick());
        if (!database_.record_session(connection.session, connection.account, false,
                                      wall_unix_seconds(), error)) return false;
        endpoints_.erase(endpoint_key(connection.host, connection.port));
        connections_.erase(connection_it);
        return true;
    }
    return dispatch(connection, std::move(packet), monotonic_ms, error);
}

bool TownRuntime::receive_packets(std::uint64_t monotonic_ms, std::string& error) {
    for (std::size_t count = 0; count < 1024; ++count) {
        acnet::Datagram datagram;
        if (!socket_.receive(datagram, error)) return error.empty();
        ++metrics_.packets_received;
        if (!handle_datagram(datagram, monotonic_ms, error)) return false;
    }
    return true;
}

bool TownRuntime::send_snapshots(std::uint64_t monotonic_ms, std::string& error) {
    for (auto& item : connections_) {
        Connection& connection = item.second;
        const acnet::PlayerView* viewer = players_.by_account(connection.account);
        if (viewer == nullptr) continue;
        acnet::TransformSnapshot snapshot;
        snapshot.server_tick = movement_.current_tick();
        snapshot.baseline_revision = deltas_.current_revision();
        snapshot.house_light_mask = house_light_mask();
        std::unordered_set<acnet::AccountId> included;
        for (const acnet::PlayerView* player : players_.query_zone(viewer->zone, acnet::kMaxPlayersPerZone)) {
            acnet::PlayerSnapshot state = movement_.snapshot(player->account);
            state.appearance = player->appearance;
            const auto transition = door_transitions_.find(player->account);
            if (transition != door_transitions_.end()) {
                if (transition->second.source_zone == viewer->zone) {
                    state.transition_phase = acnet::DoorTransitionPhase::Leaving;
                    state.transition_door = transition->second.door_id;
                    state.transition_expires_tick = transition->second.expires_tick;
                } else if (transition->second.ready && transition->second.destination_zone == viewer->zone) {
                    state.transition_phase = acnet::DoorTransitionPhase::Arriving;
                    state.transition_door = transition->second.door_id;
                    state.transition_expires_tick = transition->second.expires_tick;
                }
            }
            if (state.account != 0) {
                included.insert(state.account);
                snapshot.players.push_back(state);
            }
        }
        for (const auto& transition : door_transitions_) {
            if (!transition.second.ready || transition.second.source_zone != viewer->zone ||
                included.find(transition.first) != included.end() ||
                snapshot.players.size() >= acnet::kMaxPlayersPerZone) continue;
            const acnet::PlayerView* player = players_.by_account(transition.first);
            if (player == nullptr) continue;
            acnet::PlayerSnapshot state = movement_.snapshot(player->account);
            state.zone = viewer->zone;
            state.transform = transition.second.source_transform;
            state.appearance = player->appearance;
            state.transition_phase = acnet::DoorTransitionPhase::Leaving;
            state.transition_door = transition.second.door_id;
            state.transition_expires_tick = transition.second.expires_tick;
            snapshot.players.push_back(state);
        }
        std::vector<std::uint8_t> payload;
        if (!acnet::encode(snapshot, payload) ||
            !send_payload(connection, acnet::MessageType::TransformSnapshot, acnet::Channel::Snapshots, payload,
                          monotonic_ms, error)) return false;
        ++metrics_.snapshots_sent;
    }
    return true;
}

void TownRuntime::disconnect_timed_out(std::uint64_t monotonic_ms) {
    std::vector<acnet::SessionId> timed_out;
    for (const auto& item : connections_) {
        if (monotonic_ms >= item.second.last_received_ms &&
            monotonic_ms - item.second.last_received_ms > config_.connection_timeout_ms) timed_out.push_back(item.first);
    }
    for (acnet::SessionId id : timed_out) {
        const auto found = connections_.find(id);
        if (found == connections_.end()) continue;
        sessions_.disconnect(id, monotonic_ms);
        record_event("Account " + std::to_string(found->second.account) + " timed out");
        deactivate_player(found->second.account, movement_.current_tick());
        std::string ignored;
        database_.record_session(id, found->second.account, false, wall_unix_seconds(), ignored);
        endpoints_.erase(endpoint_key(found->second.host, found->second.port));
        connections_.erase(found);
    }
    sessions_.expire(monotonic_ms);
}

bool TownRuntime::step(std::uint64_t monotonic_ms, std::int64_t wall_seconds, std::string& error) {
    error.clear();
    if (!initialized_) {
        error = "runtime is not initialized";
        return false;
    }
    if (!receive_packets(monotonic_ms, error)) return false;
    movement_.tick();
    for (const auto& active : connections_) {
        const acnet::MovementPlayer* movement = movement_.player(active.second.account);
        acnet::PlayerView* mutable_view = players_.by_account(active.second.account);
        if (movement == nullptr || mutable_view == nullptr) continue;
        mutable_view->transform = movement->transform;
        /* Animation rides the input command but is republished only when it
         * actually changes, which is a handful of times a second even while
         * running -- far cheaper than putting it in every snapshot, and
         * reliable, so a viewer cannot miss a transition and hold a pose. */
        if (mutable_view->presentation.animation != movement->animation) {
            mutable_view->presentation.animation = movement->animation;
            publish_presentation(*mutable_view);
        }
    }
    if (!clock_.advance(wall_seconds, connections_.empty())) {
        error = background_error_.empty() ? "town clock update failed" : background_error_;
        return false;
    }
    const std::int64_t clock_minute = clock_.state().town_unix_seconds / 60;
    if (clock_minute != last_clock_minute_ || clock_.state().weather != last_weather_ ||
        clock_.state().weather_intensity != last_weather_intensity_) {
        for (auto& connection : connections_) connection.second.has_exterior_chunk = false;
        last_clock_minute_ = clock_minute;
        last_weather_ = clock_.state().weather;
        last_weather_intensity_ = clock_.state().weather_intensity;
    }
    zones_.expire(movement_.current_tick());
    for (auto transition = door_transitions_.begin(); transition != door_transitions_.end();) {
        if ((transition->second.ready && movement_.current_tick() > transition->second.expires_tick) ||
            (!transition->second.ready && zones_.reservation(transition->first) == nullptr)) {
            transition = door_transitions_.erase(transition);
        } else {
            ++transition;
        }
    }
    zones_.update_sleep_states(movement_.current_tick());
    npcs_.expire(movement_.current_tick());
    const std::uint32_t interval = std::max<std::uint32_t>(1, config_.tick_rate / config_.snapshot_rate);
    if ((movement_.current_tick() % interval) == 0 && !send_snapshots(monotonic_ms, error)) return false;
    for (auto& item : connections_) {
        item.second.fragments.expire(monotonic_ms);
        if (!refresh_interest_chunk(item.second, monotonic_ms, error) ||
            !send_deltas(item.second, monotonic_ms, error)) return false;
        for (const acnet::PendingDatagram& pending : item.second.reliability.retransmissions(monotonic_ms, 250, 8)) {
            if (!socket_.send(item.second.host, item.second.port, pending.bytes, error)) return false;
            ++metrics_.packets_sent;
        }
    }
    disconnect_timed_out(monotonic_ms);
    publish_population_change();
    publish_resident_change();
    const acnet::Tick checkpoint_interval = config_.tick_rate * 300U;
    if (checkpoint_interval != 0 && movement_.current_tick() - last_checkpoint_tick_ >= checkpoint_interval) {
        const std::vector<std::uint8_t> state = encode_state();
        if (state.empty() || !persistence_.write_checkpoint(persistence_.last_sequence(), state, error)) return false;
        last_checkpoint_tick_ = movement_.current_tick();
        record_event("Automatic town checkpoint completed");
    }
    ++metrics_.ticks;
    return true;
}

bool TownRuntime::shutdown(std::string& error) {
    error.clear();
    if (!initialized_) return true;
    const std::vector<std::uint8_t> state = encode_state();
    if (state.empty() || !database_.audit(0, "server_stopped", "orderly shutdown",
                                          wall_unix_seconds(), error) ||
        !persistence_.write_checkpoint(persistence_.last_sequence(), state, error) ||
        !persistence_.mark_clean_shutdown(error)) return false;
    socket_.close();
    initialized_ = false;
    std::cout << "{\"event\":\"server_stopped\",\"ticks\":" << metrics_.ticks
              << ",\"packets_received\":" << metrics_.packets_received
              << ",\"packets_sent\":" << metrics_.packets_sent << "}\n";
    return true;
}

bool TownRuntime::checkpoint_now(std::string& error) {
    error.clear();
    if (!initialized_) { error = "runtime is not initialized"; return false; }
    const std::vector<std::uint8_t> state = encode_state();
    if (state.empty() || !persistence_.write_checkpoint(persistence_.last_sequence(), state, error)) return false;
    last_checkpoint_tick_ = movement_.current_tick();
    if (!database_.audit(0, "checkpoint", "operator requested checkpoint", wall_unix_seconds(), error)) return false;
    record_event("Operator checkpoint completed");
    return true;
}

bool TownRuntime::set_account_banned(acnet::AccountId account, bool banned, std::string& error) {
    error.clear();
    if (!initialized_ || account == 0 ||
        !database_.set_banned(account, banned, wall_unix_seconds(), error) ||
        !database_.audit(0, banned ? "ban" : "unban", "account=" + std::to_string(account),
                         wall_unix_seconds(), error)) return false;
    if (banned) {
        for (auto it = connections_.begin(); it != connections_.end(); ++it) {
            if (it->second.account != account) continue;
            const acnet::SessionId session = it->first;
            send_payload(it->second, acnet::MessageType::Disconnect, acnet::Channel::Control, {},
                         monotonic_milliseconds(), error);
            sessions_.close(session);
            deactivate_player(account, movement_.current_tick());
            endpoints_.erase(endpoint_key(it->second.host, it->second.port));
            connections_.erase(it);
            break;
        }
    }
    return error.empty();
}

bool TownRuntime::grant_bank_bells(acnet::AccountId account, std::uint64_t amount, std::string& error) {
    error.clear();
    if (!initialized_) { error = "runtime is not initialized"; return false; }
    if (economy_.ledger(account) == nullptr) {
        error = "unknown account " + std::to_string(account) +
                " (accounts are created the first time a player connects)";
        return false;
    }
    const acnet::EconomyResult result = economy_.admin_grant_bank_bells(account, amount);
    if (result.code != acnet::ResultCode::Ok) {
        error = "bank grant rejected with result code " + std::to_string(static_cast<unsigned>(result.code));
        return false;
    }
    /* Durable before it is reported: the same rule a player deposit follows. */
    if (!commit_transaction(account, kAdminGrantBellsOperation, result.code, error) ||
        !database_.audit(0, "grant-bells",
                         "account=" + std::to_string(account) + " amount=" + std::to_string(amount) +
                             " balance=" + std::to_string(result.balance),
                         wall_unix_seconds(), error)) return false;
    record_event("operator granted " + std::to_string(amount) + " bells to account " + std::to_string(account));
    return true;
}

bool TownRuntime::send_mail(acnet::AccountId recipient,
                            std::uint16_t attachment,
                            const std::string& text,
                            std::string& error) {
    error.clear();
    if (!initialized_) { error = "runtime is not initialized"; return false; }
    if (economy_.ledger(recipient) == nullptr) {
        error = "unknown account " + std::to_string(recipient) +
                " (accounts are created the first time a player connects)";
        return false;
    }
    if (text.size() > acnet::kMailBodyBytes) {
        error = "letter text exceeds " + std::to_string(acnet::kMailBodyBytes) + " bytes";
        return false;
    }
    acnet::MailContent content;
    std::copy(text.begin(), text.end(), content.body.begin());
    /* The original letter fonts: 0 is a received letter and 3 is a received
     * letter with a present still attached, both unread. Anything else would
     * make the UI treat an operator gift as one the player wrote. */
    content.font = attachment != 0 ? 3 : 0;
    content.mail_type = 0; /* mMl_TYPE_MAIL */
    /* sender_name is the raw 22-byte Mail_nm_c the client projects straight
     * into the letter: an 8-byte player name, an 8-byte town name, two
     * identifiers, then the name type (0 = a player, so the UI simply prints
     * the name instead of looking up an NPC). */
    static const char kOperatorName[] = "TownHall";
    std::copy(std::begin(kOperatorName), std::end(kOperatorName) - 1, content.sender_name.begin());
    const std::string town = config_.town_name.substr(0, 8);
    std::copy(town.begin(), town.end(), content.sender_name.begin() + 8);
    content.sender_name[20] = 0;
    const acnet::EconomyResult result = economy_.admin_send_mail(recipient, attachment, content);
    if (result.code == acnet::ResultCode::Capacity) {
        error = "mailbox for account " + std::to_string(recipient) + " already holds " +
                std::to_string(acnet::kMailboxCapacity) + " letters";
        return false;
    }
    if (result.code != acnet::ResultCode::Ok) {
        error = "mail delivery rejected with result code " + std::to_string(static_cast<unsigned>(result.code));
        return false;
    }
    const acnet::MailRecord* delivered = economy_.mail(result.mail_id);
    if (delivered == nullptr) { error = "delivered letter is missing"; return false; }
    if (!commit_transaction(recipient, kAdminSendMailOperation, result.code, error) ||
        !database_.audit(0, "send-mail",
                         "account=" + std::to_string(recipient) + " item=" + std::to_string(attachment) +
                             " mail=" + std::to_string(result.mail_id),
                         wall_unix_seconds(), error)) return false;
    /* Harmless while the town is stopped, and correct if it ever runs live. */
    if (!publish_mail_change(*delivered, false, error)) return false;
    record_event("operator posted letter " + std::to_string(result.mail_id) + " to account " +
                 std::to_string(recipient));
    return true;
}

std::vector<RuntimeAccountSummary> TownRuntime::account_summaries() const {
    std::vector<RuntimeAccountSummary> summaries;
    summaries.reserve(accounts_.size());
    for (const auto& entry : accounts_) {
        RuntimeAccountSummary summary;
        summary.account = entry.first;
        summary.resident_slot = entry.second.resident_slot;
        summary.resident = entry.second.kind == acnet::PlayerKind::Resident;
        for (const auto& connection : connections_) {
            if (connection.second.account == entry.first) { summary.connected = true; break; }
        }
        const auto& name = entry.second.appearance.name;
        const auto end = std::find(name.begin(), name.end(), 0);
        summary.name.assign(name.begin(), end);
        const acnet::InventoryState* inventory = world_.inventory(entry.first);
        if (inventory != nullptr) summary.bells = inventory->bells;
        const acnet::AccountLedger* ledger = economy_.ledger(entry.first);
        if (ledger != nullptr) {
            summary.bank_balance = ledger->bank_balance;
            summary.debt = ledger->debt;
        }
        const acnet::MailboxState* mailbox = economy_.mailbox(entry.first);
        if (mailbox != nullptr) {
            summary.pending_mail = mailbox->mail.size();
            summary.carried_mail = mailbox->carried.size();
        }
        summaries.push_back(std::move(summary));
    }
    std::sort(summaries.begin(), summaries.end(), [](const auto& left, const auto& right) {
        return left.account < right.account;
    });
    return summaries;
}

bool TownRuntime::import_gci(const std::filesystem::path& source, std::string& error) {
    if (!initialized_) { error = "runtime is not initialized"; return false; }
    if (!connections_.empty()) { error = "disconnect all clients before importing a GCI"; return false; }
    if (!accounts_.empty()) { error = "GCI import requires a fresh dedicated town"; return false; }
    if (!persistence_.import_gci(source, error)) return false;
    std::vector<std::uint8_t> bytes;
    GciTownState imported;
    if (!persistence_.load_gci_bytes(bytes, error) || !decode_gci_town(bytes, imported, error)) return false;
    for (const auto& tile : imported.tiles) {
        if (!world_.set_tile(tile.first, tile.second)) { error = "failed to import GCI foreground"; return false; }
    }
    town_bootstrapped_ = true;
    if (!configure_zone_topology(error)) return false;
    for (std::size_t slot = 0; slot < imported.residents.size(); ++slot) {
        if (!imported.residents[slot].exists) continue;
        const acnet::AccountId account = static_cast<acnet::AccountId>(slot + 1);
        AccountState state;
        state.entity = entities_.add(static_cast<std::uintptr_t>(account), 0, 1, 1);
        state.kind = acnet::PlayerKind::Resident;
        state.zone = 1;
        state.transform.position = resident_spawn(state.resident_slot);
        state.appearance = imported.residents[slot].appearance;
        state.pattern = imported.residents[slot].pattern;
        state.resident_slot = static_cast<std::uint8_t>(slot);
        if (state.entity == 0 || !world_.set_inventory(account, imported.residents[slot].inventory) ||
            !economy_.set_account(account, imported.residents[slot].ledger) ||
            !housing_.register_resident(state.resident_slot, account, 100 + state.resident_slot) ||
            !database_.record_account(account, wall_unix_seconds(), error)) {
            if (error.empty()) error = "failed to import GCI resident slot";
            return false;
        }
        accounts_[account] = state;
    }
    const auto weather = static_cast<Weather>(std::min<std::uint8_t>(imported.weather,
        static_cast<std::uint8_t>(Weather::Snow)));
    if (!clock_.set_weather(weather, imported.weather_intensity) || !commit_state(110, error) ||
        !database_.audit(0, "gci_import", source.filename().string(), wall_unix_seconds(), error)) return false;
    return checkpoint_now(error);
}

bool TownRuntime::export_gci(const std::filesystem::path& destination, std::string& error) const {
    if (!initialized_) { error = "runtime is not initialized"; return false; }
    std::vector<std::uint8_t> bytes;
    if (!persistence_.load_gci_bytes(bytes, error)) return false;
    GciTownState state;
    state.tiles = world_.tiles_in_zone(1);
    for (const auto& account : accounts_) {
        if (account.second.kind != acnet::PlayerKind::Resident ||
            account.second.resident_slot >= state.residents.size()) continue;
        GciResidentState& resident = state.residents[account.second.resident_slot];
        const acnet::InventoryState* inventory = world_.inventory(account.first);
        const acnet::AccountLedger* ledger = economy_.ledger(account.first);
        if (inventory == nullptr || ledger == nullptr) { error = "resident state is incomplete"; return false; }
        resident.exists = true;
        resident.inventory = *inventory;
        resident.ledger = *ledger;
        resident.appearance = account.second.appearance;
        resident.pattern = account.second.pattern;
    }
    state.weather = static_cast<std::uint8_t>(clock_.state().weather);
    state.weather_intensity = clock_.state().weather_intensity;
    if (!encode_gci_town(bytes, state, error)) return false;
    return persistence_.export_gci_bytes(bytes, destination, error);
}

} // namespace acserver
