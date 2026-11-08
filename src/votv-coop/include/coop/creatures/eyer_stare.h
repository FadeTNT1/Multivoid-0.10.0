// coop/creatures/eyer_stare.h -- the two script-gate watches that make the night stalker (eyer_C) correct
// per player: a stare at another peer's display copy hands that copy to the staring machine, and an eyer
// never ends itself on a peer's puppet.
//
// An eyer judges the stare, the chase and the kill against its own machine's player 0 (islooking, the
// ReceiveTick pursuit and the kill sphere's overlap). Its display copy on another peer (owner_entity_sync) is
// parked but its timers still run, so the copy builds anger from THIS machine's player and reaches goAngry
// when that player stares. The goAngry watch hands the copy to owner_entity_sync::OnMirrorAngry, which lets
// it run natively on the staring machine. The kill sphere's overlap with a peer's puppet (a mainPlayer_C
// that is not this machine's player) is cancelled: the body would otherwise play the bite sounds, raise the
// blackout on this screen and destroy the eyer without hurting anyone, since a puppet's damage is refused.

#pragma once

namespace coop::eyer_stare {

// Register the watches. Idempotent and retried until both registered; net-pump ensure, through
// owner_entity_sync::Install.
void Install();

}  // namespace coop::eyer_stare
