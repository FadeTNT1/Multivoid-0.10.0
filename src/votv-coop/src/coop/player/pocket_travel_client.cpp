// coop/player/pocket_travel_client.cpp -- the client's half of a pocket trip. The shape is in the
// header; docs/pocket-dimensions.md is the design.
//
// SKELETON: the client implementation lands here.

#include "coop/player/pocket_travel_client.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

namespace coop::player::pocket_travel_client {

void Install(coop::net::Session*) {}
void Tick() {}
void OnSessionStart() {}
void OnPlayerLocation(const coop::net::PlayerLocationPayload&) {}

ue_wrap::script_gate::Verdict OnLocalLoadLevel(coop::player::pocket_travel::Pocket, const wchar_t*,
                                               void*) {
    return ue_wrap::script_gate::Verdict::Run;
}

bool LocalAway() { return false; }

}  // namespace coop::player::pocket_travel_client
