// ue_wrap/actors/dirt.h -- engine access for the dirt a player digs: the hole (dirthole_C, a prop_C that mints no
// key), the mound a buried item sits in (dirthole_item_C, an actor_save_C) and the plant a seed becomes
// (growingPlant_C). Principle-7 engine-wrapper layer: no network or coop state. What each Blueprint does is in
// ue_wrap/core/sdk_profile_names.h, with the names.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::dirt {

// The class, or one below it. A class is looked up by name each time (a world reload makes a new class object).
bool IsHoleClass(void* cls);
bool IsMoundClass(void* cls);
bool IsPlantClass(void* cls);
bool IsHole(void* obj);
bool IsMound(void* obj);
bool IsPlant(void* obj);

// Every live hole / mound / plant of the running world, never a class default. The class list is rebuilt only when the
// index's class set moves. Game thread.
std::vector<void*> AllHoles();
std::vector<void*> AllMounds();
std::vector<void*> AllPlants();

// A hole's place and its `matType`. False on a failed read.
struct HoleState {
    FVector  location{};
    FRotator rotation{};
    int32_t  mat = 0;
};
bool ReadHole(void* hole, HoleState& out);
// Spawn a hole of `cls` (a dirthole_C or below), `matType` written in the deferred window. Null on any failure.
void* SpawnHole(void* cls, const HoleState& at);

// A mound that holds a player's burial: its betterData names a class. False for any other mound (the underground
// loot mounds keep `object` or `name` instead), an unreadable one, or one the record codec cannot carry. `blob` is
// the record, encoded.
bool ReadBurial(void* mound, FVector& loc, FRotator& rot, std::vector<uint8_t>& blob);
// Spawn a mound of `cls` holding the encoded record. Null on any failure.
void* SpawnMound(void* cls, const FVector& loc, const FRotator& rot, const std::vector<uint8_t>& blob);

// A plant's row name (`plant`), "" when unread. Spawn one of `cls`: `plant` written in the deferred window, then the
// plant's own upd(false). Null on any failure.
bool ReadPlantName(void* plant, std::wstring& name);
void* SpawnPlant(void* cls, const FVector& loc, const FRotator& rot, const std::wstring& name);

// An item's whole save record, through its own betterGetData, encoded. False when the item has none or the codec
// cannot carry it.
bool ReadItemRecord(void* item, std::vector<uint8_t>& blob);
// A fresh actor of `cls` (a prop_C or below) at `loc`, its record loaded through its own betterLoadData when `blob`
// is not empty: what a client's held item is, rebuilt on the host. The caller marks it a mirror spawn first. Null on
// any failure.
void* SpawnItem(void* cls, const FVector& loc, const std::vector<uint8_t>& blob);

// The hole's own filteritems(item): whether the item cannot be buried. False (not blocked) when the call fails.
bool FilterBlocks(void* hole, void* item);
// The hole's own processBuriedItem(item). `ok` is its answer; false when the call itself failed.
bool ProcessBuried(void* hole, void* item, bool& ok);

// Is `obj` a prop_dirtball_C (the dirt a shovel throws; using it on a hole fills the hole)?
bool IsDirtBall(void* obj);

}  // namespace ue_wrap::dirt
