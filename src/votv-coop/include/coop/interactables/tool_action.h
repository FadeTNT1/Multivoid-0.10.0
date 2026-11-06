// coop/interactables/tool_action.h -- a player's work on a shared target with a tool in hand, shown on every peer and
// decided once, on the host: a crowbar's pry of a pryable (coop/interactables/pry_sync) and a hacksaw's cut of a heavy
// prop (coop/interactables/saw_sync).
//
// Both are a session a player's own input drives on its own machine -- the pry's mouse bend, the saw's held LMB --
// that ends in a change of the world: the pryable's crowbarOpen and the crowbar given back, the strain's snap, the
// saw's scraps and the prop's destroy. The acting peer keeps its own input and its own screen (the pry's bar, the
// saw's progress) and announces the session's begin, progress and end; a client's are ToolActionIntent, which the
// host judges and re-sends as ToolActionState. The world-changing end runs once, on the host's copy, through the
// game's own verbs, and its births and deaths cross on the ordinary prop lanes; a client's own copy of that end is
// cut at the script-body gate. A receiver shows the session -- the crowbar in the pryable bent as its owner bends it,
// the saw's sound at the cut -- and runs no verb that changes the world. A joiner gets every running session at its
// ready edge. Wire and late join: docs/tool-actions.md. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ToolActionPayload;
}  // namespace coop::net

namespace coop::tool_action {

// Cache the session and register both kinds' watches. Re-entered by the install pump (verb_lanes::Install).
void Install(coop::net::Session* session);

// Settle the watches; the acting peer's progress, paced; HOST: the sessions whose owner or target is gone.
void Tick(coop::net::Session& session);

// The dispatcher's two entries, the payload's format already checked by WellFormed. HOST: a client's own action.
// CLIENT: the host's state.
void OnIntent(coop::net::Session& session, const coop::net::ToolActionPayload& p, uint8_t senderSlot);
void OnState(coop::net::Session& session, const coop::net::ToolActionPayload& p);

// The payload's bounds: a known kind, op and result, finite values in range, a key that fits, reserved zero.
bool WellFormed(const coop::net::ToolActionPayload& p);

// The dispatcher refused a payload: counted.
void CountDropped();

// HOST: every running session to a just-world-ready joiner (verb_lanes::ConnectReplayForSlot).
void ConnectReplayForSlot(int slot);

// HOST: `slot` left: its stuck crowbars drop into the world, its cut ends.
void OnPeerLeft(uint8_t slot);

// Net disconnect: the sessions and counters, with one summary line.
void OnDisconnect();

// ---- shared by the two kinds -------------------------------------------------------------------------------------
// Send `p` from this peer: a client's goes to the host as ToolActionIntent; the host's goes to every client but
// `skipSlot` as ToolActionState. False when the session refused it.
bool Send(coop::net::Session& session, const coop::net::ToolActionPayload& p, int skipSlot = -1);

// HOST: `p` to one client as ToolActionState.
bool SendToSlot(coop::net::Session& session, int slot, const coop::net::ToolActionPayload& p);

}  // namespace coop::tool_action
