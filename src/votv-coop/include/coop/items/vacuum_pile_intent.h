// coop/items/vacuum_pile_intent.h -- a vacuum's pop of a dispenser pile is the host's, whoever holds the vacuum.
// Gameplay/network layer (principle 7); the engine is reached through ue_wrap::prop.
//
// The vacuum's gather timer, on whichever peer holds it, traces from its holder's arm, overlaps a sphere at
// the hit and calls `vacuumed` on each dispenser pile (trashBitsPile_C) it lands on; the body pops one item
// at a rolled transform, decrements the counters and destroys the pile on the last. On a client all of that
// was the client's alone: the item it popped was a client prop birth, which the drop drain refuses at the
// door, and the counter drop was the client's word. So the pile emptied on one screen only.
//
// This lane does for the vacuum what coop/items/broom_stroke does for the broom: on a client it refuses the
// pile's `vacuumed` body at the script-body gate and sends the pile's key and the verb's `location`. The host
// re-tests the pile, the sender's held vacuum and the reach, and runs `vacuumed` on its own pile, so the item
// rides the host's spawn expression, the counters the trash pile lane and the depletion the keyed destroy
// (docs/piles.md). On the host the same watch names the pile to the depletion watch before the body runs
// and opens the broom's dispense window, so the popped item's fall streams as a stroke's does. The rest of
// the vacuum -- its pull on loose props and what it gathers into itself -- is untouched here.
// Late join: nothing to replay, since a vacuum the host has not run changed nothing.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct VacuumPileIntentPayload;
}  // namespace coop::net

namespace coop::vacuum_pile_intent {

// Watch the pile's `vacuumed` by name and cache the session. Idempotent; the per-tick retry pump. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: settle the watch; HOST: run one queued intent per client per token. Game thread.
void Tick(coop::net::Session& session);

// HOST: a client's vacuum landed on a dispenser pile. Queued here and run from Tick under the rate limit.
void OnVacuumPileIntent(coop::net::Session& session, const coop::net::VacuumPileIntentPayload& payload,
                        uint8_t senderSlot);

// Session teardown: report the tally. A sender's queue lives in its per-slot state and goes with its occupant.
void OnDisconnect();

}  // namespace coop::vacuum_pile_intent
