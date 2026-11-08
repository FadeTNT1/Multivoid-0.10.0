// coop/player/push_wake.h -- push to wake: a player walks up to a sleeper, presses right-click twice, and the sleeper
// is woken. The animation is coop/player/push_wake_animation; this is the decision and the wake.
//
// The HOST judges every request (a client's, shown by the transport's sender slot and never by the payload; the
// host's own press takes the same path): both players in the roster and alive, the sleeper resting in a bed or a
// sleeping bag (not dreaming, not fainted), the waker in reach, nobody else waking the sleeper, the sleeper's
// five-minute cooldown run out, and at most one request a second (burst two) from the sender. It reserves the
// sleeper, has every peer play the animation (Start), and when it ends revalidates and tells the sleeper's machine
// to wake (Execute), which calls the game's own gamemode.wakeup(). sleep_sync then publishes the awake state; the
// cooldown starts when the host sees it, keyed by the sleeper's identity (profile guid, else player number), not by
// slot. A leave, a death, a wake of the sleeper's own or an Execute that does not take ends the event for every
// peer (Cancel) and starts no cooldown. The press is two right-click edges within 350 ms on the same sleeper within
// reach, hands empty. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::push_wake {

void Install(coop::net::Session* session);

// Each frame, game thread: the local press and its prompt, the host's events and timers, a client's request timeout.
void Tick();

// A PushWakeRequest or PushWakeEvent (SleepState ops 3 and 4) from `senderSlot` (0xFF: unknown). Every field is
// checked against the receiver's role and the sender's slot; `len` must be the op's payload size.
void OnReliable(const uint8_t* payload, size_t len, uint8_t senderSlot);

// HOST: the player of `slot` left. Events naming it end, told to every peer, and its cooldown stays with its identity.
void OnSlotGone(int slot);

// Session end, any role: every event, view, cooldown and prompt state dropped.
void OnSessionEnd();

// The plate of the online player `slot` on this peer: what to offer the viewer. Wake when the viewer is aiming at
// them within reach with empty hands and may push; Cooldown when the host has said so (`cooldownSecs` is what is
// left); None otherwise. Game thread.
enum class Hint : uint8_t { None = 0, Wake = 1, Cooldown = 2 };
Hint HintFor(int slot, int* cooldownSecs);

}  // namespace coop::push_wake
