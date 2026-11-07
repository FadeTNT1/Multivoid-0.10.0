// coop/voice/voice_playback.h -- receive-side voice: a per-slot jitter buffer, PLC and opus decode
// on the game thread, feeding per-slot PCM rings that a miniaudio callback mixes and spatializes. A
// faithful Simple Voice Chat port, summarised in docs/voice-and-chat.md; the numbers that
// make it faithful are a jitter threshold of 3 frames (in order delivers at once, out of order
// sorts, an overflow pops the oldest, a stop marker flushes the buffer and resets lastSeq and the
// decoder), a gap of up to 5 frames covered by opus PLC and a wider one resetting the decoder,
// dupes dropped, ~100 ms of prebuffer (5 frames) before a channel plays again after silence, a
// distance gain of 1 - d/maxDist (a whisper halves the radius; the range is the session's
// voice.distance_cm, 0 = unlimited), a vertical fade of 1 - |dz|/(range * kVerticalFadeRatio), the
// REDUCED-mode pan in [-0.5,0.5] (volume clamp(1 -+ pan*1.4, 0.3, 1)), talking = decoded <250 ms.
// Threading: OnFrame, TickDecode, SetListener and SetSpeaker are GAME THREAD; the miniaudio
// callback touches only the PCM rings (SPSC), the position atomics, the range atomic (distanceCm_)
// and the row's default (DefaultRangeCm(), a constant read). No engine access anywhere, and no
// allocation after Start beyond the lazily created per-slot opus decoders.
//
// World sounds (PlayWorldSound): the same device and mixer also play a few short, positioned one-shots
// that are not a peer's voice (the sleepers' snore, coop/player/sleeper_snore). Each is a fixed-pool
// emitter: the game thread fills one and publishes it with a release store of its `active` flag, the
// callback reads it after the matching acquire and clears the flag when it ends or is stopped. The
// samples are borrowed, mono s16, and must outlive the playback. The listener of a world sound is its
// own (SetWorldListener: the camera), not the voice listener.

#pragma once

#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::voice {

// The vertical fade scales with the range: 3200 cm of fade at the 4800 cm default (SVC's 32-block
// fade over its 48-block range).
constexpr float kVerticalFadeRatio = 3200.0f / 4800.0f;

// The row's default range, read where it lives.
inline float DefaultRangeCm() {
    return ::coop::config_registry::rows::voice_distance_cm.row->defF;
}

struct PlaybackConfig {
    std::string device;          // output device name substring ("" = system default)
    float volume = 1.0f;         // master voice volume 0..3 (SVC range)
    float distanceCm = 4800.0f;  // proximity radius (whisper = half)
    int   jitterThreshold = 3;   // 0 disables buffering
    int   prebufferFrames = 5;   // ~100 ms latency floor
};

class Playback {
public:
    bool Start(const PlaybackConfig& cfg);
    void Stop();
    bool running() const { return running_; }

    // Game thread: one wire frame for `slot` (jitter-buffer insert).
    void OnFrame(int slot, const coop::net::VoiceFramePayload& f);

    // Game thread: drain jitter buffers -> decode/PLC -> PCM rings.
    void TickDecode();

    // Game thread: position snapshots for the mixer (atomics).
    void SetListener(float x, float y, float z, float yawDeg);
    void SetSpeaker(int slot, float x, float y, float z, bool valid);

    // Game thread: drop a channel's stream state -- when its peer left, and for every slot at Stop,
    // which also runs at a device reopen while everyone stays. The per-person volume is not stream
    // state: voice_chat resets it where the person leaves.
    void ResetSlot(int slot);

    // ---- world sounds (game thread; see the file comment) ----
    // Start a one-shot of `frames` mono s16 samples at `rate` Hz, played `pitch` times as fast, at the
    // world point (x, y, z). Full gain (times `gain`) within `fullCm` of the world listener, falling
    // linearly to silence at `silentCm`. Returns a handle (non-zero), or 0 when the output is not
    // running or all emitters are busy.
    uint32_t PlayWorldSound(const int16_t* pcm, uint32_t frames, uint32_t rate, float pitch, float x,
                            float y, float z, float gain, float fullCm, float silentCm);
    void StopWorldSound(uint32_t handle);          // a stale or zero handle is a no-op
    bool WorldSoundPlaying(uint32_t handle) const;
    void SetWorldListener(float x, float y, float z, float yawDeg);
    static constexpr int kMaxEmitters = 8;

