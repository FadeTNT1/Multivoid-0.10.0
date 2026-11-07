// coop/voice/voice_playback.cpp -- see coop/voice/voice_playback.h.

#include "coop/voice/voice_playback.h"

#include "ue_wrap/core/log.h"

#include <chrono>
#include <cmath>
#include <cstring>

#include <miniaudio.h>
#include <opus.h>

namespace coop::voice {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrameSamples = 960;       // 20 ms
constexpr int kMaxPlcFrames = 5;         // SVC output_buffer_size: gap compensation cap
constexpr int64_t kTalkTimeoutMs = 250;  // SVC TalkCache
constexpr float kPi = 3.14159265358979f;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// SVC REDUCED-mode pan: lateral/forward in listener space (UE: yaw 0 = +X, right = (-sin yaw,
// cos yaw)); the two channel volumes clamp(1 -+ pan * 1.4, 0.3, 1). Shared by the voice channels and
// the world sounds.
void PanVolumes(float dx, float dy, float dist, float yawRad, float& volL, float& volR) {
    const float fwd = dx * std::cos(yawRad) + dy * std::sin(yawRad);
    const float lat = -dx * std::sin(yawRad) + dy * std::cos(yawRad);
    float pan = 0.0f;
    if (dist > 1.0f) {
        pan = std::atan2(lat, fwd) / kPi;  // [-1, 1]
        if (pan > 0.5f) pan = 0.5f;        // SVC clamps to +-0.5
        if (pan < -0.5f) pan = -0.5f;
    }
    volL = 1.0f - pan * 1.4f;
    volR = 1.0f + pan * 1.4f;
    if (volL > 1) volL = 1;
    if (volL < 0.3f) volL = 0.3f;
    if (volR > 1) volR = 1;
    if (volR < 0.3f) volR = 0.3f;
}

}  // namespace

// miniaudio playback callback (pUserData = the Playback*); f32 stereo out.
void PlaybackDataCallback(void* dev, void* out, const void* /*in*/, unsigned int frameCount) {
    auto* device = static_cast<ma_device*>(dev);
    auto* self = static_cast<Playback*>(device->pUserData);
    if (!self || !out) return;
    self->MixOutput(static_cast<float*>(out), frameCount);
}

bool Playback::Start(const PlaybackConfig& cfg) {
    if (running_) return true;
    masterVolume_.store(cfg.volume, std::memory_order_relaxed);
    distanceCm_.store(cfg.distanceCm, std::memory_order_relaxed);
    jitterThreshold_ = cfg.jitterThreshold;
    prebufferFrames_ = cfg.prebufferFrames;

    auto* ctx = new ma_context();
    if (ma_context_init(nullptr, 0, nullptr, ctx) != MA_SUCCESS) {
        UE_LOGW("voice_playback: ma_context_init failed");
        delete ctx;
        return false;
    }
    context_ = ctx;

    ma_device_id chosenId{};
    bool haveId = false;
    if (!cfg.device.empty()) {
        ma_device_info* outs = nullptr;
        ma_uint32 outCount = 0;
        if (ma_context_get_devices(ctx, &outs, &outCount, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < outCount; ++i) {
                if (std::strstr(outs[i].name, cfg.device.c_str())) {
                    chosenId = outs[i].id;
                    haveId = true;
                    UE_LOGI("voice_playback: device '%s'", outs[i].name);
                    break;
                }
            }
        }
        if (!haveId)
            UE_LOGW("voice_playback: no output device matches '%s' -- using default",
                    cfg.device.c_str());
    }

    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.sampleRate = kSampleRate;
    dc.dataCallback = [](ma_device* d, void* o, const void* i, ma_uint32 n) {
        PlaybackDataCallback(d, o, i, n);
    };
    dc.pUserData = this;
    if (haveId) dc.playback.pDeviceID = &chosenId;

    auto* dev = new ma_device();
    if (ma_device_init(ctx, &dc, dev) != MA_SUCCESS) {
        UE_LOGW("voice_playback: ma_device_init(playback) failed -- voice muted locally");
        delete dev;
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
        return false;
    }
    if (ma_device_start(dev) != MA_SUCCESS) {
        UE_LOGW("voice_playback: ma_device_start(playback) failed");
        ma_device_uninit(dev);
        delete dev;
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
        return false;
    }
    device_ = dev;
    running_ = true;
    UE_LOGI("voice_playback: output running (48 kHz stereo f32, radius %.0f cm, "
            "jitter %d, prebuffer %d)",
            distanceCm_.load(std::memory_order_relaxed), jitterThreshold_, prebufferFrames_);
    return true;
}

