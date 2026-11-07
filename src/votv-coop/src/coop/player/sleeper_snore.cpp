// coop/player/sleeper_snore.cpp -- see coop/player/sleeper_snore.h.

#include "coop/player/sleeper_snore.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/player/sleeper_snore_sound.h"
#include "coop/player/sleepers.h"
#include "coop/voice/voice_chat.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>

namespace coop::sleeper_snore {
namespace {

namespace E = ue_wrap::engine;
using Clock = std::chrono::steady_clock;
using ue_wrap::FVector;

constexpr float kFullCm = 100.f;                         // full volume within about a metre
constexpr float kSilentCm = 400.f;                       // silent by four: heard only right beside the sleeper
constexpr float kGain = 0.25f;                           // the snore sits far under the configured volume
constexpr float kPitchBase = 0.96f;                      // a touch slower and lower: a softer breath
constexpr float kPitchSpread = 0.03f;                    // +-3 %
constexpr float kEarAboveHeadCm = 25.f;                  // the wall check aims just over the head, clear of the pillow
constexpr float kFirstDelayMinS = 0.3f, kFirstDelayMaxS = 2.5f;
constexpr float kIntervalMinS = 4.5f, kIntervalMaxS = 6.5f;   // the built-in breath is about 4 s long
constexpr auto kScanEvery = std::chrono::milliseconds(250);
constexpr auto kListenerEvery = std::chrono::milliseconds(50);
constexpr size_t kMaxSources = 64;

struct State {
    uint32_t handle = 0;
    Clock::time_point next{};
    FVector head{};
    bool said = false;
};
std::map<uint32_t, State> g_states;
Clock::time_point g_nextScan{};
Clock::time_point g_nextListener{};
bool g_listenerOk = false;
FVector g_listener{};
bool g_silentSaid = false;
std::mt19937 g_rng{static_cast<uint32_t>(Clock::now().time_since_epoch().count())};

float Rand(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

std::string Describe(uint32_t key) {
    char b[48];
    if (key & coop::sleepers::kSnoreOnlineKey)
        std::snprintf(b, sizeof(b), "online slot %u", key & ~coop::sleepers::kSnoreOnlineKey);
    else
        std::snprintf(b, sizeof(b), "offline sleeper %u", key);
    return b;
}

// The listener is the local camera (the voice's own is the actor's spot), with the actor's head height as a fallback.
void UpdateListener() {
    FVector cam = E::GetCameraLocation();
    float yaw = E::GetCameraRotation().Yaw;
    if (cam.X == 0.f && cam.Y == 0.f && cam.Z == 0.f) {
        void* const local = coop::players::Registry::Get().Local();
        ue_wrap::FRotator r{};
        if (!local || !E::TryGetActorLocation(local, cam)) {
            g_listenerOk = false;
            return;
        }
        cam.Z += 60.f;
        if (E::TryGetActorRotation(local, r)) yaw = r.Yaw;
    }
    g_listener = cam;
    g_listenerOk = true;
    coop::voice_chat::SetWorldListener(cam.X, cam.Y, cam.Z, yaw);
}

void Stop(State& st) {
    if (st.handle) coop::voice_chat::StopWorldSound(st.handle);
    st.handle = 0;
}

// A wall, floor or door between the listener and the sleeper's head (static and dynamic geometry, as the
// nameplate's check). A trace that cannot run counts as clear, so the snore is never lost to a missing hook.
bool Walled(const FVector& head) {
    void* const local = coop::players::Registry::Get().Local();
    if (!local) return false;
    const FVector ear{head.X, head.Y, head.Z + kEarAboveHeadCm};
    return ue_wrap::trace::LineBlockedStatDyn(local, g_listener, ear) == 1;
}

void Scan(Clock::time_point now) {
    if (!coop::config::ResolveFlag(::coop::config_registry::rows::sleepers_snore)) {
        StopAll();
        return;
    }
    if (!coop::voice_chat::WorldAudioReady()) {
        if (!g_silentSaid && coop::sleepers::Count() > 0) {
            g_silentSaid = true;
            UE_LOGI("sleeper_snore: the voice output is not running (voice off or no device) -- sleepers stay silent");
        }
        StopAll();
        return;
    }
    coop::sleepers::SnoreSource src[kMaxSources];
    const size_t n = coop::sleepers::SnoreSources(src, kMaxSources);
    // Sleepers that are gone: their snore is cut.
    for (auto it = g_states.begin(); it != g_states.end();) {
        bool listed = false;
        for (size_t i = 0; i < n; ++i)
            if (src[i].key == it->first) { listed = true; break; }
        if (listed) { ++it; continue; }
        Stop(it->second);
        UE_LOGI("sleeper_snore: %s stopped (woke, gone or removed)", Describe(it->first).c_str());
        it = g_states.erase(it);
    }
    if (n == 0) return;
    coop::sleeper_snore_sound::Prepare();
    const auto& clips = coop::sleeper_snore_sound::Clips();
    if (clips.empty()) return;
    UpdateListener();
    const float volume = kGain * coop::config::ResolveFloat(::coop::config_registry::rows::sleepers_snore_volume);
    for (size_t i = 0; i < n; ++i) {
        auto [it, fresh] = g_states.try_emplace(src[i].key);
        State& st = it->second;
        st.head = src[i].head;
        if (fresh) st.next = now + std::chrono::milliseconds(static_cast<int>(1000.f * Rand(kFirstDelayMinS, kFirstDelayMaxS)));
        if (st.handle && !coop::voice_chat::WorldSoundPlaying(st.handle)) st.handle = 0;
        if (!g_listenerOk) continue;
        const float dx = st.head.X - g_listener.X, dy = st.head.Y - g_listener.Y, dz = st.head.Z - g_listener.Z;
        const bool inRange = std::sqrt(dx * dx + dy * dy + dz * dz) < kSilentCm;
        if (st.handle) {
            if (!inRange || Walled(st.head)) Stop(st);   // walked off, or a wall came between: cut it
            continue;
        }
        if (now < st.next || !inRange) continue;   // out of earshot: nothing to start
        if (Walled(st.head)) continue;             // another room: not heard
        const auto& clip = clips[static_cast<size_t>(g_rng() % clips.size())];
        const float pitch = kPitchBase + Rand(-kPitchSpread, kPitchSpread);
        st.handle = coop::voice_chat::PlayWorldSound(clip.pcm.data(), static_cast<uint32_t>(clip.pcm.size()), clip.rate,
                                                     pitch, st.head.X, st.head.Y, st.head.Z, volume, kFullCm, kSilentCm);
        if (!st.handle) continue;   // the emitters are all busy: tried again next scan
        st.next = now + std::chrono::milliseconds(static_cast<int>(1000.f * Rand(kIntervalMinS, kIntervalMaxS)));
        if (!st.said) {
            st.said = true;
            UE_LOGI("sleeper_snore: %s snores at (%.0f, %.0f, %.0f), %.0f cm away (pitch %.2f, volume %.2f)",
                    Describe(src[i].key).c_str(), st.head.X, st.head.Y, st.head.Z,
                    std::sqrt(dx * dx + dy * dy + dz * dz), pitch, volume);
        }
    }
}

}  // namespace

void Tick() {
    const auto now = Clock::now();
    if (!g_states.empty() && now >= g_nextListener) {
        g_nextListener = now + kListenerEvery;
        UpdateListener();
    }
    if (now < g_nextScan) return;
    g_nextScan = now + kScanEvery;
    Scan(now);
}

void StopAll() {
    for (auto& [key, st] : g_states) Stop(st);
    g_states.clear();
}

}  // namespace coop::sleeper_snore
