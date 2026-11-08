// coop/player/push_wake_animation.h -- what a push to wake looks and sounds like on this peer: the sleeper's body rocked
// side to side, the waker's arms and body reaching toward it, and a quiet slap on each shove. Presentation only: it
// decides no outcome and writes no actor transform; validation, the timer and the wake belong to the caller.
//
// The sleeper rocks by moving its body mesh under the capsule, so the lying pose coop/player/sleepers owns is untouched.
// A slot with no puppet on this peer (the local player) simply has no body to move. Everything is put back when the
// shake ends, on CancelPushWake, and when either body goes. Game thread.

#pragma once

#include <cstdint>

namespace coop::push_wake_animation {

inline constexpr uint16_t kDefaultDurationMs = 1500;

// Begin the animation of event `eventId`; `durationMs` 0 means kDefaultDurationMs. An event already running with this
// id, this waker or this sleeper is ended and replaced. Refused, with a log line, for a bad or equal slot pair and for
// a remote sleeper not lying online on this peer.
void StartPushWake(uint32_t eventId, uint8_t wakerSlot, uint8_t sleeperSlot, uint16_t durationMs);
// End event `eventId` now and put everything back. An id not running is a no-op.
void CancelPushWake(uint32_t eventId);

// Each frame, after the puppets are driven: advances the running events.
void Tick();
// A slot's puppet is going (a leave, a session's end, a level change): events naming it end now.
void OnSlotGone(int slot);

}  // namespace coop::push_wake_animation
