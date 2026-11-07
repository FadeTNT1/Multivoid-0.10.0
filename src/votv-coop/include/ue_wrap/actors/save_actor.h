// ue_wrap/actors/save_actor.h -- engine access for the persistent actors a held tool leaves behind: the actor_save_C
// descendants of profile::name::kPlayerDeployedSaveActorClasses (a sticker, a candle, a string of lights, a weld, a
// nail, a rug, a snowman, a built wall, a jumper cable). Principle-7 engine-wrapper layer: the allowlist's classes, the
// actor_save_C key and save flag, and the actor's own getData / loadData / processKeys. No network or coop state.
//
// Three facts shape it. The key is minted LAZILY: actor_save_C::getKey hands a None key to lib_C::assignKey, which mints
// a random one and files the actor in the gamemode's key table (the table getObjectFromKey reads, so every processKeys
// in the world resolves it), so EnsureKey is the game's own verb and not a field write. The record is the class's own:
// getData and loadData are dispatched on the most-derived declaration, so a class that adds save state is carried
// without a line here changing. And actor_save_C::loadData restores the key alone, never the transform: the game's
// load spawns the actor at the record's transform first, and so does Spawn.
//
// Game thread only: every path reads or calls engine object state.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::save_actor {

// The allowlist, in profile order: the index IS what a caller keys its per-class policy on. Count() is its length.
size_t Count();
const wchar_t* ClassNameAt(size_t idx);

// Re-resolve the allowlist's classes through the object index (a class a new world loaded, one it unloaded). Cheap,
// but the caller paces it (about once a second); the lookups below answer from what the last call resolved. Game thread.
void Refresh();

// The allowlist index of `obj`'s class, -1 when it is none of them (an exact class compare, so a subclass of an
// allowlisted class that is not itself listed is not one). The class pointer is checked against the listing the
// object index gave it when Refresh resolved it, so a class unloaded since never matches a stranger at its address.
// Allocation-free; game thread (the listing check asserts it).
int IndexOf(void* obj);

// The allowlist index of a class leaf name (case-insensitive, as the engine compares names), -1 when it is not listed.
int IndexOfName(const std::wstring& className);

// The loaded UClass of an allowlist index, null while it is not loaded. Game thread.
void* ClassAt(size_t idx);

// Whether `obj` is the gamemode (mainGamemode_C or below): the caller of every save load's spawn. Game thread.
bool IsGamemode(void* obj);

// actor_save_C::key as a string, "" when it is None or `actor` is not an actor_save_C. A field read.
std::wstring KeyOf(void* actor);

// Write actor_save_C::key. False for a non-actor_save_C or a name that cannot be made. A field write: the gamemode's
// key table is not told (EnsureKey does that).
bool WriteKey(void* actor, const std::wstring& key);

// The game's own getKey on `actor`: a None key is minted, and the key (old or new) is filed in the gamemode's key
// table under this actor. The key, or "" when the call did not run or still answered None. Game thread.
std::wstring EnsureKey(void* actor);

// actor_save_C::skipSave, which ignoreSave returns to the game's save walk. Write it on every mirror in the deferred
// window, before the finish, so no autosave sees the actor unmarked (docs/deployables.md, the first mirror rule).
bool WriteSkipSave(void* actor, bool on);

// The actor's own legacy record through its most-derived getData (struct_save1: class, transform, key and the typed
// groups the class files). False when it does not resolve or the call fails.
bool CaptureRecord(void* actor, save_record::SaveRecord& out);

// The actor's 0.10.0 save record through its most-derived betterGetData (struct_betterSave: the actor's SaveGame
// properties as ThingsAndStuffLib::actorToBytes writes them, its attach key, key and lifespan), encoded by
// ue_wrap/actors/inventory_item_codec. This is the record the game's own save keeps for an actor_save_C, so it carries
// what a class without a getData of its own holds (a jumper cable's two ends). False when the codec is not measured
// yet, the function does not resolve or the record does not encode.
bool CaptureBetter(void* actor, std::vector<uint8_t>& blob);

// The legacy record handed to the actor's most-derived loadData AS GIVEN (the caller has decided what it takes from
// the sender). `resolve` governs how the record's class leaves resolve (null = FindClass). Game thread.
bool ApplyRecord(void* actor, const save_record::SaveRecord& r, save_record::ClassResolver resolve = nullptr);

// The encoded 0.10.0 record decoded and handed to the actor's most-derived betterLoadData, which also writes the key,
// the attach key and the lifespan it carries: a caller with a canonical key writes it again after. False when the
// codec is not measured, the encoding is malformed or does not end where the blob does, or the call fails.
bool ApplyBetter(void* actor, const std::vector<uint8_t>& blob);

// The actor's own processKeys: its attach keys resolved in this world and the attachments made (a sticker to what it
// was stuck on, a weld to its two props, a nail's constraint). False when it does not resolve or the call fails.
bool ProcessKeys(void* actor);

// Spawn `cls` at the record's transform through the deferred window: `key` and the save flag written inside it, the
// scale set after the finish when it is not unit. The record itself is NOT applied (ApplyRecord). Null on any failure,
// nothing left half-built. Game thread.
void* Spawn(void* cls, const save_record::SaveRecord& r, const std::wstring& key, bool skipSave);

// Drop every cached class, member and function (level change / disconnect).
void ResetCache();

}  // namespace ue_wrap::save_actor
