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
    uint16_t equipped_item;
    uint8_t transition_phase;
    uint32_t transition_door;
    uint32_t transition_expires_tick;
} AcNetRemotePlayer;

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

typedef struct AcNetItemSlot {
    uint16_t item;
    uint8_t condition;
} AcNetItemSlot;

typedef struct AcNetTownBootstrapTile {
    uint16_t item;
    uint8_t buried;
} AcNetTownBootstrapTile;

typedef struct AcNetPlayerAppearance {
    uint8_t name[8];
    uint8_t gender;
    uint8_t face;
    uint16_t clothing;
    uint16_t equipped_item;
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
 * posted; `text` is a fixed-size, not necessarily terminated body. */
#define ACNET_MAIL_TEXT_BYTES 96
#define ACNET_MAILBOX_CAPACITY 10

typedef struct AcNetMailRecord {
    uint64_t id;
    uint64_t sender;
    uint64_t recipient;
    uint16_t attachment;
    uint32_t revision;
    uint8_t text[ACNET_MAIL_TEXT_BYTES];
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
int acnet_client_frame(int16_t stick_x,
                       int16_t stick_y,
                       uint16_t buttons,
                       uint16_t action,
                       AcNetTransform* local_transform);
size_t acnet_client_remote_players(AcNetRemotePlayer* output, size_t capacity);
size_t acnet_client_baseline_tiles(AcNetTileState* output, size_t capacity);
int acnet_client_tile(uint32_t zone_id, int16_t x, int16_t z, AcNetTileState* output);
uint32_t acnet_client_baseline_revision(void);
uint32_t acnet_client_baseline_zone(void);
uint8_t acnet_client_house_light_mask(void);
int acnet_client_house(AcNetHouseState* output);
size_t acnet_client_house_furniture(AcNetHouseFurniture* output, size_t capacity);
size_t acnet_client_inventory(AcNetItemSlot* output, size_t capacity);
uint32_t acnet_client_inventory_revision(void);
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
int acnet_client_submit_town_bootstrap(const uint8_t town_name[8],
                                       uint16_t land_id,
                                       const AcNetPlayerAppearance* appearance,
                                       const AcNetTownBootstrapTile* tiles,
                                       size_t tile_count);
int acnet_client_take_town_bootstrap_result(uint16_t* result_code,
                                            uint32_t* revision,
                                            uint8_t* initialized);
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
int acnet_client_request_world_with_tool_auto(uint8_t operation_type,
                                              uint32_t zone_id,
                                              int16_t x,
                                              int16_t z,
                                              uint32_t expected_tile_revision,
                                              uint32_t expected_inventory_revision,
                                              uint8_t inventory_slot,
                                              uint8_t tool_slot,
                                              uint16_t expected_item);
int acnet_client_take_world_result(AcNetWorldResult* output);
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
/* Pending mail. The revision is what a claim must quote; the letters arrive in
 * the baseline and stay current through account-targeted mailbox deltas. */
uint32_t acnet_client_mailbox_revision(void);
size_t acnet_client_mail(AcNetMailRecord* output, size_t capacity);
int acnet_client_claim_mail(uint64_t mail_id);
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
int acnet_client_request_encounter(uint8_t kind,
                                   uint32_t expected_inventory_revision,
                                   uint8_t tool_slot,
                                   uint64_t idempotency_high,
                                   uint64_t idempotency_low);
int acnet_client_request_encounter_auto(uint8_t kind,
                                        uint32_t expected_inventory_revision,
                                        uint8_t tool_slot);
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
