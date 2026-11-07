// coop/interactables/piece_break_fx.h -- the break particles and tear-off sound of a pried wall piece, shown on every
// peer (ReliableKind::PieceBreakFx). Gameplay/network layer (principle 7); the engine is reached only through
// ue_wrap::asologo_piece.
//
// A crowbar's pry of a prop_asologoPiece_C family prop (a wall radiator, a security TV) ends in the host's
// crowbarOpen -> breakingSequence, which calls the piece's own break() (its break particles and its tear-off sound)
// and then frees the body and breaks its world constraints. The host's copy is the only one that runs the sequence, so
// the other peers saw the piece fall, through the prop lanes, with neither effect. The host watches break() after it
// ran and sends the piece's element id to every client, the one whose player pried it too, since that client's own end
// was cut at the gate. A receiver runs break() alone on its copy, never the sequence, whose physics and constraints
// the prop lanes already carry. A copy that has not resolved yet is retried for a short while. A break() that ran
// natively on a client (its own damage) is noted, so the host's cue does not play it a second time. Wire and late
// join: docs/tool-actions.md. Game thread.

#pragma once

namespace coop::net {
class Session;
struct PieceBreakFxPayload;
}  // namespace coop::net

namespace coop::piece_break_fx {

// Cache the session; register the break() watch once per process (both roles: it sends on a host and notes on a
// client). Called each time the install fanout runs.
void Install(coop::net::Session* session);

// The watch's settle; CLIENT: the cues whose copy had not resolved yet. Game thread.
void Tick();

// Is `payload` (`len` bytes) well formed: a non-zero element id and a zero reserved word. The receive edge asks before
// OnFx.
bool WellFormed(const coop::net::PieceBreakFxPayload& payload, int len);

// CLIENT: the host's cue.
void OnFx(const coop::net::PieceBreakFxPayload& payload);

// Net disconnect: the pending cues and the rate state go, with one summary line.
void OnDisconnect();

}  // namespace coop::piece_break_fx
