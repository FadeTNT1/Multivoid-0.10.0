// coop/player/sleeper_snore_sound.h -- the snore's samples: a `snore.wav` beside main.dll when there is one,
// otherwise the recorded breath embedded in the DLL (resources/snore.rc, assets/snore/snore.wav), and only if that
// cannot be read a few short snores synthesized at first use (no snore asset ships in the game, and a SoundWave
// cannot be cooked without the 4.27 editor). Mono s16. Game thread.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::sleeper_snore_sound {

struct Clip {
    std::vector<int16_t> pcm;
    uint32_t rate = 0;
};

// Build or load the clips once (idempotent). Logs which source was used.
void Prepare();
// The clips, valid for the process (never freed), empty before Prepare.
const std::vector<Clip>& Clips();

// The synthesis alone (for Prepare and a drill): one snore, `lengthS` seconds, from `seed`.
Clip Synthesize(uint32_t seed, float lengthS);

}  // namespace coop::sleeper_snore_sound
