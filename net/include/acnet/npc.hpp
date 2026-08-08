#pragma once

#include "acnet/player_query.hpp"
#include "acnet/world.hpp"
#include "acnet/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace acnet {

struct NpcState {
    EntityId entity = 0;
    ZoneId zone = 0;
    Transform transform;
    std::uint16_t schedule_state = 0;
    std::uint16_t animation = 0;
    std::uint16_t emotion = 0;
    EntityId destination = 0;
    Revision revision = 1;
    /* Who is currently holding this NPC in conversation, or 0 for nobody.
     * Replicated so a viewer can tell that a villager is busy *without* a round
     * trip: the check happens when a player presses A, and a request that had
     * to wait for an answer would either stall the interaction or start a
     * conversation it then had to unwind. */
    AccountId conversation_owner = 0;
};

struct NpcInteractionLease {
    AccountId owner = 0;
    EntityId npc = 0;
    std::uint32_t lease_id = 0;
    Tick expires_tick = 0;
};

struct ConversationState {
    NpcInteractionLease lease;
    std::uint32_t dialogue_node = 1;
    Revision revision = 1;
    bool active = false;
};

struct ConversationResult {
    ResultCode code = ResultCode::InternalError;
    EntityId npc = 0;
    std::uint32_t lease_id = 0;
    std::uint32_t dialogue_node = 0;
    Revision revision = 0;
    Tick expires_tick = 0;
    bool completed = false;
};

struct EventLease {
    std::uint32_t event_id = 0;
    AccountId owner = 0;
    Tick expires_tick = 0;
};

using DialogueResolver = std::function<std::optional<std::uint32_t>(EntityId npc,
                                                                   AccountId player,
                                                                   std::uint32_t node,
                                                                   std::uint16_t choice)>;

struct NpcConfig {
    float interaction_radius = 100.0F;
    Tick lease_duration_ticks = 600;
};

class NpcAuthority {
public:
    explicit NpcAuthority(PlayerDirectory* players, NpcConfig config = {});

    bool add_npc(const NpcState& npc);
    bool remove_npc(EntityId entity);
    NpcState* npc(EntityId entity);
    const NpcState* npc(EntityId entity) const;
    const ConversationState* conversation(EntityId entity) const;
    std::vector<NpcState> zone_snapshot(ZoneId zone) const;
    const std::unordered_map<EntityId, NpcState>& all_npcs() const { return npcs_; }

    ConversationResult request_conversation(AccountId account, EntityId npc, Tick tick);
    ConversationResult advance_conversation(AccountId account,
                                             EntityId npc,
                                             std::uint32_t lease_id,
                                             std::uint16_t choice,
                                             Tick tick);
    bool release_conversation(AccountId account, EntityId npc, std::uint32_t lease_id);
    std::size_t release_player(AccountId account);
    /* Which NPCs this account currently holds, so a caller can republish them
     * after releasing -- release_player reports a count, not the identities. */
    std::vector<EntityId> leases_held_by(AccountId account) const;
    /* Which leases `tick` is about to expire, for the same reason. */
    std::vector<EntityId> expiring_leases(Tick tick) const;
    std::size_t expire(Tick tick);

    bool acquire_event(std::uint32_t event_id, AccountId account, Tick tick, Tick duration);
    bool release_event(std::uint32_t event_id, AccountId account);
    const EventLease* active_event() const { return event_.has_value() ? &*event_ : nullptr; }

    const PlayerView* nearest_player(EntityId npc, float radius) const;
    void set_dialogue_resolver(DialogueResolver resolver) { resolver_ = std::move(resolver); }

private:
    bool player_in_range(AccountId account, const NpcState& npc) const;
    std::uint32_t next_lease_id();
    static Revision next_revision(Revision revision);

    PlayerDirectory* players_;
    NpcConfig config_;
    DialogueResolver resolver_;
    std::unordered_map<EntityId, NpcState> npcs_;
    std::unordered_map<EntityId, ConversationState> conversations_;
    std::optional<EventLease> event_;
    std::uint32_t lease_counter_ = 0;
};


