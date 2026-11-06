// coop/props/fire_extinguisher_sync.h -- a fire extinguisher's burst is the host's, and every peer sees it.
//
// An extinguisher (prop_fireExt_C) bursts when a hit over 5 lands on it, or a physics impact, or a fire's damage:
// the guards of addDamage, receivedPhyiscsDamage and fireDamage call its `shoot`, which starts the thruster, the
// flame particles and the roar, sets `active`, and runs a latent loop that drains `life` and randomly extinguishes
// what its ray reaches. Nothing in that path is on the wire, so a burst a client caused ran on that client alone and
// one the host caused left the clients with an extinguisher that never moved.
//
// The host owns the burst. A client's swing and a client's thrown contact on an extinguisher are the host's as a
// world container's are (coop/props/crate_break: the extinguisher is host-damaged, so the swing is cancelled and sent
// as a HitIntent, the contact as a PropContactIntent, and the host runs the game's own damage on its copy). On a
// client `shoot` itself is cancelled unless this module is replaying the host's burst, so no other route into it
// (a guard of the subclass, a loadData) starts one there.
//
// The state travels as the extinguisher's own save record (coop/props/prop_save_data), which carries `life` and
// `active`. The host marks a prop whose `shoot` ran, publishes its record, republishes it about once a second while
// the charge lasts and once more when it is spent, and claims the prop's motion under the drive stream
// (coop/props/prop_drive_host) while it thrusts, so a rocketing extinguisher moves on every peer. A client lands the
// record through a narrow apply of its own (a prop_record_gates applier): it writes `life` and `active` and nothing
// else, even while the prop is driven here, starts the burst once with the game's own `shoot` on the edge, and stops
// the components natively when the charge is spent. The client's loop never extinguishes: the host's does, and fire
// reaches the clients by their own lanes. A peer that joins mid-burst takes the record with the prop's spawn, whatever
// the order the pose and the record arrive in. Game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::props::fire_extinguisher_sync {

// Cache the session; register the watches and the record rules once per process. Called each time the install
// fanout runs.
void Install(coop::net::Session* session);

// The watches' settle; HOST: the bursts marked, claimed and published.
void Tick();

// Net disconnect: the marks, the replayed set and the once-per-session latches go.
void OnDisconnect();

}  // namespace coop::props::fire_extinguisher_sync
