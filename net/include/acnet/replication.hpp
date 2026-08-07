#pragma once

#include "acnet/economy.hpp"
#include "acnet/housing.hpp"
#include "acnet/npc.hpp"
#include "acnet/player_query.hpp"
#include "acnet/types.hpp"
#include "acnet/world.hpp"

#include <array>
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
    /* One recipient's mailbox. Always account-targeted, so it reaches the
     * addressee wherever they are standing. */
    Mail,
    /* Who occupies the town's four original resident slots. Town-wide, and not
     * derivable from the viewer's interest set: a resident who is logged out
     * still owns their house. */
    Resident,
};

/* Town-wide occupancy, replicated so the count stays live between baselines.
 * Population 0 means "not reported"; capacity is never 0. */
struct TownOccupancy {
    std::uint8_t population = 0;
    std::uint8_t capacity = 1;
};

bool encode_town_delta(const TownOccupancy& occupancy, std::vector<std::uint8_t>& output);
bool decode_town_delta(const std::vector<std::uint8_t>& input, TownOccupancy& occupancy);

/* One mailbox change. `removed` claims report only the identifier; a delivery
 * carries the whole letter so the recipient never has to ask for a baseline. */
struct MailDelta {
    AccountId account = 0;
    Revision mailbox_revision = 0;
    bool removed = false;
    MailRecord record;
};

bool encode_mail_delta(const MailDelta& delta, std::vector<std::uint8_t>& output);
bool decode_mail_delta(const std::vector<std::uint8_t>& input, MailDelta& delta);

/* One original resident slot. The client's own save only ever holds the account
 * it logged in as, so without this the other three houses read as vacant
 * everywhere the game asks Save_t who lives in a slot -- the town map most
 * visibly. `occupied` false means the slot is authoritatively empty, which is
 * different from "not reported": a roster is only ever sent whole. */
struct ResidentIdentity {
    AccountId account = 0;
    std::array<std::uint8_t, 8> name{};
    std::uint8_t gender = 0;
    bool occupied = false;
};

struct ResidentRoster {
    std::array<ResidentIdentity, kOriginalResidentSlots> slots{};

    bool operator==(const ResidentRoster& other) const;
    bool operator!=(const ResidentRoster& other) const { return !(*this == other); }
};

bool encode_resident_delta(const ResidentRoster& roster, std::vector<std::uint8_t>& output);
bool decode_resident_delta(const std::vector<std::uint8_t>& input, ResidentRoster& roster);

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
    std::uint8_t house_light_mask = 0;
    /* Town-wide, like town_population: who owns the four original houses,
     * whether or not they are logged in. Resident deltas keep it live. */
    ResidentRoster residents;
    bool has_house = false;
    HouseState house;
    InventoryState inventory;
    AccountLedger ledger;
    MailboxState mailbox;
    /* The viewer's own pending letters, in mailbox order. */
    std::vector<MailRecord> mail;
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
