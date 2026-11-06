// coop/props/prop_food_state.h -- the food family's live state is the host's, and every peer sees the host's.
//
// A food does not hold still: its own tick lerps `temperature` toward the room, spends `freshnessAlpha` and the
// freshness count until it spoils, and at the ends of its range burns or freezes the actor into another; a cooking
// food's tick also adds `cookProgress` while it is hot and calls `cookItem`, which spawns the cooked prop and destroys
// the raw one; a fridge, a radiator and a microwave heat or cool it through accumulateTemperature, addTemperature and
// microwave. Every peer running that is a separate author of the same food, which is how the shrimp on one peer
// cooked and on the other did not.
//
// The live-prop save record cannot carry it: a record is captured at one birth and applied at another, while these
// fields move continuously, and the record is stamped with the CAPTURING peer's world clock, which the receiver's
// loadData reads as elapsed time. So the family stays claimed off that lane (prop_save_data's
// DeclareClassOwnedElsewhere), and this lane carries the state instead.
//
// The host owns the progression. On a client every shared food's tick (prop_food_C's, prop_cookingFood_C's and the
// two overrides that replace it, the blue brain cheese's and the coffee cup's), its heating (accumulateTemperature,
// addTemperature, microwave) and its cookItem are cancelled at the script-body gate; a food is shared when it is a
// tracked world prop or a keyed one outside a hand. The host's own tick runs as the game has it, and its births and
// deaths (a cooked replacement, a burnt one) ride the ordinary prop lifecycle. The host notes every food whose tick
// ran, reads its state round the table and sends FoodState when it moved past a step (half a kelvin, a fiftieth of
// freshness, half a unit of cookProgress), at most once a second a food, and at once on an edge (a bite, the
// freshness count, spoiled, frozen). A client writes the fields and draws what its tick would have shown: the
// celsius, the steam, and on an edge the game's own setSpoiled and freezed. A joiner is sent every noted food's
// state at its world-ready edge, paced.
//
// A client's bite of a shared food that does not finish it runs on the client (its player eats) and is reported as
// FoodBiteIntent with the bites left; the host judges the reach and that the count is one under its own, writes it
// and publishes. The finishing bite destroys the food and is the take-or-use request's (coop/props/prop_act_request).
//
// A food inside a container or a pocket is a stored record on its own lane and crosses whole, stamp included --
// docs/scope.md. A food in a player's hand is that player's own and is not touched here. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct FoodStatePayload;
struct FoodBiteIntentPayload;
}  // namespace coop::net

namespace coop::props::prop_food_state {

// Claim prop_food_C and every class below it from the live-prop record lane, cache the session, and register the
// watches once. The session install is a retry pump re-entered every net_pump tick, so it stays cheap after the
// first call.
void Install(coop::net::Session* session);

// The watches' settle; HOST: the noted foods' states read and published, the joiners' seeds paced; CLIENT: states
// parked for a food not here yet, tried again when the key index moves.
void Tick(coop::net::Session& session);

// HOST: a joiner's world is up; every noted food's state goes to it, paced.
void OnPeerWorldReady(int slot);

// CLIENT: the host's state for one food. HOST: a client's bite.
void OnState(const coop::net::FoodStatePayload& p);
void OnBite(coop::net::Session& session, const coop::net::FoodBiteIntentPayload& p, uint8_t senderSlot);

// The dispatcher's format checks: a key of 1..31 characters, finite fields in range, no unknown flag.
bool WellFormed(const coop::net::FoodStatePayload& p);
enum class Drop : uint8_t { Size, Malformed };
void CountDropped(Drop why);

// HOST: publish this food's state on the next tick, whatever the step (a lane that changed it out of the tick).
void MarkDirty(void* food);

// Net disconnect: the tables, the seeds, the parked states and the counters go, with one summary line.
void OnDisconnect();

}  // namespace coop::props::prop_food_state
