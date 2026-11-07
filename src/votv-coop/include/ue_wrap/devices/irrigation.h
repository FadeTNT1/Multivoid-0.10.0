// ue_wrap/devices/irrigation.h -- engine access for the irrigation set: the placed faucet (faucet_C), the ground hose
// (groundhose_C), both actor_save_C and not props, and the sprinkler (prop_sprinkler_C), a prop. What each class does is
// in sdk_profile_names.h beside their names; what is synced, and how, is docs/deployables.md, "Irrigation".
//
// Members and verbs resolve by name off the live classes, kept per class object, so a newer build that renames one
// leaves that read refusing rather than reading whatever sits at an old offset. Game thread throughout.

#pragma once

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::irrigation {

// The two actor_save_C kinds the irrigation lane names by key. The sprinkler is a prop the prop lanes carry.
enum class Kind : uint8_t { None = 0, Faucet = 1, Hose = 2 };

const char* KindName(Kind k);

// Which of the two `actor` is (a subclass included), None for anything else.
Kind KindOf(void* actor);
bool IsSprinkler(void* actor);
// The class `actor` was born of is the held faucet or the held hose: the two verbs that place the set.
bool IsFaucetItem(void* actor);
bool IsHoseItem(void* actor);

// The actor_save key, "" for None; its raw write. RegisterKey runs the actor's own getKey, which mints a key when it has
// none and files the actor under it in the game's key map, so getObjectFromKey and processKeys find it; "" on failure.
std::wstring KeyOf(void* a);
bool SetKey(void* a, const std::wstring& key);
std::wstring RegisterKey(void* a);

// The live links between the three, as the game holds them (null when unset). A setter writes the field alone.
void* FaucetHose(void* faucet);
void* HoseFaucet(void* hose);
void* HoseSprinkler(void* hose);
void* SprinklerHose(void* sprinkler);
bool SetFaucetHose(void* faucet, void* hose);
bool SetHoseFaucet(void* hose, void* faucet);
bool SetHoseSprinkler(void* hose, void* sprinkler);
bool SetSprinklerHose(void* sprinkler, void* hose);

bool ReadFaucetActive(void* faucet, bool& out);
bool ReadHoseDoCollect(void* hose, bool& out);
// A hose is settled when it no longer ticks: its BeginPlay (a placement) and resume() turn the tick on and finish()
// turns it off, so a ticking hose is one a player is still laying.
bool HoseSettled(void* hose);
// The hose's spline points, world space.
bool ReadHosePoints(void* hose, std::vector<FVector>& out);

// The actor's own record, through its most-derived getData; and the record handed back through its loadData, as given.
bool CaptureRecord(void* a, save_record::SaveRecord& out);
bool ApplyRecord(void* a, const save_record::SaveRecord& r, save_record::ClassResolver resolve);

// The verbs. Finish is groundhose_C.finish (tick off, gen, collision on, faucet.upd); Upd is the faucet's or the
// sprinkler's upd; UpdEffect the hose's updEffect (its water and its sprinkler's upd); Detach its detachSprinkler.
bool CallFinish(void* hose);
bool CallUpd(void* faucetOrSprinkler);
bool CallUpdEffect(void* hose);
bool CallDetachSprinkler(void* hose);

// Spawn `k` at the record's transform, the key written in the deferred window. A hose spawned here has `conntofauc`
// unset, so it never ticks and never follows this machine's player. Null on any failure.
void* Spawn(Kind k, const save_record::SaveRecord& at, const std::wstring& key);

// Every live instance of `k` in the running world.
std::vector<void*> AllOf(Kind k);

}  // namespace ue_wrap::irrigation