void Playback::Stop() {
    if (!running_) return;
    running_ = false;
    if (device_) {
        auto* dev = static_cast<ma_device*>(device_);
        ma_device_uninit(dev);  // joins the callback; rings are safe to reset after
        delete dev;
        device_ = nullptr;
    }
    if (context_) {
        auto* ctx = static_cast<ma_context*>(context_);
        ma_context_uninit(ctx);
        delete ctx;
        context_ = nullptr;
    }
    for (int i = 0; i < coop::players::kMaxPeers; ++i) ResetSlot(i);
    for (Emitter& e : emitters_) e.active.store(false, std::memory_order_release);
}

void Playback::ResetSlot(int slot) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    Channel& ch = channels_[slot];
    ch.bufCount = 0;
    ch.flushing = false;
    ch.lastSeq = -1;
    if (ch.decoder) {
        opus_decoder_destroy(static_cast<OpusDecoder*>(ch.decoder));
        ch.decoder = nullptr;
    }
    ch.ringWrite.store(0);
    ch.ringRead.store(0);
    ch.primed.store(false);
    ch.lastFrameMs.store(0);
    ch.whispering.store(false);
    ch.posValid.store(false);
}

void Playback::SetListener(float x, float y, float z, float yawDeg) {
    listenerX_.store(x, std::memory_order_relaxed);
    listenerY_.store(y, std::memory_order_relaxed);
    listenerZ_.store(z, std::memory_order_relaxed);
    listenerYaw_.store(yawDeg, std::memory_order_relaxed);
}

void Playback::SetSpeaker(int slot, float x, float y, float z, bool valid) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    Channel& ch = channels_[slot];
    ch.posX.store(x, std::memory_order_relaxed);
    ch.posY.store(y, std::memory_order_relaxed);
    ch.posZ.store(z, std::memory_order_relaxed);
    ch.posValid.store(valid, std::memory_order_relaxed);
}

void Playback::SetSlotVolume(int slot, float v) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    channels_[slot].volume.store(v, std::memory_order_relaxed);
}

float Playback::SlotVolume(int slot) const {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return 1.0f;
    return channels_[slot].volume.load(std::memory_order_relaxed);
}

bool Playback::IsTalking(int slot, bool* whispering) const {
    // `whispering` is written on EVERY return, not only the talking one: an out-param the caller
    // has to pre-initialise to read it safely is one a future caller will not.
    if (whispering) *whispering = false;
    if (slot < 0 || slot >= coop::players::kMaxPeers) return false;
    const Channel& ch = channels_[slot];
    const int64_t last = ch.lastFrameMs.load(std::memory_order_relaxed);
    if (last == 0 || NowMs() - last >= kTalkTimeoutMs) return false;
    if (whispering) *whispering = ch.whispering.load(std::memory_order_relaxed);
    return true;
}

// ---- jitter buffer (the SVC AudioPacketBuffer port; GT only) ----

void Playback::OnFrame(int slot, const coop::net::VoiceFramePayload& f) {
    if (slot < 0 || slot >= coop::players::kMaxPeers || !running_) return;
    Channel& ch = channels_[slot];

    if (jitterThreshold_ <= 0) {
        DeliverInOrder(ch, f);
        return;
    }
    // Stop marker: flag a full drain (deliver everything buffered, in order,
    // then the stop itself resets the stream state).
    if ((f.flags & coop::net::kVoiceFlagStop) != 0) ch.flushing = true;

    // In-order fast path: empty buffer + exactly-next (or first) seq.
    if (ch.bufCount == 0 && !ch.flushing &&
        (ch.lastSeq < 0 || static_cast<int64_t>(f.seq) == ch.lastSeq + 1)) {
        DeliverInOrder(ch, f);
        return;
    }
    // Insert sorted by seq (drop if full pre-pop fails -- shouldn't happen).
    if (ch.bufCount >= 16) {
        DeliverInOrder(ch, ch.buf[0]);  // pop oldest (accept the gap)
        std::memmove(&ch.buf[0], &ch.buf[1], sizeof(ch.buf[0]) * (--ch.bufCount));
    }
    int at = ch.bufCount;
    while (at > 0 && ch.buf[at - 1].seq > f.seq) --at;
    std::memmove(&ch.buf[at + 1], &ch.buf[at], sizeof(ch.buf[0]) * (ch.bufCount - at));
    ch.buf[at] = f;
    ++ch.bufCount;

    // Past the threshold (or flushing): deliver from the front.
    while (ch.bufCount > 0 && (ch.flushing || ch.bufCount > jitterThreshold_)) {
        coop::net::VoiceFramePayload head = ch.buf[0];
        std::memmove(&ch.buf[0], &ch.buf[1], sizeof(ch.buf[0]) * (--ch.bufCount));
        DeliverInOrder(ch, head);
    }
    if (ch.bufCount == 0) ch.flushing = false;
}

