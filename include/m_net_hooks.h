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
u64 Net_CarriedMailId(int index);
/* Mailbox to pocket, present out of a carried letter, and throwing an emptied
 * letter away -- the same three steps the original UI performs locally. */
int Net_RequestTakeMail(u64 mail_id);
int Net_RequestClaimMail(u64 mail_id);
int Net_RequestDiscardMail(u64 mail_id);
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
#define Net_RequestDig(position) FALSE
#define Net_RequestFillHole(position) FALSE
#define Net_RequestBury(position, item, inventory_slot) FALSE
#define Net_RequestPlant(position, item, inventory_slot) FALSE
#define Net_RequestChop(position) FALSE
#define Net_RequestEncounter(kind, species) FALSE
#define Net_EncounterRecordsPending() FALSE
#define Net_BankingAuthoritative() FALSE
#define Net_RequestBankTransfer(amount) FALSE
#define Net_RequestPayDebt(amount) FALSE
#define Net_MailAuthoritative() FALSE
#define Net_MailboxMailId(index) 0
#define Net_CarriedMailId(index) 0
#define Net_RequestTakeMail(mail_id) FALSE
#define Net_RequestClaimMail(mail_id) FALSE
#define Net_RequestDiscardMail(mail_id) FALSE
#endif

#ifdef __cplusplus
}
#endif

#endif
