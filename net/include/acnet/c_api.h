#ifndef ACNET_C_API_H
#define ACNET_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AcNetClientStatus {
    ACNET_OFFLINE = 0,
    ACNET_CONNECTING = 1,
    ACNET_CONNECTED = 2,
    ACNET_RECONNECTING = 3,
    ACNET_REJECTED = 4,
    ACNET_FAILED = 5
} AcNetClientStatus;

#define ACNET_CUSTOM_PATTERN_TEXTURE_BYTES 512

typedef struct AcNetTransform {
    float x;
    float y;
    float z;
    float velocity_x;
    float velocity_y;
    float velocity_z;
    int16_t yaw;
    uint16_t action;
} AcNetTransform;

typedef struct AcNetRemotePlayer {
    uint64_t entity_id;
    uint64_t account_id;
    uint32_t zone_id;
    AcNetTransform transform;
    uint8_t name[8];
    uint8_t gender;
    uint8_t face;
    uint16_t clothing;
    uint16_t clothing_index;
    uint32_t appearance_revision;
    uint8_t pattern_present;
    uint8_t pattern_palette;
    uint8_t pattern_texture[ACNET_CUSTOM_PATTERN_TEXTURE_BYTES];
    /* What this player is holding and what their skeleton is doing, in the
     * original game's own indices: mPlayer_ANIM_* for the two keyframes,
     * mPlayer_PART_TABLE_* for the blend, mPlayer_ITEM_MAIN_* for the tool.
     * Already range-checked by the decoder, so a viewer may index its tables
     * with them directly. */
    uint16_t equipped_item;
    uint8_t animation_body;
    uint8_t animation_overlay;
    uint8_t animation_part_table;
    uint8_t animation_item_state;
    uint8_t animation_looping;
    uint8_t animation_reversed;
    uint8_t transition_phase;
    uint32_t transition_door;
    uint32_t transition_expires_tick;
} AcNetRemotePlayer;

/* One original resident slot. `occupied` 0 means the slot is authoritatively
 * empty and the other fields are zero. */
typedef struct AcNetResident {
    uint64_t account_id;
    uint8_t name[8];
    uint8_t gender;
    uint8_t occupied;
} AcNetResident;

typedef struct AcNetHouseState {
    uint64_t house_id;
    uint64_t owner_account_id;
    uint32_t zone_id;
    uint32_t revision;
    uint8_t original_slot;
    uint8_t upgrade_level;
    uint8_t initialized;
    uint8_t main_light_on;
    uint8_t basement_light_on;
    int16_t music_tracks[3];
    uint64_t furniture_switches[12];
} AcNetHouseState;

typedef struct AcNetHouseFurniture {
    uint8_t x;
    uint8_t z;
    uint8_t floor;
    uint8_t layer;
    uint16_t item;
    uint8_t condition;
} AcNetHouseFurniture;

typedef struct AcNetHouseUpdateResult {
    uint16_t result_code;
    uint64_t house_id;
    uint32_t house_revision;
    uint8_t replayed;
} AcNetHouseUpdateResult;

typedef struct AcNetTileState {
    uint32_t zone_id;
    int16_t x;
    int16_t z;
    uint32_t revision;
    uint16_t item;
    uint8_t condition;
    uint8_t terrain;
    uint8_t buried;
    uint8_t placed_furniture;
} AcNetTileState;

/* Why a tile changed, mirroring acnet::TileChangeCause. ACNET_TILE_CAUSE_SERVER
 * means no player caused it -- overnight growth, a save import, an operator
 * command -- and must never be animated. */
#define ACNET_TILE_CAUSE_SERVER 0
#define ACNET_TILE_CAUSE_DROP 1
#define ACNET_TILE_CAUSE_PICKUP 2
#define ACNET_TILE_CAUSE_DIG 3
#define ACNET_TILE_CAUSE_BURY 4
#define ACNET_TILE_CAUSE_PLANT 5
#define ACNET_TILE_CAUSE_CHOP 6
#define ACNET_TILE_CAUSE_PLACE_FURNITURE 7
#define ACNET_TILE_CAUSE_REMOVE_FURNITURE 8
#define ACNET_TILE_CAUSE_FILL_HOLE 9

