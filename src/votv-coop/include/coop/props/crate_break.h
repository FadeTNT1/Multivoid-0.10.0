// coop/props/crate_break.h -- a world container's damage, break and spill are the host's.
//
// A supply crate (prop_container_sbox_C) and every other prop_container_C takes damage through its physicsImpact
// component's impactDamage, and its break spills what it holds as fresh props. Those contents are the host's
// (coop/props/container_contents_sync), so a break on a client's copy would spill a second set the host never sees.
// A client therefore runs no damage on a container: a held swing's addDamage is cancelled and sent as a HitIntent
// (coop/player/hit_intent), which the host judges and runs as the container's own addDamage on its copy; an impact on
// the container's mirror from a prop this client throws or syncs (a thrown rock), or by the container this client
// threw, is cancelled (coop/props/prop_damage_gate) and sent as a PropContactIntent, which the host judges and runs as
// the component's own impactDamage. The host's break and spill then travel the ordinary lanes: the births through
// host_spawn_watcher, the death through the destroy seam. A client's own copy, destroyed by the host's word, spills
// nothing: a prop born on a client while a container's destroy or break body runs there is dissolved. The cues the
// component plays reach every peer through coop/props/prop_impact_fx. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct HitIntentPayload;
struct PropContactIntentPayload;
}  // namespace coop::net

namespace ue_wrap::script_gate {
struct Call;
}  // namespace ue_wrap::script_gate

namespace coop::props::crate_break {

// Is `prop` a world container or a fire extinguisher, whose damage only the host runs? Any role. The extinguisher is
// one so that its burst starts on the host and travels as its record (coop/props/fire_extinguisher_sync).
bool IsHostDamaged(void* prop);

// CLIENT: comp_physicsImpact_C::impactDamage call `c` on the mirror of `container`, which prop_damage_gate cancels.
// When the hitting actor is a prop this client threw or syncs (its throw record or its syncer row), or the container
// is itself, the contact goes to the host; a causer with no element id yet waits for it a short while. Any other
// impact is the host's own copy's to take, and is said once per causer class.
void OnClientImpact(const ue_wrap::script_gate::Call& c, void* container);

// HOST: the judged swing (coop/player/hit_judge passed) of the client in `slot` at container `eid`: the container's own
// addDamage on the host's copy, with the slot's puppet as the damager and the hit on its StaticMesh. False with `why`
// when the container is gone, the struck point lies off it, or the call did not run.
bool HostApplySwing(uint32_t eid, uint8_t slot, const coop::net::HitIntentPayload& judged, const char*& why);

// HOST: a client's contact intent, queued. The format was checked at the receive edge.
void OnContactIntent(int senderSlot, const coop::net::PropContactIntentPayload& p);

// HOST: true while the host runs a client's swing or contact on a container (its no-damage impact is still a cue).
bool HostApplying();

// CLIENT: true while a container's destroy or break body runs on this thread here; a prop born now is its spill.
bool InsideMirrorSpill();

// CLIENT: `actor`, born inside a container's destroy or break here, is a second spill: destroyed next tick, unsent.
void DissolveSpill(void* actor);

// Cache the session; register the container watches once per process. Called each time the install fanout runs.
void Install(coop::net::Session* session);

// The watches' settle; CLIENT: the waiting contacts and the dissolves; HOST: each sender's queued contact, judged.
void Tick();

// Net disconnect: the queues, the waits, the counts and the once-per-session latches go.
void OnDisconnect();

}  // namespace coop::props::crate_break