/* ---------------------------------------------------------------------------
 * Villagers.
 *
 * Save_t.animals[] was entirely client-local, and that is the single largest
 * source of permanent divergence in a shared town: mSDI_OnlineTownGenerationBegin
 * seeds the roll from the town seed, so a *fresh* save on every machine
 * generates the same fifteen villagers -- and from the next boot onward each
 * client evolves its own copy alone. mNpc_Grow moves somebody in behind a local
 * RANDOM(100), move-outs are chosen from local dialogue, and clothes, mood and
 * catchphrases drift independently. Two players in the same town stop agreeing
 * about who lives there.
 *
 * This carries the roster: who lives here, what they look like, where their
 * house is, what they are wearing and how they feel. It deliberately does not
 * carry Animal_c::memories -- the per-player relationship records, which are
 * 7/8ths of the struct's 0x988 bytes and are account-scoped rather than
 * town-scoped. Those are a separate phase; see CURRENT_STATUS.md.
 * ------------------------------------------------------------------------ */
constexpr std::size_t kVillagerSlots = 15;      // ANIMAL_NUM_MAX
constexpr std::size_t kVillagerNameBytes = 8;   // LAND_NAME_SIZE / PLAYER_NAME_LEN
constexpr std::size_t kVillagerCatchphraseBytes = 10; // ANIMAL_CATCHPHRASE_LEN
/* mNpc_LOOKS_NUM: the personality count. mNpc_LOOKS_UNSET is the last value and
 * marks a slot with no villager, so it is accepted as well. */
constexpr std::uint8_t kVillagerLooksUnset = 6;

struct VillagerIdentity {
    /* AnmPersonalID_c: which character this is, and which town they came from.
     * npc_id is the species/character; looks is the personality. */
    std::uint16_t npc_id = 0;
    std::uint16_t land_id = 0;
    std::array<std::uint8_t, kVillagerNameBytes> land_name{};
    std::uint8_t name_id = 0;
    std::uint8_t looks = kVillagerLooksUnset;
    /* Anmhome_c: the acre and unit their house stands on. */
    std::uint8_t home_block_x = 0;
    std::uint8_t home_block_z = 0;
    std::uint8_t home_ut_x = 0;
    std::uint8_t home_ut_z = 0;
    /* Opaque bytes in the game's own font encoding, like mail text. */
    std::array<std::uint8_t, kVillagerCatchphraseBytes> catchphrase{};
    std::uint16_t cloth = 0;
    std::uint16_t present_cloth = 0;
    std::uint8_t cloth_original_id = 0xFF;
    std::uint8_t umbrella_id = 0xFF;
    std::uint8_t mood = 0;
    std::uint8_t mood_time = 0;
    std::uint8_t is_home = 0;
    std::uint8_t moved_in = 0;
    std::uint8_t removing = 0;
    std::uint16_t previous_land_id = 0;
    std::array<std::uint8_t, kVillagerNameBytes> previous_land_name{};
    std::array<std::uint8_t, kVillagerNameBytes> parent_name{};
    /* How this villager feels about each of the others, 128 being neutral. */
    std::array<std::uint8_t, kVillagerSlots> relations{};

    bool operator==(const VillagerIdentity& other) const {
        return npc_id == other.npc_id && land_id == other.land_id && land_name == other.land_name &&
               name_id == other.name_id && looks == other.looks && home_block_x == other.home_block_x &&
               home_block_z == other.home_block_z && home_ut_x == other.home_ut_x &&
               home_ut_z == other.home_ut_z && catchphrase == other.catchphrase && cloth == other.cloth &&
               present_cloth == other.present_cloth && cloth_original_id == other.cloth_original_id &&
               umbrella_id == other.umbrella_id && mood == other.mood && mood_time == other.mood_time &&
               is_home == other.is_home && moved_in == other.moved_in && removing == other.removing &&
               previous_land_id == other.previous_land_id && previous_land_name == other.previous_land_name &&
               parent_name == other.parent_name && relations == other.relations;
    }
};