/* One tile change that arrived as a delta. `actor_account` is 0 exactly when
 * `cause` is ACNET_TILE_CAUSE_SERVER. */
typedef struct AcNetTileChange {
    AcNetTileState tile;
    uint64_t actor_account;
    uint8_t cause;
} AcNetTileChange;

typedef struct AcNetItemSlot {
    uint16_t item;
    uint8_t condition;
} AcNetItemSlot;

/* One row of Nook's shelf. `quantity` 0 means the row is sold out for today;
 * the row keeps its index so a purchase still names the same thing. */
typedef struct AcNetShopEntry {
    uint16_t item;
    uint32_t price;
    uint16_t quantity;
} AcNetShopEntry;

typedef struct AcNetTownBootstrapTile {
    uint16_t item;
    uint8_t buried;
} AcNetTownBootstrapTile;

typedef struct AcNetPlayerAppearance {
    uint8_t name[8];
    uint8_t gender;
    uint8_t face;
    uint16_t clothing;
    uint16_t clothing_index;
    uint32_t appearance_revision;
    uint8_t pattern_present;
    uint8_t pattern_palette;
    uint8_t pattern_texture[ACNET_CUSTOM_PATTERN_TEXTURE_BYTES];
} AcNetPlayerAppearance;

typedef struct AcNetWorldResult {
    uint16_t result_code;
    uint64_t idempotency_high;
    uint64_t idempotency_low;
    uint32_t zone_id;
    int16_t x;
    int16_t z;
    uint32_t tile_revision;
    uint32_t inventory_revision;
    uint16_t transferred_item;
    uint8_t inventory_slot;
    uint8_t replayed;
} AcNetWorldResult;

typedef struct AcNetEncounterResult {
    uint16_t result_code;
    uint32_t inventory_revision;
    uint16_t item;
    uint8_t inventory_slot;
    uint8_t caught;
    uint8_t replayed;
} AcNetEncounterResult;

/* Mirrors acnet::MailRecord. `sender` is 0 for a letter the town operator
 * posted. The text fields are opaque bytes in the game's own font encoding and
 * are sized to the matching Mail_c fields, so a letter can be projected into
 * the original UI without reinterpretation. `location` is 0 while the letter
 * waits in the house mailbox and 1 once the player is carrying it. */
#define ACNET_MAIL_NAME_BYTES 22
#define ACNET_MAIL_HEADER_BYTES 24
#define ACNET_MAIL_BODY_BYTES 192
#define ACNET_MAIL_FOOTER_BYTES 32
#define ACNET_MAILBOX_CAPACITY 10
#define ACNET_CARRIED_MAIL_CAPACITY 10
#define ACNET_MAIL_IN_MAILBOX 0
#define ACNET_MAIL_CARRIED 1

typedef struct AcNetMailRecord {
    uint64_t id;
    uint64_t sender;
    uint64_t recipient;
    uint16_t attachment;
    uint32_t revision;
    uint8_t location;
    uint8_t font;
    uint8_t mail_type;
    uint8_t paper_type;
    uint8_t header_back_start;
    uint8_t sender_name[ACNET_MAIL_NAME_BYTES];
    uint8_t header[ACNET_MAIL_HEADER_BYTES];
    uint8_t body[ACNET_MAIL_BODY_BYTES];
    uint8_t footer[ACNET_MAIL_FOOTER_BYTES];
} AcNetMailRecord;

typedef struct AcNetEconomyResult {
    uint16_t result_code;
    uint8_t operation_type;
    uint32_t inventory_revision;
    uint32_t auxiliary_revision;
    uint64_t balance;
    uint64_t debt;
    uint32_t bells;
    uint16_t item;
    uint8_t inventory_slot;
    uint64_t mail_id;
    uint8_t replayed;
} AcNetEconomyResult;

typedef struct AcNetTradeResult {
    uint16_t result_code;
    uint64_t trade_id;
    uint32_t trade_revision;
    uint32_t inventory_revision;
    uint8_t finalized;
} AcNetTradeResult;

typedef struct AcNetConversationResult {
    uint16_t result_code;
    uint64_t npc_entity;
    uint32_t lease_id;
    uint32_t dialogue_node;
    uint32_t revision;
    uint32_t expires_tick;
    uint8_t completed;
} AcNetConversationResult;

