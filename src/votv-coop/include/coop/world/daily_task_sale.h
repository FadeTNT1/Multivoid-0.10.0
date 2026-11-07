// coop/world/daily_task_sale.h -- the drone sale seen from the daily task: what the grader counted, and its hints.
//
// droneSellLocation_C::sell is the one writer of task progress, and it runs on the host alone (a client's drone
// tick is suppressed; a client's box, zip floppy and reels reach it as rows of the drone's inventory). So a
// package any player sends is graded on the host, and the task, the progress mail and the reward reach every
// peer on their own lanes (TaskNewState, the email mirror, the balance). Two things of the sale stayed on the
// host: why a sold signal did or did not count (the progress mail says only "0 out of 3"), and the corner hints
// the grader raises (a sent tape's fill). This lane says the first in the log, item by item, and carries the
// second to every client (ReliableKind::TaskSaleHint). Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::daily_task_sale {

// Store the session. Idempotent. Game thread.
void Install(coop::net::Session* session);

// Per tick: HOST registers the sale and hint watches and says once whether the gate took them. Game thread.
void Tick();

// Wire handler (CLIENT): show one sale hint of the host's. A host's own copy never arrives (host-authored kind).
void OnTaskSaleHint(const coop::net::TaskSaleHintPayload& p);

}  // namespace coop::daily_task_sale
