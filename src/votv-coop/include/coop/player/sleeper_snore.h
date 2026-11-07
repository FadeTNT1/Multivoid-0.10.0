// coop/player/sleeper_snore.h -- the soft snore of a sleeping player, played locally on every peer.
//
// Nothing crosses the wire: each peer derives who sleeps from the sleeper state it already has (an offline
// sleeper's built body, an online player's settled lying pose; coop/player/sleepers SnoreSources) and plays a
// one-shot for each, every 3 to 6 s at random, through the voice output's device and mixer
// (coop/voice/voice_chat PlayWorldSound). It is full volume within a metre of the camera and silent by six, panned
// to the sleeper's head. With voice off, or no output device, it is silently off. Stops on a wake, a removed or
// destroyed sleeper, a disconnect, the session's end, or a world change (the body is gone), but not on looting.
// The sound itself is coop/player/sleeper_snore_sound. Game thread.

#pragma once

namespace coop::sleeper_snore {

// Every tick of the sleepers' tick; cheap when nobody sleeps or the snore is off.
void Tick();
// Cut every snore and forget the schedule (the session ended).
void StopAll();

}  // namespace coop::sleeper_snore
