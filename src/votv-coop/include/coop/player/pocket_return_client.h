// coop/player/pocket_return_client.h -- a client's way home from a pocket dimension, as a join made
// in place (coop/player/pocket_travel.h; docs/pocket-dimensions.md, "Return").
//
// The client is away in a pocket level, a full world of its own; the main world is gone on this
// machine. Coming home re-runs the join's second half for the admitted slot without dropping the
// session: PocketTravel{ReturnRequest} to the host (which re-arms the slot's save transfer), the
// save transfer into the same zcoop slot, the story-save load of it from the world the player stands
// in (ue_wrap::engine::LoadStorySaveForReturn), and a FIRST world-ready announce in the new world, so
// the host runs the full connect replay. Admission, the roster, the slot and the identity do not
// change. The client's own curtain covers it. The fallback (the main world came back natively from
// the stale slot) runs the same way and replaces that world whole.
//
// Gameplay layer: the engine is reached only through ue_wrap. Game thread, except Cover(), which any
// thread may read (the loading screen's render).

#pragma once

#include <cstdint>

#include "coop/player/pocket_travel.h"

namespace coop::net { class Session; }

namespace coop::player::pocket_return_client {

// Cache the session pointer. Idempotent, every frame (harness/pump).
void Install(coop::net::Session* session);

// Start the way home from `from` (`why`: ReturnExit, ReturnDeath or ReturnFallback) for trip
// `tripSeq`; `departure` is where the player stood before the trip (x, y, z cm, pitch, yaw, roll
// deg), the place the return puts it back. Sends the ReturnRequest, arms the transfer and the
// return's world load, raises the curtain. False, logged, when it cannot start: already returning,
// no connected client session, or a send refused. Game thread.
bool Begin(coop::player::pocket_travel::Pocket from, coop::player::pocket_travel::Reason why,
           uint32_t tripSeq, const float departure[6]);

// Drive the phases: the transfer, the inventory profile, the load, the world-ready announce, the
// host's PlayerLocation. Every phase is bounded; a bound that runs out fails the return, and
// pocket_travel_client::OnReturnFinished hears the outcome either way. Called every session tick
// by pocket_travel_client::Tick, which net_pump runs on frames with no local player too (the load's
// world swap), so a bound runs out even when no world comes up. Game thread.
void Tick();

// A return is in flight (Begin succeeded, OnReturnFinished not yet called).
bool Active();

// The return has issued its world load and owes the new world's world-ready announce: the announce
// may go while the player is still counted away. Game thread.
bool AnnounceOwed();

// Stop a return in flight without a callback (the session is ending: net_pump's teardown, through
// pocket_travel::OnSessionEnd). Its panel and the join curtain it raised come down, also when the
// return has finished and only the host's bracket was left to lift the curtain. Game thread.
void Abort(coop::player::pocket_travel::Reason why);

// The host refused this slot's way home (its own PlayerLocation row with cause Refused): the return in
// flight fails on its next tick, and the client half ends the session cleanly. A no-op with no return
// in flight. Game thread.
void NoteRefusedByHost();

// Session start: forget any return. Game thread.
void OnSessionStart();

// The return's own cover, for the loading screen: up from Begin until the return ends. Any thread.
enum class CoverPhase : uint8_t { WaitingForHost = 0, Downloading, Loading };
struct CoverView {
    bool active = false;
    coop::player::pocket_travel::Pocket from = coop::player::pocket_travel::Pocket::Main;
    CoverPhase phase = CoverPhase::WaitingForHost;
    uint32_t doneBytes = 0;
    uint32_t totalBytes = 0;
    uint64_t phaseMs = 0;  // in the current phase
};
CoverView Cover();
// Take the cover down without touching the return (the overlay's fault guard). Any thread.
void HideCover();

}  // namespace coop::player::pocket_return_client
