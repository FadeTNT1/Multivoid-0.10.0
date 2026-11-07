// coop/props/prop_spawn_authoring.h -- did a PLAYER author this birth, or did the game?
//
// A client's world mints keyed props constantly with nobody asking: spawners, a loot mound, a lightning
// strike, an impact. None may cross -- every peer's game makes its own -- so the drop-intent lane cannot
// admit by CLASS; the discriminator is WHO ASKED, read off the VM at the finish-spawning seam: the frames
// above the call and the dispatches that started them (ue_wrap/core/script_stack.h). A birth is the
// player's when that chain holds the player's hand item, one of the player's own action events or verbs,
// a widget the player clicked, a granted act's replay, or an actor a player's act made a moment ago.
// docs/props.md, "Who authors a prop", has the rules and their limits.
//
// Gameplay layer (principle 7): reaches the engine only through ue_wrap. Game thread only.

#pragma once

#include <cstdint>

namespace coop::prop_spawn_authoring {

// Which rule named a birth the player's. The first four are the verbs read before the walk; Replace is
// decided at the drain, from a shared destroy the same dispatch sent.
enum class Rule : uint8_t {
    None, Menu, ShrimpTray, Toolgun, Shovel, Replay, HandItem, PawnAction, Widget, Inherited, Replace, Count
};
const char* RuleName(Rule rule);   // "menu", "hand-item", ...; "none" for None

// Install the script-body gate watch on the gamemode's spawn verb. A no-op on the host, which never asks
// the question. Idempotent and retry-throttled while mainGamemode_C is unresolved. Game thread.
void Install(bool isClient);

// Which rule authors the birth the caller is looking at RIGHT NOW. Call it from inside a
// FinishSpawningActor post callback and nowhere else: it reads that callback's calling frame
// (ue_wrap::ufunction_hook::CurrentCallerFrame) and this thread's open dispatches, which mean nothing
// outside one. A pure query, any class. A reflected call of our own is never a player's, except a
// granted act's replay. None for every ambient spawn the world makes for itself. Game thread.
Rule BirthAuthorship();
bool BirthIsPlayerAuthored();   // BirthAuthorship() != Rule::None

// The drop-intent seam's record of an authored birth of any class (a projectile, a deployable, a prop):
// counted, said once per rule and class, and remembered, so a birth whose frame or dispatch runs on this
// actor within 30 s is authored too (the nail a fired nail projectile leaves, a deployable's follow-up).
// The memory holds 128 actors by index and serial.
void NoteAuthoredBirth(void* actor, Rule rule);

// Whether `actor` was born authored and is still this peer's alone: remembered, the same object, and not
// bound to a wire identity since (a mirror, an element row), at which point its own later births are the
// shared world's. Any age. Game thread.
bool IsAuthoredActor(void* actor);

// The outermost dispatch now open on this thread (ue_wrap::script_stack::OutermostDispatchSerial); 0
// outside every dispatch. A birth carries it to the drain.
uint64_t DispatchToken();

// Both roles, at the finish-spawning seam: the dispatch `actor` was born in (the last 32 births).
void NoteBirthDispatch(void* actor);
// The destroy seam: is `actor` dying inside the very dispatch it was born in? A Blueprint that spawns an
// actor to read it and destroys it at once (an inventory slot's respawnReset, a double click) makes such
// births; no peer was ever told of one, so its death names nothing on the wire.
bool DiesInItsBirthDispatch(void* actor);
void NoteTransientDeath();   // counted for the teardown verdict

// The CLIENT's destroy seam, after it sent a keyed destroy of a shared prop: this dispatch has destroyed
// a prop on every peer. A birth in the same dispatch is the destroyed prop's replacement
// (lib_C::replaceProp, a morph, a slice, a glowstick lit) and crosses with it.
void NoteSharedDestroy();
bool SharedDestroyCrossedIn(uint64_t token);   // the last 16 such dispatches

// The drain: an authored birth crossed as an intent, by `rule`. Counted; a Replace is said once per
// class. Game thread.
void NoteAdmitted(void* actor, Rule rule);

// Session teardown: report the tallies, the menu verb's two columns and every rule's, and zero them;
// forget the remembered actors and the cached verdicts. The watch and the resolved UFunctions stay. Game
// thread.
void Reset();

}  // namespace coop::prop_spawn_authoring
