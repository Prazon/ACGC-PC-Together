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
int Net_HouseOccupied(int house_index);
int Net_IsConnected(void);
int Net_IsOnline(void);
int Net_ConfigureQuickstart(const char* name, int gender);
int Net_QuickstartEnabled(void);
int Net_ResidentSlot(void);
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
int Net_RequestEncounter(int kind);
#else
#define Net_PreSimulation(play) ((void)0)
#define Net_PostSimulation(play) ((void)0)
#define Net_OnActorCreated(actor) ((void)0)
#define Net_OnActorDestroyed(actor) ((void)0)
#define Net_OnSceneLoaded(play) ((void)0)
#define Net_BeginSceneTransition(play, next_scene) ((void)0)
#define Net_ApplyHouseStateBeforeRoom(play) FALSE
#define Net_HouseOccupied(house_index) FALSE
#define Net_IsConnected() FALSE
#define Net_IsOnline() FALSE
#define Net_ConfigureQuickstart(name, gender) FALSE
#define Net_QuickstartEnabled() FALSE
#define Net_ResidentSlot() (-1)
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
#define Net_RequestEncounter(kind) FALSE
#endif

#ifdef __cplusplus
}
#endif

#endif
