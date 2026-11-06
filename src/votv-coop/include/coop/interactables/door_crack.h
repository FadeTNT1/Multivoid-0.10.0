// coop/interactables/door_crack.h -- a door's leaf roots on the door lane: the crack a hit leaves, and a swing caught
// mid-way, carried through the door adapter's optional hooks (interactable_channel.h) on DoorState, not a lane of their own.
//
// door_C opens and closes by moving two scene components under its leaves (door_r_root, door_l_root). A swing lerps
// them; a hit (addDamage) on a door at rest nudges each a little toward its open place and, past a threshold, begins
// the swing. So a hit that does not open the door still changes what the door looks like, with no change in the open
// bool the lane carried alone, and a client's copy stood shut until the swing ran. Here that place rides the state:
//   - HOST, edge: DetailOf is the dedup token, so a crack that changed is an edge though the bool did not; FillPayload
//     puts the place in the payload, flagged a seed (kSeedFlagSeed | kSeedFlagCrack). The edge is sent from a watch
//     after the host's addDamage (door_state_verbs), whoever hit: a host-local swing or a client's intent the host ran.
//   - HOST, snapshot: the same, and FillSeed adds a door caught mid-swing (kSeedFlagMoving: the move timeline's place
//     and the swing's start place).
//   - CLIENT: a seed is kept per key and driven by ApplySeed until the door stands there (interactable_seed.h), which
//     also holds it for a door that has not streamed in yet. A resting crack is the roots put where the host's are
//     (K2_SetRelativeLocation); a swing in progress is started the usual way (SmartApply) and then put at the host's
//     place in it (SetPlaybackPosition with its anchors). The hit is never replayed: the client's own addDamage is
//     refused (door_verb_intent) and a place is what arrives.
// Gameplay/network layer; the engine only through ue_wrap::door.

#pragma once

#include "coop/interactables/interactable_seed.h"
#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::interactable_sync::door_crack {

// A token for the dedup of an edge: 0 for a door that is not a shut one at rest with its roots off their rest place,
// else a value that differs for every distinct place (the two roots' per-mille). Game thread.
uint32_t DetailOf(void* door);

// Fill a payload about to be sent (a live edge or the snapshot) with the shut door's crack, if it has one. Game thread.
void FillPayload(void* door, const std::wstring& key, bool on, coop::net::KeyedTogglePayload& p);

// Snapshot only: a door mid-swing sends where it stands in it. Runs after FillPayload. Game thread.
void FillSeed(void* door, bool on, coop::net::KeyedTogglePayload& p);

// Whether a received state is a seed this lane keeps and drives.
bool IsSeed(const coop::net::KeyedTogglePayload& p);

// One drive of a held seed on the client's copy. The caller marks the apply as the lane's own (ApplyingDoor), so the
// client's refusal of its own door verbs lets the swing it starts through. Game thread.
SeedStep ApplySeed(void* door, bool on, const coop::net::KeyedTogglePayload& p, uint32_t& cookie);

}  // namespace coop::interactable_sync::door_crack
