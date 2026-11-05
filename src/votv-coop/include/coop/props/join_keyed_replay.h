// coop/props/join_keyed_replay.h -- the host's word for a keyed prop, kept through the join bracket.
//
// A keyed prop whose actor GC-churns mid-join leaves its mirror row holding a dead pointer while the game
// re-creates a same-key twin at its default (or save) transform. The membership sweep rebinds the row onto that
// twin (join_membership_sweep, "keyed churn RE-BIND"), but a rebind only moves the actor pointer
// (element::CreateOrAdoptPropMirror): the converge the PropSpawn row did to the dead actor never reached the
// replacement, so it kept a stale pose and flags.
//
// So while a bracket is open the receiver keeps the newest authoritative pose and physics flags per host eid
// (the PropSpawn row, then any PropSnapPos correction after it), and the sweep replays them onto the replacement
// through remote_prop_spawn::ConvergeKeyedToHost, the same convergence the snapshot apply runs.
//
// Scoped to the bracket: BeginClaimTracking, the end of the sweep and session teardown clear it. Game thread only.

#pragma once

#include <cstdint>
#include <string>

#include "coop/props/remote_prop_spawn.h"  // HostPose
#include "ue_wrap/core/types.h"

namespace coop::join_keyed_replay {

// A keyed PropSpawn row the host expressed this bracket (the newest per eid wins).
void NoteSpawn(uint32_t eid, const coop::remote_prop_spawn::HostPose& pose, const std::wstring& keyW,
               const std::wstring& classW);

// A PropSnapPos correction for `eid`: newer than its spawn row, so it replaces the retained pose and flags. A
// no-op for an eid with no retained row (a pile, or nothing keyed expressed this bracket).
void NoteSnapPos(uint32_t eid, const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t physFlags);

// Replays the retained word for `eid` onto `actor`, a same-key re-create the sweep just rebound to the row. A held,
// driven or thrown actor is left to the stream that owns it. True when a retained row existed and was applied.
bool ReplayOnto(uint32_t eid, void* actor);

// Drops everything retained.
void Clear();

}  // namespace coop::join_keyed_replay