typedef struct AcNetTransferOffer {
    uint16_t result_code;
    uint32_t source_zone;
    uint32_t destination_zone;
    float destination_x;
    float destination_y;
    float destination_z;
    uint64_t token_high;
    uint64_t token_low;
    uint32_t expires_tick;
    uint32_t baseline_revision;
} AcNetTransferOffer;

typedef struct AcNetFurnitureResult {
    uint16_t result_code;
    uint64_t house_id;
    uint32_t house_revision;
    uint32_t inventory_revision;
    uint8_t inventory_slot;
    uint16_t item;
    uint8_t replayed;
} AcNetFurnitureResult;

int acnet_client_start(const char* host,
                       uint16_t port,
                       uint64_t town_id,
                       uint64_t account_id,
                       uint64_t build_id,
                       const char* invite_key);
void acnet_client_stop(void);
int acnet_client_poll(void);
/* `action` is mPlayer_INDEX_*; the four animation arguments are the local
 * player's own animation0_idx, animation1_idx, part_table_idx, and
 * now_item_main_index, with the keyframe's playback mode. They are what other
 * clients draw this player with, so they are range-checked before they are
 * sent and again when they arrive. */
int acnet_client_frame(int16_t stick_x,
                       int16_t stick_y,
                       uint16_t buttons,
                       uint16_t action,
                       uint8_t animation_body,
                       uint8_t animation_overlay,
                       uint8_t animation_part_table,
                       uint8_t animation_item_state,
                       uint8_t animation_looping,
                       uint8_t animation_reversed,
                       AcNetTransform* local_transform);
size_t acnet_client_remote_players(AcNetRemotePlayer* output, size_t capacity);
size_t acnet_client_baseline_tiles(AcNetTileState* output, size_t capacity);
int acnet_client_tile(uint32_t zone_id, int16_t x, int16_t z, AcNetTileState* output);
uint32_t acnet_client_baseline_revision(void);
uint32_t acnet_client_baseline_zone(void);
/* Counts whole baselines, unlike acnet_client_baseline_revision which moves for
 * every delta of every kind. A viewer projecting the whole interest chunk keys
 * on this so an unrelated animation change does not rewrite the field. */
uint32_t acnet_client_baseline_serial(void);
/* Drains tile changes that arrived as deltas, oldest first, removing exactly
 * what it copies. Returns the number written. */
size_t acnet_client_drain_tile_changes(AcNetTileChange* output, size_t capacity);
/* Non-zero when a change was evicted before the caller drained it; the caller
 * must then reproject the whole chunk. Cleared once the queue empties. */
int acnet_client_tile_changes_overflowed(void);
uint8_t acnet_client_house_light_mask(void);
int acnet_client_house(AcNetHouseState* output);
size_t acnet_client_house_furniture(AcNetHouseFurniture* output, size_t capacity);
size_t acnet_client_inventory(AcNetItemSlot* output, size_t capacity);
uint32_t acnet_client_inventory_revision(void);
/* The item this account is authoritatively holding, or 0 for an empty hand.
 * Shares the inventory revision: it moved out of a pocket to get there. */
uint16_t acnet_client_equipped_item(void);
/* Swap pocket slot `inventory_slot` with the hand. Equipping names the slot the
 * tool is in, putting away names an empty slot, and swapping tools names the
 * next one -- the same move in all three cases, so nothing is created or
 * destroyed. `expected_item` is the item the caller believes is in that slot,
 * or 0 to skip the check. */
int acnet_client_request_hold_item(uint8_t inventory_slot, uint16_t expected_item);
uint32_t acnet_client_bells(void);
uint64_t acnet_client_bank_balance(void);
uint64_t acnet_client_debt(void);
int64_t acnet_client_town_time(void);
uint8_t acnet_client_weather(void);
uint8_t acnet_client_weather_intensity(void);
uint32_t acnet_client_town_seed(void);
uint16_t acnet_client_town_land_id(void);
uint8_t acnet_client_resident_slot(void);
int acnet_client_town_initialized(void);
size_t acnet_client_town_name(uint8_t* output, size_t capacity);
/* Town-wide occupancy, unlike acnet_client_remote_players() which is the
 * viewer's interest set. Returns 0 when the server has reported no population
 * yet, leaving the outputs untouched. */
