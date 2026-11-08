// coop/interactables/microwave_sync.h -- the microwave is the host's.
//
// A microwave (prop_microwave_C) sets its time on action 4 (`timeMode` mod 5, `timer` 5..60 s) and starts on action 8
// (`wave()`), whose latent loop spends `time`, turns the plate and calls microwave(self) and microwaveElec() on what is
// inside, and ends with the ding; opening the door while it runs is the same stop (forceOpenDoor). None of that is in
// its save record and none of it was on a wire, so each peer ran its own microwave, and the food in it cooked on the
// peer whose player pressed the button.
//
// The host owns it. On a client both presses are cancelled at the script-body gate and sent as MicrowaveIntent with
// the microwave's key and the action; a press the client cannot send runs on its own copy. The host refuses a sender
// that is not alive or out of reach and runs the game's own action once with the sender's puppet. A client's `wave`
// is cancelled from any other route, and since a loop already running there (one started before the lane) is not
// stopped by that, its effects are cancelled too: every class's microwave and microwaveElec called from a microwave's
// frame. The host's loop heats the food, which reaches the clients by the food lane (coop/props/prop_food_state).
//
// The host notes every microwave a press, a wave or its own tick touched, and sends MicrowaveState -- the mode, the
// set time, what is left, running or not, and a ding on the stop -- on every change and once a second while it runs;
// a joiner gets every noted microwave's at its ready edge. A client writes the mode and draws the run as wave does
// (the light, the loop sound) and its stop as the loop does (the ding, the light, the sound), and turns the plate
// itself while it runs and is powered. The door is the swinger lane's (coop/interactables/interactable_sync).
// The same message with the skull-fire flag is an animal skull's cue (coop/interactables/skull_fire_sync). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct MicrowaveIntentPayload;
struct MicrowaveStatePayload;
}  // namespace coop::net

namespace coop::microwave_sync {

// Cache the session and register the watches once. Re-entered by the install pump.
void Install(coop::net::Session* session);

// The watches' settle; HOST: the queued presses run, the noted microwaves read and published; CLIENT: the running
// plates turned.
void Tick(coop::net::Session& session);

// HOST: a joiner's world is up: every noted microwave's state to it.
void OnPeerWorldReady(int slot);

// HOST: a client's press. CLIENT: the host's state.
void OnIntent(coop::net::Session& session, const coop::net::MicrowaveIntentPayload& p, uint8_t senderSlot);
void OnState(const coop::net::MicrowaveStatePayload& p);

// The dispatcher's format check of a state, and its drop count.
bool WellFormed(const coop::net::MicrowaveStatePayload& p);
void CountDropped();

// Net disconnect: the tables, queues and counters go, with one summary line.
void OnDisconnect();

}  // namespace coop::microwave_sync
