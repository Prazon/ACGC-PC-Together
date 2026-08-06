#pragma once

#include "acnet/economy.hpp"
#include "acnet/housing.hpp"
#include "acnet/npc.hpp"
#include "acnet/player_query.hpp"
#include "acnet/types.hpp"
#include "acnet/world.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace acnet {

enum class ResourceKind : std::uint8_t {
    Player,
    Npc,
    Tile,
    Clock,
    Weather,
    Shop,
    House,
    Event,
    /* Town-wide occupancy. Appended, never inserted: the wire encodes this
     * enum as a u8 and validates against the last value. */
    Town,
};

/* Town-wide occupancy, replicated so the count stays live between baselines.
 * Population 0 means "not reported"; capacity is never 0. */
struct TownOccupancy {
    std::uint8_t population = 0;
    std::uint8_t capacity = 1;
};

bool encode_town_delta(const TownOccupancy& occupancy, std::vector<std::uint8_t>& output);
bool decode_town_delta(const std::vector<std::uint8_t>& input, TownOccupancy& occupancy);

struct ZoneBaseline {
    Tick server_tick = 0;
    Revision revision = 0;
    ZoneId zone = 0;
    std::int64_t town_unix_seconds = 0;
    std::uint8_t weather = 0;
    std::uint8_t weather_intensity = 0;
    /* Town-wide, unlike players[] which is the viewer's interest set.
     * A population of 0 means "not reported" and readers must not render it;
     * capacity is always at least 1 so the pair is never nonsensical. */
    std::uint8_t town_population = 0;
    std::uint8_t town_capacity = 1;
    std::uint8_t occupied_house_mask = 0;
    bool has_house = false;
    HouseState house;
    InventoryState inventory;
    AccountLedger ledger;
    ShopState shop;
    std::vector<std::pair<TileAddress, TileState>> tiles;
    std::vector<PlayerSnapshot> players;
    std::vector<NpcState> npcs;
};

struct TileStateDelta {
    TileAddress address;
    TileState state;
};

bool encode_tile_delta(const TileStateDelta& delta, std::vector<std::uint8_t>& output);
bool decode_tile_delta(const std::vector<std::uint8_t>& input, TileStateDelta& delta);

bool encode_baseline(const ZoneBaseline& baseline, std::vector<std::uint8_t>& output);
bool decode_baseline(const std::vector<std::uint8_t>& input, ZoneBaseline& baseline);

struct ReplicationDelta {
    Revision revision = 0;
    ResourceKind kind = ResourceKind::Event;
    ZoneId zone = 0;
    AccountId target_account = 0;
    EntityId entity = 0;
    bool reliable = true;
    bool has_position = false;
    Vec3 position;
    std::vector<std::uint8_t> payload;
};

struct InterestContext {
    AccountId account = 0;
    ZoneId zone = 0;
    Vec3 position;
    bool exterior = false;
    float radius = 1200.0F;
};

struct DeltaQueryResult {
    bool requires_baseline = false;
    Revision newest_revision = 0;
    std::vector<ReplicationDelta> deltas;
};

class DeltaLog {
public:
    explicit DeltaLog(std::size_t capacity = 8192);

    Revision append(ReplicationDelta delta);
    DeltaQueryResult since(Revision after, const InterestContext& interest, std::size_t maximum) const;
    Revision current_revision() const { return revision_; }
    std::size_t size() const { return deltas_.size(); }

private:
    static bool relevant(const ReplicationDelta& delta, const InterestContext& interest);

    std::size_t capacity_;
    Revision revision_ = 0;
    std::deque<ReplicationDelta> deltas_;
};

ZoneBaseline build_baseline(ZoneId zone,
                            Tick tick,
                            Revision revision,
                            std::int64_t town_unix_seconds,
                            std::uint8_t weather,
                            std::uint8_t weather_intensity,
                            const WorldAuthority& world,
                            const PlayerDirectory& players,
                            const NpcAuthority& npcs);

} // namespace acnet