int acnet_client_town_population(uint8_t* population, uint8_t* capacity);
/* The four original resident slots, indexed by slot, filled from the newest
 * baseline or Resident delta. Unlike acnet_client_remote_players() this covers
 * residents who are logged out -- it is the town's ownership record, not a
 * presence list. Returns 0 (writing nothing) until a roster has arrived, which
 * the caller must distinguish from a roster whose slots are all vacant. */
size_t acnet_client_residents(AcNetResident* output, size_t capacity);
/* `island_tiles` is the two island acres in acre-major order, or NULL when the
 * client could not read the field's acre layout yet -- the server then leaves
 * the island uninitialized and adopts it from a later login. `island_block_x0`
 * and `island_block_x1` are the acre columns those two blocks occupy. */
/* `native_fruit` is Save_Get(fruit), the fruit this town grows; the server
 * prices it at a quarter of what a foreign fruit fetches. Pass 0 when it is
 * not known yet and the server keeps whatever it already has. */
int acnet_client_submit_town_bootstrap(const uint8_t town_name[8],
                                       uint16_t land_id,
                                       uint16_t native_fruit,
                                       const AcNetPlayerAppearance* appearance,
                                       const AcNetTownBootstrapTile* tiles,
                                       size_t tile_count,
                                       const AcNetTownBootstrapTile* island_tiles,
                                       size_t island_tile_count,
                                       uint8_t island_block_x0,
                                       uint8_t island_block_x1);
int acnet_client_take_town_bootstrap_result(uint16_t* result_code,
                                            uint32_t* revision,
                                            uint8_t* initialized);
/* Reliable cosmetic update. The authenticated server assigns the appearance
 * revision and republishes the bounded pattern bytes in zone baselines. */
int acnet_client_update_appearance(const AcNetPlayerAppearance* appearance);
int acnet_client_request_world(uint8_t operation_type,
                               uint32_t zone_id,
                               int16_t x,
                               int16_t z,
                               uint32_t expected_tile_revision,
                               uint32_t expected_inventory_revision,
                               uint8_t inventory_slot,
                               uint16_t expected_item,
                               uint64_t idempotency_high,
                               uint64_t idempotency_low);
int acnet_client_request_world_auto(uint8_t operation_type,
                                    uint32_t zone_id,
                                    int16_t x,
                                    int16_t z,
                                    uint32_t expected_tile_revision,
                                    uint32_t expected_inventory_revision,
                                    uint8_t inventory_slot,
                                    uint16_t expected_item);
int acnet_client_take_world_result(AcNetWorldResult* output);
/* Nook's shelf as the server rolled it, in the order a Buy request indexes.
 * Pass NULL to learn how many rows there are. Zero rows means the client has no
 * baseline yet, not an empty shop. */
size_t acnet_client_shop_stock(AcNetShopEntry* output, size_t capacity);
/* The revision a Buy must quote. Zero before the first baseline. */
uint32_t acnet_client_shop_revision(void);
/* The spotlight rare furniture, which also appears in the stock list. Zero at
 * the tiers that stock no rare item. */
uint16_t acnet_client_shop_rare_item(void);
/* An NPC the server owns, in the zone being viewed. Distinct from a remote
 * player: villagers are still simulated client-side and do not appear here. */
typedef struct AcNetNpcState {
    uint64_t entity;
    uint32_t zone;
    uint32_t revision;
    float x, y, z;
    int16_t yaw;
    uint16_t schedule_state;
    uint16_t animation;
    uint16_t emotion;
} AcNetNpcState;

/* Server-owned NPCs in the viewed zone. Pass NULL to learn how many. */
size_t acnet_client_npcs(AcNetNpcState* output, size_t capacity);

/* The revision a Donate must quote, and whether the museum already displays an
 * item. Zero / false before the first baseline. */
uint32_t acnet_client_museum_revision(void);
int acnet_client_museum_has(uint16_t item);
/* Sell every pocket whose bit is set, as one transaction quoting the current
 * inventory revision. The counter sells a whole selection and quotes one total,
 * which per-slot requests cannot express: they would each have to quote the
 * revision the previous one produced. */
