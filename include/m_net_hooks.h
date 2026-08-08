#ifndef M_NET_HOOKS_H
#define M_NET_HOOKS_H

#include "m_actor.h"
#include "m_play.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NETCODE_ENABLED
void Net_PreSimulation(GAME_PLAY* play);
void Net_PostSimulation(GAME_PLAY* play);
void Net_OnActorCreated(ACTOR* actor);
void Net_OnActorDestroyed(ACTOR* actor);
void Net_OnSceneLoaded(GAME_PLAY* play);
void Net_BeginSceneTransition(GAME_PLAY* play, int next_scene);
int Net_ApplyHouseStateBeforeRoom(GAME_PLAY* play);
int Net_HouseMainLightOn(int house_index);
int Net_IsConnected(void);
int Net_IsOnline(void);
/* TRUE only while the server holds authoritative tiles for the player's current
 * position, which today means the exterior. Interiors keep the original local
 * path: their floor items ride the room-state submit instead of the tile
 * table, and the server has no tile storage for those zones. Call sites must
 * branch on this rather than Net_IsConnected() so an indoor action is not
 * turned into a doomed exterior tile request and silently dropped. */
int Net_WorldTilesAuthoritative(void);
int Net_ConfigureQuickstart(const char* name, int gender);
int Net_QuickstartEnabled(void);
int Net_ResidentSlot(void);
/* Who owns original resident slot `slot`, which a connected client cannot read
 * out of Save_t: its private_data only ever holds the account it logged in as,
 * so every other slot looks vacant. Returns -1 when there is no authoritative
 * roster and the caller must fall back to Save_t, 0 when the slot is
 * authoritatively empty, and 1 after writing PLAYER_NAME_LEN bytes to `name`
 * and the gender to `gender` (either may be NULL). */
int Net_ResidentIdentity(int slot, u8* name, s8* gender);
int Net_PrefillQuickstartName(void);
int Net_ApplyQuickstartIdentity(void);
void Net_ApplyTownIdentity(void);
void Net_RandomizeInitialAppearance(void);
int Net_SubmitInitialTown(void);
int Net_RequestPickup(const xyz_t* position, mActor_name_t item);
int Net_RequestDrop(int ut_x, int ut_z, mActor_name_t item);
/* Claims a tile for a drop animation the caller is about to start, so the
 * authoritative tile projection leaves that cell alone until the item lands --
 * the drop actor writes the field itself when it touches down, and painting the
 * item in early would show it on the ground while it is still in the air.
 * Returns FALSE when no claim slot is free, in which case the caller must skip
 * the animation and let the projection place the item. */
int Net_BeginPredictedDrop(int ut_x, int ut_z, mActor_name_t item);
/* The same claim for a pickup, whose prediction is the opposite: the caller
 * clears the cell immediately and the claim keeps the projection from painting
 * the item back before the server confirms it is gone. */
int Net_BeginPredictedPickup(const xyz_t* position);
/* Drops a claim whose animation never started, so the authoritative projection
 * takes the tile back instead of leaving it untouched for the whole budget. */
void Net_CancelPredictedTile(int ut_x, int ut_z);
int Net_RequestDig(const xyz_t* position);
int Net_RequestFillHole(const xyz_t* position);
int Net_RequestBury(const xyz_t* position, mActor_name_t item, int inventory_slot);
int Net_RequestPlant(const xyz_t* position, mActor_name_t item, int inventory_slot);
int Net_RequestChop(const xyz_t* position);
/* `species` is the item the player just hooked or swung at. The server accepts
 * it only if that species can legally appear right now, and it alone decides
 * whether the catch lands, so the encyclopedia entry is not written here — see
 * Net_EncounterRecordsPending(). */
int Net_RequestEncounter(int kind, mActor_name_t species);
/* TRUE while the server owns what this player is holding, which is any online
 * session. Private_c::equipment is then a projection of the authoritative
 * inventory, refreshed whenever the server reports a change, so nothing may
 * write to it locally -- Net_RequestHoldItem is how an item reaches the hand.
 * Equipping used to be a purely local move out of a pocket, which the next
 * authoritative projection undid by restoring the pocket while the tool was
 * still held, duplicating it. */