    // Icon surface (any thread). True if a frame decoded < 250 ms ago.
    bool IsTalking(int slot, bool* whispering = nullptr) const;

    // Live-tunable (dev menu).
    void SetMasterVolume(float v) { masterVolume_.store(v, std::memory_order_relaxed); }
    float MasterVolume() const { return masterVolume_.load(std::memory_order_relaxed); }
    // The proximity range (cm; 0 = unlimited): the session's voice.distance_cm, applied live by
    // the row's subscriber; the mixer thread reads it.
    void SetDistanceCm(float cm) { distanceCm_.store(cm, std::memory_order_relaxed); }
    float DistanceCm() const { return distanceCm_.load(std::memory_order_relaxed); }
    void SetSlotVolume(int slot, float v);
    float SlotVolume(int slot) const;

    static std::vector<std::string> EnumerateDevices();

private:
    struct Channel {
        // Jitter buffer (GT only).
        coop::net::VoiceFramePayload buf[16];
        int   bufCount = 0;
        bool  flushing = false;
        int64_t lastSeq = -1;
        void* decoder = nullptr;  // OpusDecoder* (lazy)

        // PCM ring: the game thread produces in TickDecode, the audio callback consumes. The
        // monotonic counters are 64-bit because 32-bit ones wrap after ~24.8 h of continuous talk
        // on one channel.
        static constexpr uint32_t kRingSamples = 48000;  // 1 s
        int16_t ring[kRingSamples];
        std::atomic<uint64_t> ringWrite{0};
        std::atomic<uint64_t> ringRead{0};
        std::atomic<bool> primed{false};

        // TalkCache + per-frame whisper state (icons + attenuation radius).
        std::atomic<int64_t> lastFrameMs{0};
        std::atomic<bool> whispering{false};

        // Mixer position (GT writes, callback reads; per-component atomics --
        // a torn read across components misplaces one 10 ms block, inaudible).
        std::atomic<float> posX{0}, posY{0}, posZ{0};
        std::atomic<bool> posValid{false};

        std::atomic<float> volume{1.0f};
    };

    struct Emitter {
        std::atomic<bool> active{false};  // published last by the game thread; cleared by the callback
        std::atomic<bool> stop{false};
        std::atomic<uint32_t> gen{0};
        // Written by the game thread before `active` is released; then read-only to the callback.
        const int16_t* pcm = nullptr;
        uint32_t frames = 0;
        float step = 1.0f;  // source frames per output frame (rate / 48000 * pitch)
        float x = 0, y = 0, z = 0;
        float gain = 1.0f, fullCm = 100.0f, silentCm = 600.0f;
        double cursor = 0.0;  // callback only
    };

    void DeliverInOrder(Channel& ch, const coop::net::VoiceFramePayload& f);
    void PushPcm(Channel& ch, const int16_t* samples, int count);
    void MixOutput(float* out, uint32_t frameCount);
    void MixWorldSounds(float* out, uint32_t frameCount);

    Channel channels_[coop::players::kMaxPeers];
    Emitter emitters_[kMaxEmitters];
    std::atomic<float> worldX_{0}, worldY_{0}, worldZ_{0}, worldYaw_{0};

    std::atomic<float> listenerX_{0}, listenerY_{0}, listenerZ_{0}, listenerYaw_{0};
    std::atomic<float> masterVolume_{1.0f};
    std::atomic<float> distanceCm_{4800.0f};
    int jitterThreshold_ = 3;
    int prebufferFrames_ = 5;

    bool running_ = false;
    void* device_ = nullptr;   // ma_device*
    void* context_ = nullptr;  // ma_context*

    friend void PlaybackDataCallback(void*, void*, const void*, unsigned int);
};

}  // namespace coop::voice