void Playback::TickDecode() {
    if (!running_) return;
    // The jitter buffer delivers eagerly from OnFrame; this drain covers the
    // threshold residue when a burst pauses (deliver aged frames so a short
    // utterance under the threshold still plays out).
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        Channel& ch = channels_[slot];
        if (ch.bufCount == 0) continue;
        const int64_t last = ch.lastFrameMs.load(std::memory_order_relaxed);
        // No new frame for ~100 ms but residue buffered: drain it.
        if (last != 0 && NowMs() - last > 100) {
            while (ch.bufCount > 0) {
                coop::net::VoiceFramePayload head = ch.buf[0];
                std::memmove(&ch.buf[0], &ch.buf[1], sizeof(ch.buf[0]) * (--ch.bufCount));
                DeliverInOrder(ch, head);
            }
            ch.flushing = false;
        }
    }
}

// ---- seq handling + decode (the SVC AudioChannel port; GT only) ----

void Playback::DeliverInOrder(Channel& ch, const coop::net::VoiceFramePayload& f) {
    if ((f.flags & coop::net::kVoiceFlagStop) != 0 || f.opusLen == 0) {
        // Burst end: reset stream state; the decoder starts the next burst fresh.
        ch.lastSeq = -1;
        if (ch.decoder)
            opus_decoder_ctl(static_cast<OpusDecoder*>(ch.decoder), OPUS_RESET_STATE);
        return;
    }
    if (!ch.decoder) {
        int err = 0;
        ch.decoder = opus_decoder_create(kSampleRate, 1, &err);
        if (!ch.decoder || err != OPUS_OK) {
            ch.decoder = nullptr;
            return;
        }
    }
    const int64_t seq = static_cast<int64_t>(f.seq);
    if (ch.lastSeq >= 0 && seq <= ch.lastSeq) {
        // A HUGE backward jump is the u32 wire seq wrapping (or a sender stream reset that lost its
        // stop marker) -- a new burst, not a late duplicate. Reset the stream state and decode this
        // frame fresh.
        if (ch.lastSeq - seq <= (int64_t{1} << 30)) return;  // dupe / too-late reorder
        ch.lastSeq = -1;
        opus_decoder_ctl(static_cast<OpusDecoder*>(ch.decoder), OPUS_RESET_STATE);
    }
    if (ch.lastSeq >= 0) {
        const int64_t gap = seq - (ch.lastSeq + 1);
        if (gap > 0 && gap <= kMaxPlcFrames) {
            // Opus PLC for the missing frames.
            int16_t plc[kFrameSamples];
            for (int64_t i = 0; i < gap; ++i) {
                const int n = opus_decode(static_cast<OpusDecoder*>(ch.decoder), nullptr, 0,
                                          plc, kFrameSamples, 0);
                if (n > 0) PushPcm(ch, plc, n);
            }
        } else if (gap > kMaxPlcFrames) {
            opus_decoder_ctl(static_cast<OpusDecoder*>(ch.decoder), OPUS_RESET_STATE);
        }
    }
    ch.lastSeq = seq;

    int16_t pcm[kFrameSamples];
    const int n = opus_decode(static_cast<OpusDecoder*>(ch.decoder), f.opus, f.opusLen,
                              pcm, kFrameSamples, 0);
    if (n <= 0) return;
    PushPcm(ch, pcm, n);

    ch.whispering.store((f.flags & coop::net::kVoiceFlagWhisper) != 0,
                        std::memory_order_relaxed);
    ch.lastFrameMs.store(NowMs(), std::memory_order_relaxed);
}

void Playback::PushPcm(Channel& ch, const int16_t* samples, int count) {
    const uint64_t write = ch.ringWrite.load(std::memory_order_relaxed);
    const uint64_t read = ch.ringRead.load(std::memory_order_acquire);
    const uint64_t free = Channel::kRingSamples - (write - read);
    if (static_cast<uint64_t>(count) > free) return;  // overrun: drop (jitter-buffer-grade loss)
    for (int i = 0; i < count; ++i)
        ch.ring[(write + i) % Channel::kRingSamples] = samples[i];
    ch.ringWrite.store(write + count, std::memory_order_release);
    // Prebuffer: start the mixer only once ~100 ms is queued (SVC's silence
    // pre-fill equivalent -- absorbs network jitter without mid-word gaps).
    if (!ch.primed.load(std::memory_order_relaxed) &&
        (write + count - read) >= static_cast<uint64_t>(prebufferFrames_ * kFrameSamples)) {
        ch.primed.store(true, std::memory_order_release);
    }
}

