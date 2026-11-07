// coop/player/sleeper_drag.h -- carrying a sleeper's body somewhere else (ReliableKind::SleeperDrag).
// Gameplay/network layer (principle 7); the body itself is coop/player/sleepers', reached through its drag seams.
//
// Any player can pick up an offline sleeper (coop/player/sleepers: the body a player who left lies in) and carry it:
// hold the left mouse button while aiming at the body within reach, move, release to put it down. While it is held
// the body follows a point about 1.5 m in front of the carrier's camera, on the floor traced under that point, still
// lying, turned to face the carrier, eased there over a few frames.
//
// The host owns the sleeper's pose. A client's press asks (Start); the host takes one carrier per body and answers
// every peer with a Moved that names the carrier, which is the client's go-ahead (one naming another slot is a
// refusal). The carrier streams the spot it holds the body at (Pose, about 10 Hz) and ends with the final spot (End);
// the host checks each against the carrier's own puppet, places the body and sends every other peer Moved, which
// they ease toward. At the end the host writes the spot into the sleeper's record and its owner's profile, so the
// player wakes there and a saved world keeps it. A carrier that leaves, goes quiet for 3 s, or whose body stops
// being a sleeper (its owner joining, a kick) puts it down where it is. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct SleeperDragPayload;
}  // namespace coop::net

namespace coop::sleeper_drag {

// Cache the session (from coop::sleepers::Install).
void Install(coop::net::Session* session);

// Per tick (from coop::sleepers::Tick): the press and release, the local carry, the other peers' carries eased, and
// on the host a silent carrier's timeout.
void Tick();

// A SleeperDrag the receive edge accepted (exactly sizeof the payload). `senderSlot` is the transport's sender.
void OnReliable(const coop::net::SleeperDragPayload& payload, int senderSlot);

// The player of `slot` is leaving: a body they carry is put down where it is (host); a client stops easing it.
void OnPlayerLeaving(int slot);

// The session ended: every carry is dropped.
void OnSessionEnd();

}  // namespace coop::sleeper_drag
