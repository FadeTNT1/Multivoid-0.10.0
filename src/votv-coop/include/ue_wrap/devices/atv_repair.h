// ue_wrap/devices/atv_repair.h -- engine access for the ATV's repair look and its unscrew: what part of the ATV the
// local player looks at (a fitted wheel, an empty socket, the spare box, the storage), the game's own finished unscrew
// replayed for a given wheel or for the battery, and the end of an unscrew in progress.
//
// Principle-7 engine-wrapper layer: no network or coop state, no authority decisions. The coop half,
// coop/interactables/atv_repair, decides who runs what; names are sdk_profile_names.h's. Game thread, everything.

#pragma once

#include <cstdint>

namespace ue_wrap::atv_repair {

// The look lookAt last wrote on this copy. -1 = none for the two indices.
struct Look {
    int32_t tire = -1;          // lookAtTire: a fitted wheel, 0..3
    int32_t socket = -1;        // lookAtTireSocket: an empty socket, 0..3
    bool    atSpareBox = false; // lookAtSpareTireBox
    bool    atContainer = false;// lookAtContainer: the ATV's storage, where playerUsedOn stows the held item instead
};

// Resolves the five look fields and the three functions on ATV_C, once. False until ATV_C is loaded; a loaded class
// missing any of them answers false for the session, said once.
bool Resolve();

// False, `out` default, on null or unresolved.
bool ReadLook(void* atv, Look& out);

// The game's own finished unscrew on this copy for `tire` (0..3, the fitted wheel that comes off as a prop), or for the
// battery (tire -1: it comes out as a prop carrying its charge): lookAtTire is written to `tire`, lookAtTireSocket to
// -1 and playersWheel cleared, so unscrewPanel takes exactly that branch, then getUnscrewSpawn() names the spawn
// component and unscrewPanel(spawnAt) runs. The game's own body then spawns the prop, writes the presence, runs its
// reducer and resetUnscrew, and plays its sound. False, nothing run, on null, unresolved, a tire outside -1..3 or a
// null spawn component.
bool ReplayUnscrew(void* atv, int32_t tire);

// resetUnscrew() on this copy: the progress widget, the timer and the unscrewing player cleared. False on null,
// unresolved or a call that did not dispatch.
bool ResetUnscrew(void* atv);

}  // namespace ue_wrap::atv_repair
