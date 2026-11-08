// coop/player/push_wake_internal.h -- what the three files of push to wake share (push_wake, push_wake_host,
// push_wake_input). Nothing outside coop/player includes it.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::push_wake::detail {

using coop::net::PushWakeReason;

inline constexpr float    kReachCm = 250.f;           // the local aim: as the loot press (coop/player/sleepers)
inline constexpr float    kHostReachCm = 450.f;       // the host's judge: the aim's reach plus a stream's lag and a camera's lift
inline constexpr uint64_t kDoublePressMs = 350;       // two right-click edges this close are one push
inline constexpr uint64_t kCooldownMs = 5ull * 60 * 1000;
inline constexpr uint64_t kExecuteWaitMs = 4000;      // an Execute that has not taken by then failed
inline constexpr uint64_t kRequestWaitMs = 2000;      // a client asks again no sooner (the host's answer is Start or Denial)
inline constexpr uint16_t kDurationMs = 1500;         // push_wake_animation::kDefaultDurationMs

// ---- shared (push_wake.cpp) ----
coop::net::Session* Sess();
bool IsHost();
// Where the player of `slot` is on this peer: the local body, or the puppet. False when neither reads.
bool PositionOf(int slot, ue_wrap::FVector& out);
// Alive by what this peer holds: the local body's dead flag, or a puppet's streamed health.
bool IsAlive(int slot);
// The slot is a player lying in a bed or a sleeping bag on this peer: Resting with a bed, not a faint, and (for a remote
// slot) shown lying. The sleeper a push can wake.
bool IsWakeableSleeper(int slot);
// The local wake: the sleeper's own machine, after the host's word. False when it is not resting in a bed.
bool WakeLocalSleeper(uint32_t eventId);
// A localized hint on this player's screen.
void ShowHint(const std::string& utf8);
// The text of a refusal; `cooldownSecs` is what is left on a Cooldown.
void ShowDenial(PushWakeReason why, uint16_t cooldownSecs);
// Send one PushWakeEvent: to `toSlot`, or to every client when it is below zero.
void SendEvent(coop::net::Session& s, int toSlot, coop::net::PushWakeWhat what, uint32_t eventId, uint8_t waker,
               uint8_t sleeper, uint16_t durationMs, uint16_t cooldownSecs, PushWakeReason why);

// ---- the host (push_wake_host.cpp) ----
// A request from `waker` (0 for the host's own press) for `target`: judged, and started or refused. A refusal reaches
// the requester (a client by a Denial, the host on its own screen).
void HostRequest(coop::net::Session& s, int waker, int target);
void HostTick(coop::net::Session& s);
void HostOnSlotGone(coop::net::Session& s, int slot);
void HostReset();

// ---- the local press (push_wake_input.cpp) ----
void InputTick(coop::net::Session& s);
void InputReset();
// The slot the local player aims at and may push, or -1; read by the nameplate through HintFor.
int AimedSlot();
// A request this peer sent that is still unanswered (a client), so it does not send another.
bool RequestPending();
void NoteRequestSent();
void NoteRequestAnswered();
// The cooldown the host last reported for `slot`'s player, in seconds left, or 0.
int KnownCooldownSecs(int slot);
void NoteCooldown(int slot, uint16_t secs);

}  // namespace coop::push_wake::detail