int Net_EquipmentAuthoritative(void);
/* Swap pocket slot `inventory_slot` with the hand: equipping names the slot the
 * item is in, putting away names an empty slot, and swapping tools names the
 * next one. `item` is what the caller believes is in that slot, or EMPTY_NO to
 * skip the check. */
int Net_RequestHoldItem(int inventory_slot, mActor_name_t item);
/* TRUE once a request is in flight, meaning the caller must leave the
 * encyclopedia alone: the original writes it the instant the animation starts,
 * which would record a fish the server may still refuse. The hook writes it
 * when the accepted result arrives, using the species the server committed. */
int Net_EncounterRecordsPending(void);
/* TRUE while the bank ledger is server-owned, which is any online session. The
 * savings and loan overlays must branch on this: their local commit writes the
 * bank balance, the loan, the wallet, and money-sack items straight into the
 * save, and the authoritative apply overwrites the first three every time the
 * server reports a change, so the two would fight. Online they send the
 * requests below instead and let the accepted result move the money. */
int Net_BankingAuthoritative(void);
/* Positive deposits, negative withdraws. Both quote the observed ledger and
 * inventory revisions; the server refuses a stale one. */
int Net_RequestBankTransfer(int amount);
int Net_RequestPayDebt(u32 amount);
/* TRUE while the server owns this player's letters. The house mailbox array and
 * the carried mail array are then projections of authoritative state, refreshed
 * whenever the server reports a change, so nothing may write to them locally --
 * the three requests below are how a letter moves. */
int Net_MailAuthoritative(void);
/* Identifier of the letter currently projected into slot `index` of the house
 * mailbox / the carried mail array, or 0 if that slot is empty. */
u64 Net_MailboxMailId(int index);

/* TRUE once the server has reported a shelf. Save_Get(shop).items is then a
 * projection of it and must not be rolled locally: the row index is what a
 * purchase names, so a locally rolled shelf would buy the wrong thing. */
int Net_ShopStockAuthoritative(void);
void Net_ApplyAuthoritativeShopStock(void);

/* TRUE when the server owns the pockets and the wallet, which it does whenever
 * a baseline has arrived. Local bell and pocket writes are pointless then --
 * the next projection overwrites them. */
int Net_EconomyAuthoritative(void);
/* Sell the submenu's first `count` selected items in one transaction. */
int Net_RequestSellItems(GAME_PLAY* play, int count);
/* Buy one shelf item. The row index comes from the projected shelf. */
int Net_RequestBuyItem(mActor_name_t item);
/* Donate a pocket item to the town's museum. */
int Net_RequestDonate(int inventory_slot, mActor_name_t item);
u64 Net_CarriedMailId(int index);
/* Mailbox to pocket, present out of a carried letter, and throwing an emptied
 * letter away -- the same three steps the original UI performs locally. */
int Net_RequestTakeMail(u64 mail_id);
int Net_RequestClaimMail(u64 mail_id);
int Net_RequestDiscardMail(u64 mail_id);

/* TRUE once the server has reported the four resident gyroids.
 * Save_Get(homes[i]).haniwa is then a projection of authoritative state: the
 * owner's edits are captured and submitted whole when the submenu closes, and
 * the two contested moves below are server transactions. */
int Net_GyroidAuthoritative(void);
/* A guest takes (and pays for) display slot `item_slot` of house `house_idx`'s
 * gyroid. The local mutation may proceed optimistically; the projection
 * settles it when the result and the gyroid delta land. */
int Net_RequestGyroidTake(int house_idx, int item_slot, mActor_name_t item);
/* The owner empties the gyroid's sale proceeds into their wallet. */
int Net_RequestGyroidCollect(int house_idx);

/* TRUE once the town has reported a turnip schedule. Save_Get(kabu_price_schedule)
 * is then a projection of it and must not be rolled locally: one town has one
 * stalk market, and a locally rolled week quotes every player a different price
 * for the same turnip. */
int Net_TurnipMarketAuthoritative(void);
/* Today's authoritative price for one turnip stack, matching what the server
 * will actually pay for it. 0 when there is no schedule. */
u32 Net_TurnipSellPrice(mActor_name_t item);
/* Projects the authoritative schedule into the save so Kabu_get_price and the
 * shop dialogue read it without further plumbing. */
void Net_ApplyAuthoritativeTurnipMarket(void);

/* TRUE once the town has reported its tune. Save_Get(melody) is then a
 * projection: one town has one tune and everybody hears it on the hour, so a
 * locally set one would have every player hearing a different town. */
