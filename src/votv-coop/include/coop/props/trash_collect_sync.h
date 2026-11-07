// coop/props/trash_collect_sync.h -- mirror a freshly-spawned-and-grabbed item (the VOTV trash-pile
// collect).
//
// Pressing E on a trashBitsPile_C ("trash stack") spawns one Aprop_C trash item and auto-grabs
// it into the player's hands the same frame. The item is born with Key == None, because its BP
// UCS has not minted a NewGuid yet, so the held-prop pose stream would emit unmatchable
// PropPose key='None' and the Init POST broadcast would skip it -- the held item never mirrors.
//
// The collect CANNOT be caught with a UFunction observer: trashBitsPile_C::playerTryToCollect is
// dispatched BP->BP through ProcessInternal, which bypasses our ProcessEvent detour entirely
// (measured: a POST observer installed and NEVER fired while the item was clearly held). So
// detection happens where the engine state IS visible -- net_pump's held-prop send, which
// already resolves grabbing_actor every tick. From the mint onward the existing pose stream
// carries the item into the collector's hands, and PropRelease/PropDestroy unwind it like any
// held prop.

#pragma once

#include "ue_wrap/core/types.h"  // FVector (the hard throw's launch direction)

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::trash_collect_sync {

// Install. Caches `session` (re-cached every call for reconnect) and arms two seams, each
// idempotent and retried on later calls until its class resolves. First, a POST Func patch on
// BeginDeferredActorSpawnFromClass: that is the deterministic clump-to-pile converter, and it
// is a Func patch because the clump's own spawn is EX_CallMath and invisible to ProcessEvent.
// Second, the InpActEvt_use family -- the client-grab bridge, the use-deny suppressors and the
// hard-throw bridge -- which lives in trash_use_intercept and is a PRE INTERCEPTOR, not an
// observer: it returns true to CANCEL the native use on a client grab or throw. Call from the
// world-gated subsystem install. Game thread.
void Install(coop::net::Session* session);

// Game thread. Express eligible held items so peers can resolve the held-pose stream.
// The host also re-expresses tracked keyed props; clients never re-author tracked mirrors.
// Returns true when a spawn is broadcast. Kerfurs, clumps, swingers and pending sweep
// candidates remain on their own synchronization paths.
bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* session);

// Drop the cached session (full session teardown / aggregate disconnect). The trash entity ctx
// map is cleared separately by trash_channel::OnDisconnect.
void OnDisconnect();

// TEST-ONLY (VOTVCOOP_RUN_GRAB_INTENT_TEST): send a GrabIntent{eid} from THIS client to the
// host, using the cached client session. Exercises the full client->host wire, the host's
// OnGrabIntent and the puppet hand-drive without the client suppress-native / collision path.
// No-op if the session is not a running client. Returns true iff sent. Game thread.
bool DebugSendGrabIntent(uint32_t eid);

// TEST-ONLY: send a ThrowIntent{eid} from THIS client -- the throw fallback for when the real
// InpActEvt_use toggle cannot be driven. No-op if not a running client. Returns true iff sent.
// Game thread.
bool DebugSendThrowIntent(uint32_t eid);

// TEST-ONLY: the hard throw, the LMB shape: a ThrowIntent{eid, kHardThrow, dir} from THIS client,
// so a drill can put a clump into a real arc. `dir` is the unit launch direction. Game thread.
bool DebugSendHardThrowIntent(uint32_t eid, const ue_wrap::FVector& dir);

}  // namespace coop::trash_collect_sync