// ---- the mixer (miniaudio callback thread) ----

void Playback::MixOutput(float* out, uint32_t frameCount) {
    std::memset(out, 0, sizeof(float) * 2 * frameCount);
    const float lx = listenerX_.load(std::memory_order_relaxed);
    const float ly = listenerY_.load(std::memory_order_relaxed);
    const float lz = listenerZ_.load(std::memory_order_relaxed);
    const float yawRad = listenerYaw_.load(std::memory_order_relaxed) * kPi / 180.0f;
    const float master = masterVolume_.load(std::memory_order_relaxed);
    // The session's range (0 = unlimited), read once per block; never resolved here.
    const float range = distanceCm_.load(std::memory_order_relaxed);

    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        Channel& ch = channels_[slot];
        if (!ch.primed.load(std::memory_order_acquire)) continue;
        const uint64_t write = ch.ringWrite.load(std::memory_order_acquire);
        const uint64_t read = ch.ringRead.load(std::memory_order_relaxed);
        const uint64_t avail = write - read;
        if (avail == 0) {
            // Underrun: re-prebuffer before this channel speaks again.
            ch.primed.store(false, std::memory_order_release);
            continue;
        }

        // Spatial params once per callback block (~10 ms).
        float gainL = 1.0f, gainR = 1.0f;
        if (ch.posValid.load(std::memory_order_relaxed)) {
            const float dx = ch.posX.load(std::memory_order_relaxed) - lx;
            const float dy = ch.posY.load(std::memory_order_relaxed) - ly;
            const float dz = ch.posZ.load(std::memory_order_relaxed) - lz;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            // The range, four cases. range > 0, speaking: maxDist = range, the vertical fade
            // spans range * kVerticalFadeRatio. range > 0, whispering: maxDist = range / 2 and
            // the same fade (a whisper halves the reach, not the fade). range == 0 (unlimited),
            // speaking: no attenuation and no fade. range == 0, whispering: as under the row's
            // default range, so a whisper stays local on a server with global voice.
            const bool whispering = ch.whispering.load(std::memory_order_relaxed);
            float att = 1.0f;    // SVC AL_LINEAR_DISTANCE: 1 - dist / maxDist
            float vfade = 1.0f;  // SVC vertical fade: 1 - |dz| / fade
            if (range > 0.0f || whispering) {
                const float base = range > 0.0f ? range : DefaultRangeCm();
                const float maxDist = whispering ? base * 0.5f : base;
                att = 1.0f - dist / maxDist;
                if (att < 0) att = 0;
                if (att > 1) att = 1;
                vfade = 1.0f - std::fabs(dz) / (base * kVerticalFadeRatio);
                if (vfade < 0) vfade = 0;
            }
            float volL = 1.0f, volR = 1.0f;
            PanVolumes(dx, dy, dist, yawRad, volL, volR);
            const float g = att * vfade;
            gainL = g * volL;
            gainR = g * volR;
        }
        const float vol = master * ch.volume.load(std::memory_order_relaxed);
        gainL *= vol;
        gainR *= vol;

        const uint32_t take =
            avail < frameCount ? static_cast<uint32_t>(avail) : frameCount;
        for (uint32_t i = 0; i < take; ++i) {
            const float s =
                static_cast<float>(ch.ring[(read + i) % Channel::kRingSamples]) / 32768.0f;
            out[2 * i + 0] += s * gainL;
            out[2 * i + 1] += s * gainR;
        }
        ch.ringRead.store(read + take, std::memory_order_release);
        // (take < frameCount leaves silence for the tail -- the underrun
        // re-prime triggers on the next callback when avail hits 0.)
    }

    MixWorldSounds(out, frameCount);

    // Soft clip.
    const uint32_t n = 2 * frameCount;
    for (uint32_t i = 0; i < n; ++i) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

// ---- world sounds ----

void Playback::SetWorldListener(float x, float y, float z, float yawDeg) {
    worldX_.store(x, std::memory_order_relaxed);
    worldY_.store(y, std::memory_order_relaxed);
    worldZ_.store(z, std::memory_order_relaxed);
    worldYaw_.store(yawDeg, std::memory_order_relaxed);
}