int acnet_client_request_sell(uint16_t slot_mask);
int acnet_client_request_economy_auto(uint8_t operation_type,
                                      uint32_t expected_inventory_revision,
                                      uint32_t expected_aux_revision,
                                      uint32_t shop_index,
                                      uint8_t inventory_slot,
                                      uint16_t expected_item,
                                      uint64_t amount,
                                      uint64_t recipient,
                                      uint64_t mail_id);
int acnet_client_take_economy_result(AcNetEconomyResult* output);
/* The bank ledger revision a deposit, withdrawal, or debt payment must quote.
 * acnet_client_bank_balance()/acnet_client_debt() report the same ledger. */
uint32_t acnet_client_bank_revision(void);
/* The account's letters, mailbox entries first then carried ones, as one list
 * ordered the way the server holds them. The revision covers both halves and is
 * what every mail request must quote; the letters arrive in the baseline and
 * stay current through account-targeted mail deltas.
 *
 * The two steps match the original game: take a letter out of the mailbox, then
 * take its present out of the carried letter. Discarding is only allowed once a
 * letter no longer holds a present. */
uint32_t acnet_client_mailbox_revision(void);
size_t acnet_client_mail(AcNetMailRecord* output, size_t capacity);
int acnet_client_take_mail(uint64_t mail_id);
int acnet_client_claim_mail(uint64_t mail_id);
int acnet_client_discard_mail(uint64_t mail_id);
int acnet_client_request_trade(uint8_t action,
                               uint64_t trade_id,
                               uint64_t other_account,
                               uint32_t expected_trade_revision,
                               const uint8_t* slots,
                               size_t slot_count);
int acnet_client_take_trade_result(AcNetTradeResult* output);
int acnet_client_request_conversation(uint8_t action,
                                      uint64_t npc_entity,
                                      uint32_t lease_id,
                                      uint16_t choice);
int acnet_client_take_conversation_result(AcNetConversationResult* output);
/* `species` is the item the client observed itself hooking or swinging at.
 * Spawns are still simulated on the client, so the server treats it as a claim:
 * it is committed only if that species can legally appear at the town's current
 * month, hour, and weather. Pass 0 to let the server choose.
 *
 * The rod or net is not named: the server reads whatever this account is
 * authoritatively holding, so a tool sitting in a pocket cannot catch. */
int acnet_client_request_encounter(uint8_t kind,
                                   uint16_t species,
                                   uint32_t expected_inventory_revision,
                                   uint64_t idempotency_high,
                                   uint64_t idempotency_low);
int acnet_client_request_encounter_auto(uint8_t kind,
                                        uint16_t species,
                                        uint32_t expected_inventory_revision);
int acnet_client_take_encounter_result(AcNetEncounterResult* output);
int acnet_client_request_furniture_auto(uint8_t operation_type,
                                        uint64_t house_id,
                                        uint8_t x,
                                        uint8_t z,
                                        uint8_t layer,
                                        uint32_t expected_house_revision,
                                        uint32_t expected_inventory_revision,
                                        uint8_t inventory_slot,
                                        uint16_t expected_item);
int acnet_client_take_furniture_result(AcNetFurnitureResult* output);
int acnet_client_submit_house_update(uint64_t house_id,
                                     uint32_t expected_house_revision,
                                     uint8_t upgrade_level,
                                     uint8_t main_light_on,
                                     uint8_t basement_light_on,
                                     const int16_t music_tracks[3],
                                     const uint64_t furniture_switches[12],
                                     const AcNetHouseFurniture* furniture,
                                     size_t furniture_count);
int acnet_client_take_house_update_result(AcNetHouseUpdateResult* output);
int acnet_client_request_zone_transfer(uint32_t door_id);
int acnet_client_zone_ready(uint64_t token_high,
                            uint64_t token_low,
                            const AcNetTransform* destination_transform);
int acnet_client_take_transfer_offer(AcNetTransferOffer* output);
AcNetClientStatus acnet_client_status(void);
uint64_t acnet_client_account(void);
uint64_t acnet_client_entity(void);
uint32_t acnet_client_server_tick(void);
const char* acnet_client_last_error(void);

uint64_t acnet_actor_created(const void* actor, int16_t profile, int16_t scene);
void acnet_actor_destroyed(const void* actor);
uint64_t acnet_actor_entity(const void* actor);
void acnet_scene_loaded(int16_t scene);

#ifdef __cplusplus
}
#endif

#endif
