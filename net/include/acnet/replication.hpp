#pragma once

#include "acnet/economy.hpp"
#include "acnet/housing.hpp"
#include "acnet/npc.hpp"
#include "acnet/player_query.hpp"
#include "acnet/shop.hpp"
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
    /* What the museum already holds. Town-wide: one town has one collection,
     * and a donor needs to know a species is already displayed before offering
     * it. Carries the revision a Donate has to quote. */
    Museum,
    /* One house's exterior gyroid: the displayed items, the visitor message,
     * and the held proceeds. Town-wide, because the gyroids stand in the field
     * where every player can browse them, not inside the house whose baseline
     * would otherwise carry them. */
    Gyroid,
    /* The town's weekly turnip schedule. Town-wide: one town has one stalk
     * market, and every player must be quoted the same price for the same
     * turnip on the same day. */
    Turnip,
    /* The town tune. Town-wide, and audible to everyone: it plays on the hour
     * and at the gate, so a locally-set one means every player hears a
     * different town. */
    TownTune,
    /* The noticeboard. Town-wide by definition -- it exists so townmates can
     * leave each other notes, which a local copy cannot do. */
    Notice,
    /* The villager roster. Town-wide: one town has one set of neighbours, and
     * without this every client evolves its own from the second boot onward. */
    Villager,
};

bool encode_villager_delta(const VillagerRoster& roster, std::vector<std::uint8_t>& output);
bool decode_villager_delta(const std::vector<std::uint8_t>& input, VillagerRoster& roster);

/* One noticeboard post. The message is opaque bytes in the game's own font
 * encoding, sized to MAIL_BODY_LEN, like mail text -- nothing reinterprets
 * player-written content. `posted_time` is the game's own lbRTC_time_c bytes,
 * carried the same way for the same reason. */
constexpr std::size_t kNoticeMessageBytes = 192;
constexpr std::size_t kNoticeTimeBytes = 8;
constexpr std::size_t kNoticeBoardPosts = 15; // mNtc_BOARD_POST_COUNT

struct NoticePost {
    std::array<std::uint8_t, kNoticeMessageBytes> message{};
    std::array<std::uint8_t, kNoticeTimeBytes> posted_time{};

    bool operator==(const NoticePost& other) const {
        return message == other.message && posted_time == other.posted_time;
    }
};

/* Only the occupied posts, oldest first. The original marks an empty slot by
 * its timestamp matching the clear code; keeping the list dense here means the
 * server never has to know what that sentinel is, and the client fills the tail
 * with it as it projects. */
struct NoticeBoard {
    std::vector<NoticePost> posts;
    Revision revision = 1;
};

bool encode_notice_delta(const NoticeBoard& board, std::vector<std::uint8_t>& output);
bool decode_notice_delta(const std::vector<std::uint8_t>& input, NoticeBoard& board);

/* Appending is contested -- two players may post at once -- so the request
 * quotes the revision it saw. The server owns the eviction: at fifteen posts
 * the oldest drops, exactly as mNtc_notice_write shifts the array down. */
struct NoticePostRequest {
    AccountId account = 0;
    IdempotencyKey idempotency;
    Revision expected_revision = 0;
    NoticePost post;
};

struct NoticePostResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    Revision revision = 0;
    bool replayed = false;
};

/* Save_t::melody -- sixteen notes of four bits each, packed into a u64 exactly
 * as mMld_TransformMelodyData_u8_2_u64 packs them. Carried opaquely: every one
 * of the sixteen values a nibble can hold is a note the game will play, so
 * there is no out-of-range case to reject. */
struct TownTune {
    std::uint64_t notes = 0;
    Revision revision = 1;

    bool operator==(const TownTune& other) const {
        return notes == other.notes && revision == other.revision;
    }
};

bool encode_town_tune_delta(const TownTune& tune, std::vector<std::uint8_t>& output);
bool decode_town_tune_delta(const std::vector<std::uint8_t>& input, TownTune& tune);

/* Anyone may retune the town at the town hall, so this is contested state: the
 * request quotes the revision it saw and a stale one is refused, exactly as a
 * house or gyroid edit is. */
struct TownTuneUpdate {
    AccountId account = 0;
    IdempotencyKey idempotency;
    Revision expected_revision = 0;
    std::uint64_t notes = 0;
};

struct TownTuneResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    Revision revision = 0;
    std::uint64_t notes = 0;
    bool replayed = false;
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

/* One player's presentation changing: a new animation, or a different item in
 * the hand. Sent zone-scoped on the reliable Events channel rather than folded
 * into the snapshot, because a full 16-player snapshot already sits close to
 * the unfragmented MTU and because a lost transition would leave a viewer
 * holding the previous pose indefinitely. */
