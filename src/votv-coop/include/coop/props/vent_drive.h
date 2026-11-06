// coop/props/vent_drive.h -- HOST: the wall vent the ventCrawler event pulls down, streamed to the
// clients on the driven-prop channel.
//
// The ventCrawler event animates the prop_vent_C keyed `ventHanging` with its ventbreakTL timeline:
// every frame it sets the vent's transform, and when the timeline finishes it runs setPropProps(static
// false, frozen true, active false, sleep false) on it. The crawler itself is a host-only actor (a
// client's BeginPlay is refused, coop/world/spawn_authority), so on a client the vent never moved:
// the motion scan skips a kinematic prop (a Static vent simulates nothing), and no lane carried the
// animation.
//
// This is the feeder. It watches the timeline's two bodies by name. While the timeline runs, the
// vent is CLAIMED on the driven-prop channel (coop/props/prop_drive_host), which publishes its pose
// whenever it moved, kinematic or not; the Finished body releases the claim, the vent rests, and the
// channel's one reliable end edge carries the final transform and the host's flags (Static now off,
// Frozen on). A client applies that end through prop_drive_stream, whose narrow static-release step
// (coop/props/prop_wire_parity ConvergeStaticRelease) takes the vent out of Static so its copy lands in
// the host's state. The creature stays host-only; only the vent's motion crosses.
//
// Game thread. A client installs nothing it acts on: both bodies return at once unless this peer hosts.

#pragma once

namespace coop::net { class Session; }

namespace coop::vent_drive {

// Register the two name watches (idempotent) and remember the session. Any role.
void Install(coop::net::Session* session);

// Per gameplay tick: settle the watches; HOST: release a claim whose timeline went silent. Game thread.
void Tick();

// Session end: forget the claim.
void OnDisconnect();

}  // namespace coop::vent_drive
