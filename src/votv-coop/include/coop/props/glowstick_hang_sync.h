// coop/props/glowstick_hang_sync.h -- a glowstick hung on world geometry (a ceiling, a wall), mirrored across peers.
//
// The glowstick (prop_glowstick_C and its coloured subclasses) does not stick through the wall-attach component, so
// prop_stick_sync never saw it. Its own tick traces forward while it is held; on a hit stickto sets `sticked`,
// `stickstatic` and `loc`, moves the `hook` component to the impact point and runs hookdelay, which binds a physics
// constraint between the hook and the stick's mesh. Neither the base prop's frozen nor Static is set, the stick
// keeps simulating on the constraint, and nothing reached the wire: the sticking peer saw it hang, every other
// peer's copy was released as an ordinary drop and fell to the floor.
//
// The lane is the stick lane's, on its message: a watch on stickto (post) and releaseglowstick (pre) records the
// edge, the next pump pass (before local_streams' release edge, as for the camera) broadcasts a PropStickState, and
// a receiver replays the stick's own verbs. The message keeps the layout; a glowstick's reuse of it:
//   flags   = kHungFlag (4) for a hang on world geometry, 0 for the release (as for the wall-attach stick)
//   loc/rot = the stick's actor pose at the edge
//   linVel  = on a hang, the hook point (`loc`, the impact point); on a release, the velocity it leaves with
//   angVel  = on a release, the angular velocity
// A peer that does not know the flag drops the message as one for a prop with no wall-attach component, so the
// protocol version stays. A stick on a prop that simulates (stickstatic false) needs the hit component and is not
// mirrored; the sender says so once. A joiner is sent every hung stick at its ready edge, after the snapshot's end.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct PropStickStatePayload;
}  // namespace coop::net

namespace coop::glowstick_hang_sync {

// The stick's two watches, once a session; idempotent. Caches `session`.
void Install(coop::net::Session* session);

// Drain the stick and release edges recorded since the last pass, verify them against the stick's members, and
// broadcast. MUST run in the pump pass before local_streams' release edge. Game thread.
void Tick();

// prop_stick_sync's receiver hands every resolved prop here first: a glowstick's hang or release is replayed and
// true returned; any other prop is left for the wall-attach path (false). `senderPeerSlot` is the message's origin.
bool OnStickState(const coop::net::PropStickStatePayload& payload, void* prop, const std::wstring& keyW,
                  uint8_t senderPeerSlot);

// The hold of a glowstick began on another peer: its own grab verbs release the stick first, which a copy hung here
// takes. A no-op on a copy that hangs nowhere. Not mirrored back. Game thread.
void ReplayRelease(void* actor);

// HOST: every hung stick to the joiner in `slot`, at its ready edge.
void ConnectReplayForSlot(int slot);

// Net disconnect: drop the pending records.
void OnDisconnect();

}  // namespace coop::glowstick_hang_sync
