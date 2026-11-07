// coop/player/pocket_travel.h -- a player's trip to a pocket dimension, kept inside the session.
//
// The game sends one player to a pocket level (sl_poopdim, sl_goop, sl_place, sl_alexpdim,
// sl_woid) with lib_C::loadLevel, which ends in a full OpenLevel: the main world is unloaded on
// that machine and comes back later from the save slot object the game instance kept. The world
// lives in the host's process, so a CLIENT's trip is that client going "away": the session stays
// up, the host parks the slot (not world-ready, its puppet hidden where it stood, its world lanes
// stopped), and the way home re-runs the join's save transfer and world load for the admitted slot
// instead of the stale slot the client left with. The HOST's trip would unload the only copy of
// the shared world, so it is refused while hosting. The design and its evidence:
// docs/pocket-dimensions.md.
//
// This file is the shared half: the pocket ids, the level classification, the per-slot location
// table both roles read, and the entry points the session calls, which hand each edge to the
// role's own half (pocket_travel_host, pocket_travel_client). Gameplay layer: the engine is
// reached only through ue_wrap.

#pragma once

#include <cstdint>

#include "ue_wrap/core/script_gate.h"

namespace coop::net { class Session; }

namespace coop::player::pocket_travel {

// Where a player is. The values are wire values (PocketTravelPayload.pocket,
// PlayerLocationPayload.pocket) and are never reused.
enum class Pocket : uint8_t {
    Main     = 0,  // the main world, untitled_1
    PoopDim  = 1,  // sl_poopdim  (prop_biotoilet_C)
    Goop     = 2,  // sl_goop     (puddle_C)
    Place    = 3,  // sl_place    (wellTp_C, prop_shake_C)
    AlexPdim = 4,  // sl_alexpdim (skullzone_C)
    Woid     = 5,  // sl_woid     (insomniac_C)
    Count,
    Other    = 0xFE,  // a level that is neither the main world nor a pocket (the menu, the backrooms)
};

// The phases of a client's PocketTravel message. Wire values.
enum class Phase : uint8_t {
    Enter         = 1,  // at the loadLevel decision, before the travel
    Arrived       = 2,  // the pocket level is up on the client
    ReturnRequest = 3,  // the way home was taken: re-run the transfer for this slot
    Failed        = 4,  // the trip or the return could not complete; `reason` says why
};

// Why an edge happened, for the logs and PlayerLocation.cause. Wire values.
enum class Reason : uint8_t {
    None           = 0,
    Trip           = 1,  // the game's own trigger sent the player
    ReturnExit     = 2,  // the pocket's own way home
    ReturnDeath    = 3,  // a run-ending inside the pocket, answered by bringing the player home
    ReturnFallback = 4,  // the main world came back without the return being seen at loadLevel
    Disconnected   = 5,  // the slot left while away
    Refused        = 6,  // the trip was refused (the host's own trip, a mirror's trip)
    TransferFailed = 7,  // the return's save transfer failed
    LoadFailed     = 8,  // the return's world load failed
    Returned       = 9,  // back in the main world and world-ready again
};

// The level name to its pocket: Main for untitled_1, a pocket id for the five, Other otherwise.
// Case-insensitive, as FNames compare.
Pocket ClassifyLevel(const wchar_t* levelName);
inline bool IsPocket(Pocket p) {
    return p != Pocket::Main && p != Pocket::Other && p < Pocket::Count;
}
// The level's short name for a log line ("sl_poopdim", "untitled_1", "other").
const char* LevelTag(Pocket p);
// Is a wire byte a Pocket this build knows (Main or one of the five)?
bool IsValidWire(uint8_t v);
const char* PhaseTag(Phase p);
const char* ReasonTag(Reason r);

// The per-slot location table. On the host it is the authority; on a client it mirrors the host's
// PlayerLocation rows. Game thread.
Pocket LocationOf(uint8_t slot);
uint32_t TripSeqOf(uint8_t slot);
void SetLocation(uint8_t slot, Pocket p, uint32_t tripSeq);
void ResetLocations();
// Two slots whose players stand in the same level: the host relays one's pose and voice to the
// other, and each shows the other's puppet.
bool SameLocation(uint8_t a, uint8_t b);

// ---- the session's entry points (net_pump, subsystems, harness/pump) -----------------------
void Install(coop::net::Session* session);  // idempotent, every frame (harness/pump)
void Tick();                                 // session tick, game thread (subsystems)
void OnSessionStart();                       // net_pump's session start
void OnDisconnectForSlot(uint8_t slot);      // HOST: a slot left (subsystems::DisconnectSlot)

// run_end_travel's non-menu branch: lib_C::loadLevel(`level`) authored by `author`, at the body's
// entry, before the travel. Hands it to the role's half; Run when there is no live session.
ue_wrap::script_gate::Verdict OnLoadLevel(const wchar_t* level, void* author);

}  // namespace coop::player::pocket_travel