int Net_TownTuneAuthoritative(void);
/* Retune the town. The accepted result and the broadcast both settle it. */
int Net_RequestTownTune(u64 notes);
void Net_ApplyAuthoritativeTownTune(void);

/* TRUE once the town has reported its noticeboard. Save_Get(noticeboard) is
 * then a projection: the board exists so townmates can leave each other notes,
 * which a local copy cannot do. */
int Net_NoticeBoardAuthoritative(void);
/* Append a post. The server owns the eviction when the board is full, so two
 * players posting at once cannot each drop a different old post. */
int Net_RequestNoticePost(const void* post, u32 size);
void Net_ApplyAuthoritativeNotices(void);
#else
#define Net_PreSimulation(play) ((void)0)
#define Net_PostSimulation(play) ((void)0)
#define Net_OnActorCreated(actor) ((void)0)
#define Net_OnActorDestroyed(actor) ((void)0)
#define Net_OnSceneLoaded(play) ((void)0)
#define Net_BeginSceneTransition(play, next_scene) ((void)0)
#define Net_ApplyHouseStateBeforeRoom(play) FALSE
#define Net_HouseMainLightOn(house_index) FALSE
#define Net_IsConnected() FALSE
#define Net_IsOnline() FALSE
#define Net_WorldTilesAuthoritative() FALSE
#define Net_ConfigureQuickstart(name, gender) FALSE
#define Net_QuickstartEnabled() FALSE
#define Net_ResidentSlot() (-1)
#define Net_ResidentIdentity(slot, name, gender) (-1)
#define Net_PrefillQuickstartName() FALSE
#define Net_ApplyQuickstartIdentity() FALSE
#define Net_ApplyTownIdentity() ((void)0)
#define Net_RandomizeInitialAppearance() ((void)0)
#define Net_SubmitInitialTown() FALSE
#define Net_RequestPickup(position, item) FALSE
#define Net_RequestDrop(ut_x, ut_z, item) FALSE
#define Net_BeginPredictedDrop(ut_x, ut_z, item) FALSE
#define Net_BeginPredictedPickup(position) FALSE
#define Net_CancelPredictedTile(ut_x, ut_z) ((void)0)
#define Net_RequestDig(position) FALSE
#define Net_RequestFillHole(position) FALSE
#define Net_RequestBury(position, item, inventory_slot) FALSE
#define Net_RequestPlant(position, item, inventory_slot) FALSE
#define Net_RequestChop(position) FALSE
#define Net_RequestEncounter(kind, species) FALSE
#define Net_EquipmentAuthoritative() FALSE
#define Net_RequestHoldItem(inventory_slot, item) FALSE
#define Net_EncounterRecordsPending() FALSE
#define Net_BankingAuthoritative() FALSE
#define Net_RequestBankTransfer(amount) FALSE
#define Net_RequestPayDebt(amount) FALSE
#define Net_MailAuthoritative() FALSE
#define Net_ShopStockAuthoritative() FALSE
#define Net_ApplyAuthoritativeShopStock() ((void)0)
#define Net_EconomyAuthoritative() FALSE
#define Net_RequestSellItems(play, count) FALSE
#define Net_RequestBuyItem(item) FALSE
#define Net_RequestDonate(inventory_slot, item) FALSE
#define Net_MailboxMailId(index) 0
#define Net_CarriedMailId(index) 0
#define Net_RequestTakeMail(mail_id) FALSE
#define Net_RequestClaimMail(mail_id) FALSE
#define Net_RequestDiscardMail(mail_id) FALSE
#define Net_GyroidAuthoritative() FALSE
#define Net_RequestGyroidTake(house_idx, item_slot, item) FALSE
#define Net_RequestGyroidCollect(house_idx) FALSE
#define Net_TurnipMarketAuthoritative() FALSE
#define Net_TurnipSellPrice(item) 0
#define Net_ApplyAuthoritativeTurnipMarket() ((void)0)
#define Net_TownTuneAuthoritative() FALSE
#define Net_RequestTownTune(notes) FALSE
#define Net_ApplyAuthoritativeTownTune() ((void)0)
#define Net_NoticeBoardAuthoritative() FALSE
#define Net_RequestNoticePost(post, size) FALSE
#define Net_ApplyAuthoritativeNotices() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
