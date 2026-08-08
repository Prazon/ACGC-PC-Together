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
/* The zone a resident house's exterior -- and therefore its gyroid -- stands
 * in. House zones are the interiors; the gyroid is worked from the field. */
constexpr ZoneId kTownFieldZone = 1;

/* The gyroid in front of each resident house: Haniwa_c in the original save.
 * Four display slots, a 128-byte visitor message in the game's own font
 * encoding (opaque here, like mail text), and the bells guests have paid. */
constexpr std::size_t kGyroidItemSlots = 4;
constexpr std::size_t kGyroidMessageBytes = 128;

/* mHm_HANIWA_TRADE_*: how a displayed item may leave the gyroid. The original
 * UI only ever writes 0..2; 3 exists in the enum but nothing produces it, so
 * it is rejected on the wire rather than given a meaning here. */
constexpr std::uint8_t kGyroidExchangeFree = 0;
constexpr std::uint8_t kGyroidExchangeDisplay = 1;
constexpr std::uint8_t kGyroidExchangeSale = 2;
constexpr std::uint8_t kMaximumGyroidExchange = kGyroidExchangeSale;

struct GyroidItem {
    std::uint16_t item = 0; /* 0 = empty, matching both EMPTY_NO and ItemSlot */
    std::uint8_t exchange = kGyroidExchangeFree;
    std::uint32_t price = 0; /* Haniwa_Item_c::extra_data; bells when exchange is Sale */

    bool operator==(const GyroidItem& other) const {
        return item == other.item && exchange == other.exchange && price == other.price;
    }
};

struct GyroidState {
    Revision revision = 1;
    std::array<GyroidItem, kGyroidItemSlots> items{};
    std::array<std::uint8_t, kGyroidMessageBytes> message{};
    std::uint32_t bells = 0;
};

enum class GyroidOpType : std::uint8_t {
    /* Owner replaces the display: items, exchange terms, and the message.
     * Bells are deliberately absent -- they only move through Take/Collect. */
    Update,
    /* A guest takes one displayed item, paying its price if it is for sale. */
    Take,
    /* Owner empties the proceeds into their wallet. */
    Collect,
};

struct GyroidOperation {
    GyroidOpType type = GyroidOpType::Update;
    AccountId account = 0;
    IdempotencyKey idempotency;
    std::uint64_t house_id = 0;
    Revision expected_gyroid_revision = 0;
    Revision expected_inventory_revision = 0;
    /* Update */
    std::array<GyroidItem, kGyroidItemSlots> items{};
    std::array<std::uint8_t, kGyroidMessageBytes> message{};
    /* Take */
    std::uint8_t item_slot = 0;
    std::uint16_t expected_item = 0;
};

struct GyroidResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    std::uint64_t house_id = 0;
    Revision gyroid_revision = 0;
    Revision inventory_revision = 0;
    std::uint16_t item = 0;         /* Take: what left the gyroid */
    std::uint32_t price = 0;        /* Take: what the guest paid */
    std::uint32_t bells_collected = 0; /* Collect */
    std::uint8_t inventory_slot = 0;   /* Take: pocket that received the item */
    bool replayed = false;
};
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

/* Everything about a house that is a *surface* rather than an object standing
 * on one. All of it is visible to somebody other than the owner -- the walls
 * and floor to anyone in the room, the palette and door design to anyone
 * walking past outside -- and all of it was previously local, so two players
 * standing in the same room saw different wallpaper and a repainted house
 * looked unchanged to everybody but its owner.
 *
 * The indices are carried opaquely. They address the game's wall, floor and
 * design tables, which the server has no business knowing the size of, and the
 * client already clamps an out-of-range index to zero as it loads the room
 * (aMI_CheckFloorWallIndex), so a hostile value cannot walk off a table on a
 * viewer. */
struct HouseSurfaces {
    std::array<std::uint8_t, kHouseFloorCount> wallpaper{};
    std::array<std::uint8_t, kHouseFloorCount> flooring{};
    /* mHm_fllot_bit_c: bit 0 wall_original, bit 1 floor_original -- the surface
     * is one of the player's own designs rather than a catalogue item. The
     * remaining six bits are unused by the original and are rejected rather
     * than stored, so they cannot become a covert channel. */
    std::array<std::uint8_t, kHouseFloorCount> pattern_bits{};
    /* mHm_hs_c::outlook_pal and the two pending changes the original applies at
     * the day rollover: ordered_outlook_pal (bought at Nook's with an upgrade)
     * and next_outlook_pal (everything else -- a villager, Wisp, a repaint). */
    std::uint8_t exterior_palette = 0;
    std::uint8_t ordered_exterior_palette = 0;
    std::uint8_t next_exterior_palette = 0;
    /* mHm_hs_c::door_original, the player design hung on the front door. */
    std::uint8_t door_design = 0;

    bool operator==(const HouseSurfaces& other) const {
        return wallpaper == other.wallpaper && flooring == other.flooring && pattern_bits == other.pattern_bits &&
               exterior_palette == other.exterior_palette &&
               ordered_exterior_palette == other.ordered_exterior_palette &&
               next_exterior_palette == other.next_exterior_palette && door_design == other.door_design;
    }
};

/* The two design flags mHm_fllot_bit_c actually defines. */
constexpr std::uint8_t kHouseSurfacePatternMask = 0x03;

class ByteWriter;
class ByteReader;

/* Shared by the HouseUpdate request and the HouseState baseline so the two can
 * never disagree about the field order. decode rejects a pattern_bits byte with
 * anything set outside kHouseSurfacePatternMask. */
bool encode_house_surfaces(ByteWriter& writer, const HouseSurfaces& surfaces);
bool decode_house_surfaces(ByteReader& reader, HouseSurfaces& surfaces);

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
    HouseSurfaces surfaces;
    /* mHm_hs_c::music_box: a 64-bit bitfield of which K.K. songs the house's
     * stereo holds, in two u32s exactly as the save stores it. Persistent
     * per-house state that only the owner changes, so it rides the same
     * whole-room submit the furniture does. */
    std::array<std::uint32_t, 2> music_box{};
    /* item is the canonical pocket item ID; condition stores the original
     * room-facing direction (0..3). Reserved footprint cells use their
     * original 0xFxxx value with condition zero. */
    std::unordered_map<FurnitureAddress, ItemSlot, FurnitureAddressHash> furniture;
    /* Meaningless for a shared house, which has no exterior of its own. */
    GyroidState gyroid;
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
    HouseSurfaces surfaces;
    /* mHm_hs_c::music_box: a 64-bit bitfield of which K.K. songs the house's
     * stereo holds, in two u32s exactly as the save stores it. Persistent
     * per-house state that only the owner changes, so it rides the same
     * whole-room submit the furniture does. */
    std::array<std::uint32_t, 2> music_box{};
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
    GyroidResult apply_gyroid(const GyroidOperation& operation);
    /* Overflow policy for Collect, mirroring the economy's sale overflow: the
     * wallet is capped at `maximum` and each `chunk` above it comes back as an
     * `overflow_item` money bag in an empty pocket. Zero disables the cap. */
    void set_wallet_policy(std::uint32_t maximum, std::uint32_t chunk, std::uint16_t overflow_item);
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
    std::unordered_map<OperationKey, GyroidResult, OperationKeyHash> gyroid_idempotency_;
    std::uint32_t wallet_maximum_ = 0;
    std::uint32_t wallet_overflow_chunk_ = 0;
    std::uint16_t wallet_overflow_item_ = 0;
};

} // namespace acnet