// Handle = generation << 4 | emitter index; the generation changes per start, so a handle of an ended
// sound never stops its emitter's next occupant.
uint32_t Playback::PlayWorldSound(const int16_t* pcm, uint32_t frames, uint32_t rate, float pitch,
                                  float x, float y, float z, float gain, float fullCm,
                                  float silentCm) {
    if (!running_ || !pcm || frames < 2 || rate == 0) return 0;
    for (int i = 0; i < kMaxEmitters; ++i) {
        Emitter& e = emitters_[i];
        if (e.active.load(std::memory_order_acquire)) continue;
        e.pcm = pcm;
        e.frames = frames;
        e.step = static_cast<float>(rate) / static_cast<float>(kSampleRate) * pitch;
        e.x = x;
        e.y = y;
        e.z = z;
        e.gain = gain;
        e.fullCm = fullCm;
        e.silentCm = silentCm > fullCm ? silentCm : fullCm + 1.0f;
        e.cursor = 0.0;
        e.stop.store(false, std::memory_order_relaxed);
        uint32_t gen = (e.gen.load(std::memory_order_relaxed) + 1) & 0x0FFFFFFFu;
        if (gen == 0) gen = 1;
        e.gen.store(gen, std::memory_order_relaxed);
        e.active.store(true, std::memory_order_release);
        return (gen << 4) | static_cast<uint32_t>(i);
    }
    return 0;
}

void Playback::StopWorldSound(uint32_t handle) {
    if (handle == 0) return;
    const uint32_t idx = handle & 0xF;
    if (idx >= static_cast<uint32_t>(kMaxEmitters)) return;
    Emitter& e = emitters_[idx];
    if (e.gen.load(std::memory_order_relaxed) == (handle >> 4)) e.stop.store(true, std::memory_order_release);
}

bool Playback::WorldSoundPlaying(uint32_t handle) const {
    if (handle == 0) return false;
    const uint32_t idx = handle & 0xF;
    if (idx >= static_cast<uint32_t>(kMaxEmitters)) return false;
    const Emitter& e = emitters_[idx];
    return e.active.load(std::memory_order_acquire) && e.gen.load(std::memory_order_relaxed) == (handle >> 4) &&
           !e.stop.load(std::memory_order_relaxed);
}

// Callback thread: each live emitter, resampled linearly, attenuated by the straight distance to the world
// listener (full within fullCm, linear to silence at silentCm) and panned like a voice.
void Playback::MixWorldSounds(float* out, uint32_t frameCount) {
    const float lx = worldX_.load(std::memory_order_relaxed);
    const float ly = worldY_.load(std::memory_order_relaxed);
    const float lz = worldZ_.load(std::memory_order_relaxed);
    const float yawRad = worldYaw_.load(std::memory_order_relaxed) * kPi / 180.0f;
    for (Emitter& e : emitters_) {
        if (!e.active.load(std::memory_order_acquire)) continue;
        if (e.stop.load(std::memory_order_acquire)) {
            e.active.store(false, std::memory_order_release);
            continue;
        }
        const float dx = e.x - lx, dy = e.y - ly, dz = e.z - lz;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float att = (e.silentCm - dist) / (e.silentCm - e.fullCm);
        if (att > 1.0f) att = 1.0f;
        float volL = 1.0f, volR = 1.0f;
        PanVolumes(dx, dy, dist, yawRad, volL, volR);
        const float g = (att > 0.0f ? att : 0.0f) * e.gain / 32768.0f;
        const float gainL = g * volL, gainR = g * volR;
        const double last = static_cast<double>(e.frames - 1);
        bool ended = false;
        for (uint32_t i = 0; i < frameCount; ++i) {
            if (e.cursor >= last) {
                ended = true;
                break;
            }
            const uint32_t k = static_cast<uint32_t>(e.cursor);
            const float frac = static_cast<float>(e.cursor - k);
            const float s = static_cast<float>(e.pcm[k]) * (1.0f - frac) + static_cast<float>(e.pcm[k + 1]) * frac;
            out[2 * i + 0] += s * gainL;
            out[2 * i + 1] += s * gainR;
            e.cursor += e.step;
        }
        if (ended || e.cursor >= last) e.active.store(false, std::memory_order_release);
    }
}

std::vector<std::string> Playback::EnumerateDevices() {
    std::vector<std::string> names;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return names;
    ma_device_info* outs = nullptr;
    ma_uint32 outCount = 0;
    if (ma_context_get_devices(&ctx, &outs, &outCount, nullptr, nullptr) == MA_SUCCESS) {
        names.reserve(outCount);
        for (ma_uint32 i = 0; i < outCount; ++i) names.emplace_back(outs[i].name);
    }
    ma_context_uninit(&ctx);
    return names;
}

}  // namespace coop::voice
