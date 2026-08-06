#pragma once

#include "acnet/player_query.hpp"
#include "acnet/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace acnet {

constexpr std::size_t kInventorySlots = 15;

enum class TerrainState : std::uint8_t {
    Normal,
    Hole,
    Tree,
    Stump,
    Planted,
};

struct ItemSlot {
    std::uint16_t item = 0;
    std::uint8_t condition = 0;
};

struct InventoryState {
    Revision revision = 1;
    std::uint32_t bells = 0;
    std::array<ItemSlot, kInventorySlots> slots{};
    /* What the player is holding. The original stores this outside the pocket
     * array too (Private_c::equipment), and holding is a move out of a pocket,
     * so it shares the inventory revision: a client that observed the pockets
     * observed the hand at the same instant. Tool checks read this and nothing
     * else -- a shovel sitting in a pocket does not dig. */
    ItemSlot equipped{};
};

struct TileAddress {
    ZoneId zone = 0;
    std::int16_t x = 0;
    std::int16_t z = 0;

    bool operator==(const TileAddress& other) const {
        return zone == other.zone && x == other.x && z == other.z;
    }
};

struct TileAddressHash {
    std::size_t operator()(const TileAddress& value) const;
};

struct TileState {
    Revision revision = 1;
    std::uint16_t item = 0;
    std::uint8_t condition = 0;
    TerrainState terrain = TerrainState::Normal;
    bool buried = false;
    bool placed_furniture = false;
};

struct IdempotencyKey {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    bool valid() const { return high != 0 || low != 0; }
    bool operator==(const IdempotencyKey& other) const { return high == other.high && low == other.low; }
};

enum class WorldOpType : std::uint8_t {
    DropItem,
    PickupItem,
    Dig,
    Bury,
    Plant,
    ChopTree,
    PlaceFurniture,
    RemoveFurniture,
    FillHole,
};

struct WorldOperation {
    WorldOpType type = WorldOpType::PickupItem;
    AccountId account = 0;
    IdempotencyKey idempotency;
    TileAddress tile;
    Revision expected_tile_revision = 0;
    Revision expected_inventory_revision = 0;
    std::uint8_t inventory_slot = 0;
    std::uint16_t expected_item = 0;
};

struct WorldResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    TileAddress tile;
    Revision tile_revision = 0;
    Revision inventory_revision = 0;
    std::uint16_t transferred_item = 0;
    std::uint8_t inventory_slot = 0;
    bool replayed = false;
};

using WorldCommitHook = std::function<bool(const WorldOperation&,
                                           const WorldResult&,
                                           const TileState&,
                                           const InventoryState&)>;

struct WorldConfig {
    float tile_size = 40.0F;
    float maximum_interaction_distance = 90.0F;
    std::size_t maximum_idempotency_records = 4096;
    Tick operation_cooldown_ticks = 6;
    Tick terrain_action_cooldown_ticks = 30;
};

class WorldAuthority {
public:
    explicit WorldAuthority(PlayerDirectory* players, WorldConfig config = {});

    bool register_inventory(AccountId account, const InventoryState& inventory = {});
    bool set_inventory(AccountId account, const InventoryState& inventory);
    bool set_tile(const TileAddress& address, const TileState& tile);
    const InventoryState* inventory(AccountId account) const;
    const TileState* tile(const TileAddress& address) const;
    std::vector<std::pair<TileAddress, TileState>> tiles_in_zone(ZoneId zone) const;
    const std::unordered_map<AccountId, InventoryState>& inventories() const { return inventories_; }
    const std::unordered_map<TileAddress, TileState, TileAddressHash>& tiles() const { return tiles_; }

    WorldResult apply(const WorldOperation& operation, Tick tick = 0);
    void set_commit_hook(WorldCommitHook hook) { commit_hook_ = std::move(hook); }

    std::uint64_t total_item_units() const;
    std::size_t idempotency_record_count() const { return idempotency_.size(); }

private:
    struct OperationKey {
        AccountId account = 0;
        IdempotencyKey key;
        bool operator==(const OperationKey& other) const {
            return account == other.account && key == other.key;
        }
    };
    struct OperationKeyHash {
        std::size_t operator()(const OperationKey& value) const;
    };

    bool in_range(const WorldOperation& operation) const;
    static Revision next_revision(Revision current);
    static std::optional<std::uint8_t> first_empty_slot(const InventoryState& inventory);
    WorldResult reject(const WorldOperation& operation,
                       ResultCode code,
                       const TileState* tile,
                       const InventoryState* inventory) const;
    void remember(const OperationKey& key, const WorldResult& result);

    PlayerDirectory* players_;
    WorldConfig config_;
    WorldCommitHook commit_hook_;
    std::unordered_map<AccountId, InventoryState> inventories_;
    std::unordered_map<TileAddress, TileState, TileAddressHash> tiles_;
    std::unordered_map<OperationKey, WorldResult, OperationKeyHash> idempotency_;
    std::vector<OperationKey> idempotency_order_;
    std::unordered_map<AccountId, Tick> next_operation_tick_;
};

} // namespace acnet
