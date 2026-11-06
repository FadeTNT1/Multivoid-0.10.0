// ue_wrap/devices/garage.h -- engine access for the base GARAGE door (Agarage_C), the principle-7
// wrapper layer: no network or coop state. coop::interactable_sync drives the sync through here.
//
// Agarage_C is a level-placed AtriggerBase_C descendant. Unlike Adoor_C it has no sensor, no
// autoclose and no tick, so it never reverts on its own and syncs SYMMETRICALLY. The wall button
// only fires runTrigger and the garage toggles itself, so we read and write the garage's own open
// flag, which catches every activation source, and never the button.
//
// Identity is the garage's LEVEL-EXPORT FName, its placed-actor name, and not the inherited
// AtriggerBase_C key. That key comes from the gamemode's one-shot, sublevel-gated keying pass
// (loadObjects -> loadTriggers, gated on isSublevelAllowed), and a garage that misses the pass
// keeps the class default, "garageDoor", which every garage instance shares; and the host was
// seen losing its garage identity across a reload while every door kept its key. The
// level-export FName is baked into the cooked map package, so both peers read the identical
// name -- the same reason door_box keys the lockers by name.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::garage {

// Resolve Agarage_C + the Open offset + the acivae UFunction. Idempotent; true
// once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is garage_C or a subclass. False if not yet resolved.
bool IsGarage(void* obj);

// The garage's LEVEL-EXPORT FName (its placed-actor name) as a wide string ("" on failure).
// This is the cross-peer identity -- see the header note on why NOT the save Key.
std::wstring GetNameKey(void* g);

// Read the garage's `Open` bool into `open`. False if the read could not be made (null /
// not resolved); leaves `open` untouched on failure.
bool TryReadOpen(void* g, bool& open);

// Drive the garage to `open` -- idempotent (a no-op if it is already there). Writes the `Open` flag
// directly, because neither settime nor acivae writes it -- only runTrigger and loadTriggerData do
// -- and the mirror's poll baseline would otherwise go stale and oscillate. Then plays the native
// animated swing through acivae(): the montage from position 0 at half rate, and the move timeline
// over its full length, both taking their direction from the flag just written. NOT settime, which
// runs that same timeline and then jumps it -- the montage at full rate from position 100, then
// SetNewTime to second 0 or 1 of a six-second track, so a close snaps shut and an open lurches
// and runs on. Must run on the game thread. False on null or an unresolved UFunction.
bool ApplyOpen(void* g, bool open);

// What NormalizeOpen found and did.
enum class Settled : uint8_t {
    Unavailable,  // the timeline (or one of its verbs) did not resolve: nothing was moved
    Already,      // at rest, flag and timeline agree
    Busy,         // mid-swing (`mov` or the timeline playing): left to finish
    Snapped,      // at rest but off its end: the timeline put at the end `open` names, its update fired
};

// Reconcile a garage that is AT REST with `open`, for a load or a join snapshot: the `Open` flag alone
// agreeing is not enough, because the native settime jumps the timeline and a load can leave Open=true
// with the move timeline (and so the mesh) at its closed end, after which the next press animates the
// wrong way. Unless the door is mid-swing, writes Open := open, stops the move timeline and puts it at
// its far end (the full length when open, 0 when closed -- acivae plays forward for open, as door_C's
// dir 0 does) with its update fired, so the mesh lands, and clears `mov`. Never touches a door that is
// swinging, so a live toggle's echo cannot cut an animation short. `before` / `after` (optional)
// receive the timeline position read before and after. Game thread.
Settled NormalizeOpen(void* g, bool open, float* before = nullptr, float* after = nullptr);

// What SettleAtRest read back after driving the door, to prove the visible state matches.
struct SettleReadback {
    float timelineBefore = 0.f;   // the move timeline's position before the settle
    float timelineAfter  = 0.f;   // and after
    bool  meshReached    = false; // the door mesh had an animation instance, so the montage could be driven
    bool  montageRead    = false; // a swing montage was active afterwards and its position read
    float montagePos     = 0.f;   // that montage's position, at the end of its open or close section
};

// NormalizeOpen for a world's load: a door at rest and off the end `open` names is driven all the way as
// SeedTo drives a joiner's -- the game's settime (the mesh montage), then the montage put at its section's end,
// the timeline stopped at its end with its update fired, `mov` cleared, the swing sound out, the nav area set --
// where NormalizeOpen moves the timeline (the collision box) alone and leaves the visible door as the load
// left it. Falls back to NormalizeOpen when settime or the timeline reads do not resolve. Never touches a
// swinging door. `rb` (optional) receives the read-back. Game thread.
Settled SettleAtRest(void* g, bool open, SettleReadback* rb = nullptr);

// The garage's swing as a peer would send it: whether it is mid-swing (`mov` or the timeline playing) and how far
// along the timeline it stands, 0 (closed end) to 1 (open end). False when the timeline did not resolve or read;
// the outputs are then untouched. Game thread.
bool TryReadTravel(void* g, bool& moving, float& fraction);

// What a SeedTo call found.
enum class Seeded : uint8_t {
    Working,  // not there yet: the door is not ready (its mesh has no animation instance yet), or is mid-swing toward
              // the seed; call again
    Rest,     // the door stands at the seed's end, its flag, timeline, collision and mesh all driven there
    Failed,   // the verbs this needs did not resolve: nothing was driven, a live apply is the fallback
};

// Put the garage where its authority stood, for a joiner's snapshot: `open` is the authority's Open, `moving` and
// `fraction` (the timeline's place, 0..1) say whether it was mid-swing and how far.
//   At rest: the game's own load verb, settime, runs once -- it plays the door-mesh montage (the one thing that moves
//   the mesh; the timeline moves only the collision box) and sets the timeline -- then the montage is put at the end of
//   its open or close section, the timeline stopped at its end with its update fired, `mov` cleared, the swing sound
//   put out and the nav area set as the finished swing sets it.
//   Mid-swing: the animated swing (acivae) starts toward `open`, and the timeline and the mesh montage are put at the
//   authority's place in it, so the door finishes with the authority's.
// `cookie` is the caller's per-seed memory, zero at the start: it records that the door has been driven, so a repeat
// call neither restarts the swing nor replays the montage, only watches the door settle and drives it again if
// something rewrote the flag (a save's late load). Never replays the wall button. Game thread.
Seeded SeedTo(void* g, bool open, bool moving, float fraction, uint32_t& cookie);

// Whether the garage is mid-swing, its `mov`: its runTrigger ignores a call then. False if the read
// could not be made (null, or the field did not resolve). Game thread.
bool TryReadMoving(void* g, bool& moving);

// The garage's own runTrigger(owner, index), the wall button's call: Open negated and the swing,
// unless it is mid-swing. For a dev drill. False on null, an unresolved UFunction or a failed
// dispatch. Game thread.
bool CallRunTrigger(void* g, void* owner, int32_t index);

}  // namespace ue_wrap::garage