struct VillagerSlot {
    bool occupied = false;
    VillagerIdentity villager;

    bool operator==(const VillagerSlot& other) const {
        return occupied == other.occupied && (!occupied || villager == other.villager);
    }
};

/* A move-in in progress. The server owns the *decision* -- when somebody is due
 * and which slot they take -- but not the villager: picking one needs the
 * character, name and personality tables, which the server holds none of and is
 * not allowed to. So it publishes the opening and a seed, and the first client
 * to run the game's own roll against that seed supplies the newcomer. Every
 * connected client may try; the first accepted request clears the opening and
 * the rest are refused as stale, which needs no election. */
struct VillagerMoveIn {
    bool pending = false;
    std::uint8_t slot = 0;
    std::uint32_t seed = 0;
};

struct VillagerRoster {
    std::array<VillagerSlot, kVillagerSlots> slots{};
    VillagerMoveIn move_in;
    /* Town time of the last move-in, so the one-per-day rule is the town's and
     * not fifteen private clocks. 0 means none yet. */
    std::int64_t last_move_in_unix = 0;
    Revision revision = 1;
    /* False until a client has handed over a generated roster. The server does
     * not invent villagers: it has no name, species or personality tables and
     * is not allowed to hold them, so the first resident's town generation is
     * the source and the server owns it from then on. */
    bool initialized = false;

    bool operator==(const VillagerRoster& other) const {
        return slots == other.slots && revision == other.revision && initialized == other.initialized &&
               move_in.pending == other.move_in.pending && move_in.slot == other.move_in.slot &&
               move_in.seed == other.move_in.seed && last_move_in_unix == other.last_move_in_unix;
    }
};

/* An occupied slot must name a villager; a vacant one must be entirely zero, so
 * a peer cannot smuggle an identity past a reader that only consults the flag.
 * The same rule the resident roster follows. */
bool valid_villager_slot(const VillagerSlot& slot);

/* A villager's server entity is derived from its roster slot rather than looked
 * up, so both ends compute the same identity from the same roster with no table
 * to keep in step. The base is clear of the service NPCs the runtime registers
 * (1000 shopkeeper, 1001 islander). */
constexpr EntityId kVillagerEntityBase = 2000;
constexpr EntityId villager_entity(std::size_t slot) {
    return kVillagerEntityBase + static_cast<EntityId>(slot);
}

/* One villager's live pose, as the simulating client sees it.
 *
 * Villagers cannot be simulated server-side: their movement needs pathfinding,
 * collision and the schedule tables, none of which the asset-free server has.
 * But every client running the AI independently is worse than a wrong answer --
 * it is fifteen different answers, so two players standing together watch the
 * same villager walk in different directions and "meet me at Bob's" means
 * nothing.
 *
 * So one connection is designated to simulate, exactly as the first resident is
 * designated to generate the roster, and reports what its AI produced. This is
 * the same trust already extended to a client for its own player movement, and
 * it buys the only thing that matters here: everybody sees the same town. */
struct NpcPose {
    EntityId entity = 0;
    Vec3 position;
    std::int16_t yaw = 0;
    std::uint16_t animation = 0;
    std::uint8_t schedule_state = 0;
};

struct NpcPoseUpdate {
    AccountId account = 0;
    ZoneId zone = 0;
    std::vector<NpcPose> poses;
};

/* A villager's memory of one player: who they are, when they last spoke, the
 * friendship, the saved letter. Carried as the game's own 312-byte record --
 * opaque, like mail text and catchphrases, because nothing here reinterprets
 * player-written content and the server has no reason to understand it.
 *
 * Account-scoped, not town-wide: a villager's memory of player A is only ever
 * read when A is talking to them. It has to be server state all the same,
 * because it is the *only* record that a player and a villager have a history,
 * and the local save that used to hold it is going away -- the extended
 * residents plan boots the client from a server baseline with no local save at
 * all, at which point an unreplicated memory is a relationship that silently
 * resets every login. */
constexpr std::size_t kVillagerMemoryBytes = 312; // sizeof(Anmmem_c)

