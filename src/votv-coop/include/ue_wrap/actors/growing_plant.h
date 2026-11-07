// ue_wrap/actors/growing_plant.h -- engine access for the growing plant (growingPlant_C, an actor_save_C) and its
// descendants, the farm pot that holds one (prop_farmPot_C) and the two mainPlayer_C members the harvest and the pot
// read. Principle-7 engine-wrapper layer: no network or coop state. What each Blueprint does is in
// ue_wrap/core/sdk_profile_names.h, with the names. Every member is resolved by name on growingPlant_C once per class
// object, so a descendant reads the same offsets. Game thread, all of it.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::growing_plant {

// The class, or one below it; the Stolas herb, whose own grown() strikes lightning and destroys itself.
bool IsPlantClass(void* cls);
bool IsPlant(void* obj);
bool IsStolasHerb(void* obj);

// Every live plant of the running world, never a class default: the instances of each class below growingPlant_C.
// A session-edge read (a seed, a joiner's snapshot), never a per-tick one.
std::vector<void*> AllPlants();

// The mirrored state of one plant. `plant` is the row name ("" when None); `disease` the persistent disease row
// (activeDisease_name, "" when None). `plantType` and the amount range are the row's (`data`), read only.
struct State {
    std::wstring plant;
    float        grow = 0.f, water = 0.f, dryness = 0.f, fertilizer = 0.f, fruitgrow = 0.f, health = 0.f;
    int32_t      amount = 0;
    bool         grew = false;
    bool         hideDirt = false;
    std::wstring disease;
    uint8_t      plantType = 0;
    int32_t      amountMin = 0, amountMax = 0;
};
bool ReadState(void* plant, State& out);

// Write the mirrored fields: the row name (when it differs and is not empty), the six floats, `amount` and `hidedirt`.
// `grew`, the disease and the row are left to the plant's own verbs. False on a member that did not resolve.
bool WriteState(void* plant, const State& in);

// The plant's own fields one at a time, for the verbs that move one.
bool WriteGrew(void* plant, bool grew);
bool ReadDiscard(void* plant, bool& discard);
bool WriteDiscard(void* plant, bool discard);

// The plant's own functions: upd(skipDataTable), updLoc(), processGrown(skipGrowCheck), addDisease(name) (its
// `success`), the disease component's heal() (false when the plant has none), addMedicine(type, time), harvestSound().
bool CallUpd(void* plant, bool skipDataTable);
bool CallUpdLoc(void* plant);
bool CallProcessGrown(void* plant, bool skipGrowCheck);
bool AddDisease(void* plant, const std::wstring& name, bool& success);
bool HealDisease(void* plant);
bool AddMedicine(void* plant, uint8_t type, float time);
bool PlayHarvestSound(void* plant);

// The harvest verb: actionOptionIndex(player, a hit on the plant, action 4, no component). False when the call did not
// run; what it did is the caller's to read.
bool CallHarvestVerb(void* plant, void* player);

// The plant's `attachTo` (the pot or the ground it grows in), or null.
void* AttachTo(void* plant);

// The plant's actor_save key, "" when it has none; and its write.
std::wstring KeyOf(void* plant);
bool SetKey(void* plant, const std::wstring& key);

// Spawn a plant of `cls` as the game's own spawners do: `plant`, the key, `attachTo` and `hidedirt` written in the
// deferred window, so BeginPlay attaches it and the construction script hides the dirt. Null on any failure.
void* SpawnPlant(void* cls, const FVector& loc, const FRotator& rot, const std::wstring& plantName, const std::wstring& key,
                 void* attachTo, bool hideDirt);

// A list_farmplants row by name (compared without case): its plant type, whether it grows in a pot and the smallest
// pot it fits (the pot's x scale). False when the table is not loaded yet or holds no such row: an item that is not a
// seed.
struct FarmRow {
    uint8_t plantType = 0;
    bool    canGrowInPots = false;
    float   minSize = 0.f;
};
bool ReadFarmRow(const std::wstring& name, FarmRow& out);

// Would the pot's own playerUsedOn plant a seed whose row is `seedName`: an empty pot, a row that grows in pots, not a
// tree, and a pot at least the row's size. The body's refusals only show a hint; this reads the same conditions first.
bool PotWouldPlant(void* pot, const std::wstring& seedName);

// A farm pot (prop_farmPot_C or below), and the plant it holds (`plant`), or null.
bool IsFarmPot(void* obj);
void* PotPlant(void* pot);

// The pot's playerUsedOn with `player` holding `seed`: the body plants the item mainPlayer_C.holdingActor_main names,
// so that member names `seed` for the one call and is put back after it. False when the call did not run.
bool CallPotPlant(void* pot, void* player, void* seed, const std::wstring& seedName);

// A fresh prop of `cls` (a prop_C or below) whose list_props row is `name`, frozen at `loc`: the item a pot plants
// on the host for a client's hand. Null on any failure.
void* SpawnSeed(void* cls, const FVector& loc, const std::wstring& name);

// mainPlayer_C `input_alt`: the held confirm key the discard asks for. False when unread.
bool ReadAltHeld(void* player, bool& held);

// mainPlayer_C "retarget add object to inventory"(item): what the harvest calls to pocket a berry. False when the call
// did not run; `success` is the body's answer.
bool CallRetargetAddToInventory(void* player, void* item, bool& success);

}  // namespace ue_wrap::growing_plant
