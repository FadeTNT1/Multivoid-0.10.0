// coop/player/pocket_travel_host.cpp -- the host's half of a pocket trip. The shape is in the
// header; docs/pocket-dimensions.md is the design.
//
// SKELETON: the host implementation lands here.

#include "coop/player/pocket_travel_host.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

namespace coop::player::pocket_travel_host {

void Install(coop::net::Session*) {}
void Tick() {}
void OnSessionStart() {}
void OnDisconnectForSlot(uint8_t) {}
void OnPocketTravel(uint8_t, const coop::net::PocketTravelPayload&) {}

ue_wrap::script_gate::Verdict OnLocalLoadLevel(coop::player::pocket_travel::Pocket, const wchar_t*,
                                               void*) {
    return ue_wrap::script_gate::Verdict::Run;
}

bool IsSlotAway(uint8_t) { return false; }
void SendLocationsToSlot(uint8_t) {}

}  // namespace coop::player::pocket_travel_host