/* The town's scheduled special visitor -- Redd, Saharah, Katrina, the designer,
 * the artist, the sale -- as mEv_special_c.
 *
 * Every client rolled its own, so the *contents* diverged even when the date
 * did not: two players walked into Redd's tent and were offered different
 * paintings, and the flags recording who had already used him were private to
 * each machine. The schedule dates come from town-seeded common data so the
 * events themselves lined up, which is exactly why this went unnoticed.
 *
 * `kind` is validated because the game indexes a table with it; the rest is
 * carried opaquely, like mail text -- it is the game's own POD and the server
 * has no reason to understand which painting is a forgery.
 *
 * The payload covers the whole of mEv_event_save_c beyond those two fields: the
 * per-event union, the weekly block, and the event flags. The flags matter as
 * much as the visitor does -- mEv_CheckFirstJob and the Halloween status are
 * read from them, and they gate what villagers do and which dialogue runs, so
 * leaving them local would have kept the towns diverging in a way the visitor
 * alone did not explain. */
constexpr std::size_t kSpecialEventPayloadBytes = 256; // >= mEv_special_u + mEv_weekly_u + flags
constexpr std::size_t kSpecialEventTimeBytes = 8;      // sizeof(lbRTC_time_c)
/* mEv_SPNPC_END: the last special-NPC event. 0xFFFFFFFF means none scheduled. */
constexpr std::uint32_t kMaximumSpecialEventKind = 6;
constexpr std::uint32_t kNoSpecialEvent = 0xFFFFFFFFu;

struct SpecialEvent {
    std::uint32_t kind = kNoSpecialEvent;
    std::array<std::uint8_t, kSpecialEventTimeBytes> scheduled{};
    std::array<std::uint8_t, kSpecialEventPayloadBytes> payload{};
    Revision revision = 1;

    bool operator==(const SpecialEvent& other) const {
        return kind == other.kind && scheduled == other.scheduled && payload == other.payload &&
               revision == other.revision;
    }
};

struct SpecialEventUpdate {
    AccountId account = 0;
    IdempotencyKey idempotency;
    Revision expected_revision = 0;
    SpecialEvent event;
};

struct SpecialEventResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    Revision revision = 0;
    bool replayed = false;
};

struct VillagerMemory {
    bool present = false;
    std::array<std::uint8_t, kVillagerMemoryBytes> data{};

    bool operator==(const VillagerMemory& other) const {
        return present == other.present && (!present || data == other.data);
    }
};

struct VillagerMemories {
    std::array<VillagerMemory, kVillagerSlots> slots{};
    Revision revision = 1;

    bool operator==(const VillagerMemories& other) const {
        return slots == other.slots && revision == other.revision;
    }
};

struct VillagerMemoryUpdate {
    AccountId account = 0;
    IdempotencyKey idempotency;
    Revision expected_revision = 0;
    VillagerMemories memories;
};

struct VillagerMemoryResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    Revision revision = 0;
    bool replayed = false;
};

enum class VillagerOpType : std::uint8_t {
    /* A client supplies the newcomer for the opening the server published. */
    MoveIn,
    /* A client reports that a villager announced they are leaving. The original
     * decides this from dialogue -- remove_exp accumulated from how a player
     * has treated them -- which is client-side state; the server owns what
     * follows, which is the slot actually emptying a day later. */
    AnnounceMoveOut,
};

constexpr std::uint8_t kMaximumVillagerOp = static_cast<std::uint8_t>(VillagerOpType::AnnounceMoveOut);

struct VillagerRequest {
    VillagerOpType type = VillagerOpType::MoveIn;
    AccountId account = 0;
    IdempotencyKey idempotency;
    Revision expected_revision = 0;
    std::uint8_t slot = 0;
    VillagerIdentity villager; /* MoveIn only */
};

struct VillagerResult {
    ResultCode code = ResultCode::InternalError;
    IdempotencyKey idempotency;
    Revision revision = 0;
    std::uint8_t slot = 0;
    bool replayed = false;
};

} // namespace acnet
