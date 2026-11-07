/* miniaudio implementation TU (the single-header pattern). Devices, plus the built-in WAV decoder
 * (the sleeper snore's optional snore.wav, coop/player/sleeper_snore_sound); we do our own
 * mixing/spatialization, so no FLAC/MP3 decoder, resource manager, node graph or engine is compiled. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"