struct PlayerPresentationDelta {
    AccountId account = 0;
    EntityId entity = 0;
    PlayerPresentation presentation;
};

bool encode_player_delta(const PlayerPresentationDelta& delta, std::vector<std::uint8_t>& output);
bool decode_player_delta(const std::vector<std::uint8_t>& input, PlayerPresentationDelta& delta);

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

/* One resident house's gyroid changing. `original_slot` names the save-side
 * Save_t.homes[] entry the viewer projects it into. */
struct GyroidDelta {
    std::uint64_t house_id = 0;
    std::uint8_t original_slot = 0;
    GyroidState state;
};

/* The whole weekly schedule, which is 18 bytes -- small enough that there is
 * no reason to encode a single day and leave readers to merge. */
bool encode_turnip_delta(const TurnipMarket& market, std::vector<std::uint8_t>& output);
bool decode_turnip_delta(const std::vector<std::uint8_t>& input, TurnipMarket& market);

bool encode_gyroid_delta(const GyroidDelta& delta, std::vector<std::uint8_t>& output);
bool decode_gyroid_delta(const std::vector<std::uint8_t>& input, GyroidDelta& delta);

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
    /* Town-wide, like the shelf: what the museum already displays, so a donor
     * is not offered a duplicate and Donate has a revision to quote. */
    MuseumState museum;
    /* The four resident gyroids, slot-indexed. `occupied` false means the slot
     * has no registered house and the state beside it is meaningless. */
    struct GyroidEntry {
        bool occupied = false;
        std::uint64_t house_id = 0;
        GyroidState state;
    };
    std::array<GyroidEntry, kOriginalResidentSlots> gyroids{};
    /* Town-wide, like the shelf and the museum. */
    TurnipMarket turnips;
    TownTune town_tune;
    NoticeBoard notices;
    VillagerRoster villagers;
    std::vector<std::pair<TileAddress, TileState>> tiles;
    std::vector<PlayerSnapshot> players;
    std::vector<NpcState> npcs;
};

/* Why a tile changed. The viewer needs this to decide whether the change gets
 * an animation: a player dropping something arcs it out of their hand, while a
 * planted sapling growing overnight simply is. Mirrors WorldOpType with a
 * `Server` sentinel prepended so a change with no acting player -- growth, a
 * GCI import, an operator command -- defaults to the silent path. Appended,
 * never inserted: the wire encodes this as a u8 validated against the last
 * value. */
enum class TileChangeCause : std::uint8_t {
    Server = 0,
    Drop,
    Pickup,
    Dig,
    Bury,
    Plant,
    ChopTree,
    PlaceFurniture,
    RemoveFurniture,
    FillHole,
};

TileChangeCause tile_change_cause(WorldOpType type);

struct TileStateDelta {
    TileAddress address;
    TileState state;
    /* Who caused it, or 0 when the server did. A viewer animating the change
     * needs the actor's position to start the arc from, and the account is the
     * only handle that survives the trip -- the tile alone cannot say which of
     * two players standing together threw the item. */
    AccountId actor = 0;
    TileChangeCause cause = TileChangeCause::Server;
};

bool encode_tile_delta(const TileStateDelta& delta, std::vector<std::uint8_t>& output);
bool decode_tile_delta(const std::vector<std::uint8_t>& input, TileStateDelta& delta);

/* The shop shelf, town-wide. A purchase takes the item off everyone's shelf,
 * so the whole stock list is republished rather than the one changed row: it
 * is at most kMaximumShopEntries short entries, and a shelf assembled from
 * partial updates could disagree with the server about what index holds what.
 * The index is what a Buy request names, so it has to be exact. */
bool encode_shop_delta(const ShopState& shop, std::vector<std::uint8_t>& output);
bool decode_shop_delta(const std::vector<std::uint8_t>& input, ShopState& shop);

/* The museum's collection, town-wide. Sent whole for the same reason as the
 * shelf, and because a donor needs the full set to know what is already
 * displayed; the ids are sorted so the encoding is stable. */
bool encode_museum_delta(const MuseumState& museum, std::vector<std::uint8_t>& output);
bool decode_museum_delta(const std::vector<std::uint8_t>& input, MuseumState& museum);

/* One NPC's state changing. Zone-scoped, unlike the shelf: an NPC matters only
 * to players who can see it. `ResourceKind::Npc` was declared long before
 * anything produced one, so a viewer's NPC list only ever refreshed on a new
 * baseline. */
bool encode_npc_delta(const NpcState& npc, std::vector<std::uint8_t>& output);
bool decode_npc_delta(const std::vector<std::uint8_t>& input, NpcState& npc);

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
