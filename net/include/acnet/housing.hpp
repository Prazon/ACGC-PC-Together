#pragma once

#include "acnet/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace acnet {

constexpr std::size_t kOriginalResidentSlots = 4;
constexpr std::size_t kHouseFloorCount = 3;
constexpr std::size_t kHouseLayerCount = 4;
constexpr std::uint8_t kMaximumHouseUpgradeLevel = 3;
constexpr std::size_t kMaximumHouseFurniture = kHouseFloorCount * kHouseLayerCount * 16U * 16U;

/* original_slot value of a house that belongs to no resident. */
constexpr std::uint8_t kSharedHouseSlot = 0xFF;
/* The island cabin. One per town, owned by nobody, editable by whoever is
 * standing in it -- which is what Save_t.island.cottage already means. */
constexpr std::uint64_t kIslandCabinHouseId = 20000;
/* Room count the shared cabin actually has: mHm_cottage_c holds a single
 * mHm_flr_c. Floors above this are rejected for a shared house. */
constexpr std::size_t kSharedHouseFloorCount = 1;

struct FurnitureAddress {
    std::uint8_t x = 0;
    std::uint8_t z = 0;
    std::uint8_t floor = 0;
    std::uint8_t layer = 0;

    bool operator==(const FurnitureAddress& other) const {
        return x == other.x && z == other.z && floor == other.floor && layer == other.layer;
    }
};

struct FurnitureAddressHash {
    std::size_t operator()(const FurnitureAddress& address) const;
};

struct HouseState {
    std::uint64_t house_id = 0;
    AccountId owner = 0;
    std::uint8_t original_slot = 0;
    ZoneId zone = 0;
    std::uint8_t upgrade_level = 0;
    Revision revision = 1;
    bool initialized = false;
    /* A shared house has no owner. Presence in its zone is the authorization,
     * so the ownership test below is skipped and the zone test is not optional. */
    bool shared = false;
    bool main_light_on = false;
    bool basement_light_on = false;
    std::array<std::int16_t, kHouseFloorCount> music_tracks{{-1, -1, -1}};
    std::array<std::uint64_t, kHouseFloorCount * kHouseLayerCount> furniture_switches{};
    /* item is the canonical pocket item ID; condition stores the original
     * room-facing direction (0..3). Reserved footprint cells use their
     * original 0xFxxx value with condition zero. */
    std::unordered_map<FurnitureAddress, ItemSlot, FurnitureAddressHash> furniture;
};

struct HouseUpdate {
    AccountId account = 0;
    IdempotencyKey idempotency;
    std::uint64_t house_id = 0;
    Revision expected_house_revision = 0;
    std::uint8_t upgrade_level = 0;
    bool main_light_on = false;
    bool basement_light_on = false;
    std::array<std::int16_t, kHouseFloorCount> music_tracks{{-1, -1, -1}};
    std::array<std::uint64_t, kHouseFloorCount * kHouseLayerCount> furniture_switches{};
    std::unordered_map<FurnitureAddress, ItemSlot, FurnitureAddressHash> furniture;
};

struct HouseUpdateResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    std::uint64_t house_id = 0;
    Revision house_revision = 0;
    bool replayed = false;
};

enum class FurnitureOpType : std::uint8_t {
    Place,
    Remove,
};

struct FurnitureOperation {
    FurnitureOpType type = FurnitureOpType::Place;
    AccountId account = 0;
    IdempotencyKey idempotency;
    std::uint64_t house_id = 0;
    FurnitureAddress address;
    Revision expected_house_revision = 0;
    Revision expected_inventory_revision = 0;
    std::uint8_t inventory_slot = 0;
    std::uint16_t expected_item = 0;
};

struct FurnitureResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    std::uint64_t house_id = 0;
    Revision house_revision = 0;
    Revision inventory_revision = 0;
    std::uint8_t inventory_slot = 0;
    std::uint16_t item = 0;
    bool replayed = false;
};

class HousingAuthority {
public:
    explicit HousingAuthority(WorldAuthority* world, PlayerDirectory* players = nullptr);

    bool register_resident(std::uint8_t original_slot, AccountId owner, ZoneId zone);
    /* Creates an ownerless house that anyone present in `zone` may decorate.
     * Registering an existing shared house again is a no-op that succeeds, so
     * startup may call it unconditionally after a restore. */
    bool register_shared_house(std::uint64_t house_id, ZoneId zone);
    const HouseState* house(std::uint64_t house_id) const;
    const HouseState* house_for(AccountId owner) const;
    const HouseState* shared_house_in(ZoneId zone) const;
    FurnitureResult apply(const FurnitureOperation& operation);
    HouseUpdateResult replace_contents(const HouseUpdate& update);
    std::size_t resident_count() const;
    std::uint64_t total_furniture_units() const;
    const std::unordered_map<std::uint64_t, HouseState>& houses() const { return houses_; }
    bool restore_house(const HouseState& house);

private:
    struct OperationKey {
        AccountId account;
        IdempotencyKey key;
        bool operator==(const OperationKey& other) const {
            return account == other.account && key == other.key;
        }
    };
    struct OperationKeyHash {
        std::size_t operator()(const OperationKey& value) const;
    };

    static Revision next_revision(Revision revision);
    static std::uint64_t house_id_for_slot(std::uint8_t slot);
    static std::optional<std::uint8_t> empty_slot(const InventoryState& inventory);

    WorldAuthority* world_;
    PlayerDirectory* players_;
    std::array<AccountId, kOriginalResidentSlots> residents_{};
    std::unordered_map<std::uint64_t, HouseState> houses_;
    std::unordered_map<AccountId, std::uint64_t> owner_houses_;
    std::unordered_map<OperationKey, FurnitureResult, OperationKeyHash> idempotency_;
    std::unordered_map<OperationKey, HouseUpdateResult, OperationKeyHash> update_idempotency_;
};

} // namespace acnet
