// coop/world/sky_sync.cpp -- see coop/world/sky_sync.h. Host-authoritative night-sky sync
// (star-dome world rotation, moon phase, the sky eye), built on the same shape as coop/world/time_sync.cpp.

#include "coop/world/sky_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers
#include "coop/session/net_pump.h"          // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"  // the earthTp end-sound offset
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/skysphere.h"
#include "ue_wrap/core/types.h"  // FRotator

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::sky_sync {
namespace {

namespace SKY = ue_wrap::skysphere;
namespace sg  = ue_wrap::script_gate;

UE_FACT_FN(kRowSetEye, "sky_sync", L"newsky_C", L"setEye", Declared);
// newsky_C::tp, the earthTp event's whole body (a latent sequence: two 2D sounds, the black-hole sky,
// three emails). The host counts its runs into SkyState.tpSeq; a client refuses its own, since its
// emails would be local rows the host never authored, and plays the cue and shows the sky from SkyState.
UE_FACT_FN(kRowTp, "sky_sync", L"newsky_C", L"tp", Declared);
constexpr int kTpTag = 0x534b5450;  // 'SKTP'

std::atomic<coop::net::Session*> g_session{nullptr};
std::chrono::steady_clock::time_point g_lastBroadcast{};

// The dome spins very slowly (dt/32 deg/frame ~ 0.03 deg/s) and moonPhase is near-static, so a
// ~1 Hz push keeps both visually locked while staying quiet on the wire.
constexpr auto kPushInterval = std::chrono::milliseconds(1000);

// The eye: the day's noon roll runs on every peer and calls the sky's setEye, so a client refuses its own
// and shows the host's, which the lane carries. Game thread.
constexpr int kSetEyeTag = 0x534b4559;  // 'SKEY'
bool     g_gateAsked = false;            // the refusal gate was registered (or refused, said)
bool     g_applyingEye = false;          // the lane's own apply of the host's eye runs
uint64_t g_eyeRefused = 0;               // a client's own setEye calls refused this session
bool     g_saidEyeRefused = false;
uint64_t g_eyeApplyFailed = 0;           // the host's eye samples whose setEye did not run this session

// earthTp. HOST: the count of tp runs (SkyState.tpSeq) and a send owed at once. CLIENT: the last count
// seen (-1 before the first sample, which is a baseline: a joiner's seed plays no cue), and the end
// sound's due time (zero when none is owed). Game thread.
uint8_t   g_tpSeq = 0;
bool      g_tpSendNow = false;
int       g_seenTpSeq = -1;
std::chrono::steady_clock::time_point g_tpOutDue{};

// The lane's own apply, for the length of one setEye call: the gate lets exactly this call run.
struct ApplyingEye {
    ApplyingEye() { g_applyingEye = true; }
    ~ApplyingEye() { g_applyingEye = false; }
    ApplyingEye(const ApplyingEye&) = delete;
    ApplyingEye& operator=(const ApplyingEye&) = delete;
};

// CLIENT, before newsky's setEye: the host's sky owns the eye. The lane's apply runs, and so does a call
// in a world this client has not announced ready, its own load of the host's save.
sg::Verdict OnSetEyePre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() == coop::net::Role::Host) return sg::Verdict::Run;
    if (g_applyingEye || !call.object) return sg::Verdict::Run;
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    ++g_eyeRefused;
    if (!g_saidEyeRefused) {
        g_saidEyeRefused = true;
        UE_LOGI("sky_sync: a client refused its own setEye -- the host's sky owns the eye (first refusal; the "
                "rest are counted)");
    }
    return sg::Verdict::Cancel;
}

// Before newsky's tp. The host's run is counted and sent; a client's own is refused (the earthTp row is
// not replayed on a client, so this is its cheat menu or a stray call).
sg::Verdict OnTpPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) {
        ++g_tpSeq;
        g_tpSendNow = true;
        UE_LOGI("sky_sync: host earthTp (newsky %p tp) -- cue #%u sent on SkyState", call.object, g_tpSeq);
        return sg::Verdict::Run;
    }
    if (!call.object || !coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    UE_LOGI("sky_sync: a client refused its own newsky tp -- earthTp is the host's (its cue and sky come on "
            "SkyState)");
    return sg::Verdict::Cancel;
}

bool Finite(const coop::net::SkyStatePayload& p) {
    return std::isfinite(p.skyPitch) && std::isfinite(p.skyYaw) &&
           std::isfinite(p.skyRoll) && std::isfinite(p.moonPhase);
}

bool MakePayload(coop::net::SkyStatePayload& out) {
    ue_wrap::FRotator rot{};
    float moon = 0.f;
    if (!SKY::ReadSky(rot, moon)) return false;  // sky not streamed in yet
    out.skyPitch = rot.Pitch;
    out.skyYaw   = rot.Yaw;
    out.skyRoll  = rot.Roll;
    out.moonPhase = moon;
    bool eye = false;
    SKY::ReadEye(eye);  // unread, the sky shows no eye
    out.eye = eye ? 1 : 0;
    bool hole = false;
    SKY::ReadBlackHole(hole);  // unread, no black hole
    out.blackHole = hole ? 1 : 0;
    out.tpSeq = g_tpSeq;
    return Finite(out);  // never push NaN/inf onto the wire
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    SKY::EnsureResolved();  // retried via Tick until newsky_C loads
    if (!g_gateAsked) {
        g_gateAsked = true;
        if (!sg::WatchName(kRowSetEye, kSetEyeTag, &OnSetEyePre, nullptr))
            UE_LOGE("sky_sync: the script gate refused the watch on newsky_C.setEye -- a client's own noon roll "
                    "can still set its eye");
        if (!sg::WatchName(kRowTp, kTpTag, &OnTpPre, nullptr))
            UE_LOGE("sky_sync: the script gate refused the watch on newsky_C.tp -- earthTp neither crosses nor "
                    "is refused on a client");
    }
}

void OnReliable(const coop::net::SkyStatePayload& payload) {
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    if (!Finite(payload)) { UE_LOGW("sky_sync: dropping non-finite payload"); return; }
    SKY::ApplySky(ue_wrap::FRotator{payload.skyPitch, payload.skyYaw, payload.skyRoll}, payload.moonPhase);
    // The eye through the sky's own setEye, which swaps the moon's texture; the gate lets this call run.
    // A call that did not run is said once and counted; each next sample tries again.
    const bool want = payload.eye != 0;
    bool cur = false;
    if (SKY::ReadEye(cur) && cur != want) {
        bool ok = false;
        {
            ApplyingEye scope;
            ok = SKY::CallSetEye(want);
        }
        if (ok)
            UE_LOGI("sky_sync: applied the host's eye %d", want ? 1 : 0);
        else if (g_eyeApplyFailed++ == 0)
            UE_LOGW("sky_sync: the host's eye %d did not apply -- the sky's setEye did not run (first failure; the "
                    "rest are counted, and each next sample tries again)", want ? 1 : 0);
    }
    // earthTp: a changed count is the host's tp starting now, so the start sound plays and the end
    // sound is owed at the tp's own offset; the first sample is the baseline.
    if (g_seenTpSeq >= 0 && payload.tpSeq != static_cast<uint8_t>(g_seenTpSeq)) {
        const bool played = SKY::PlayEarthTpSound(true);
        g_tpOutDue = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(static_cast<int>(ue_wrap::profile::name::EarthTpOutDelaySec * 1000.f));
        UE_LOGI("sky_sync: host earthTp cue #%u -> start sound %s, end sound in %.0f s", payload.tpSeq,
                played ? "played" : "NOT played (sound not loaded)", ue_wrap::profile::name::EarthTpOutDelaySec);
    }
    g_seenTpSeq = payload.tpSeq;
    // The black-hole sky is state: asserted on every sample, so a joiner's seed and the permanent gamerule
    // hold, and the host's own flip times the client's.
    const bool wantHole = payload.blackHole != 0;
    bool curHole = false;
    if (SKY::ReadBlackHole(curHole) && curHole != wantHole) {
        const bool ok = SKY::ApplyBlackHole(wantHole);
        UE_LOGI("sky_sync: applied the host's black-hole sky %d%s", wantHole ? 1 : 0,
                ok ? "" : " -- skyVisibility did not run");
    }
    static int s_n = 0;
    if ((s_n++ % 10) == 0)  // ~every 10s -- confirm convergence, not spam
        UE_LOGI("sky_sync: applied host sky yaw=%.1f moonPhase=%.3f", payload.skyYaw, payload.moonPhase);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    coop::net::SkyStatePayload p{};
    if (!MakePayload(p)) return;
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::SkyState, &p, sizeof(p));
    UE_LOGI("sky_sync: connect-snapshot -- sent sky (yaw=%.1f moonPhase=%.3f eye=%d) to slot %d",
            p.skyYaw, p.moonPhase, p.eye, peerSlot);
}

void Tick() {
    sg::ResolvePendingNames();  // the eye gate's name, on the game thread
    if (!SKY::EnsureResolved()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const auto now = std::chrono::steady_clock::now();
    if (s->role() != coop::net::Role::Host) {
        // The client's owed earthTp end sound; otherwise the client applies on receipt.
        if (g_tpOutDue != std::chrono::steady_clock::time_point{} && now >= g_tpOutDue) {
            g_tpOutDue = {};
            const bool played = SKY::PlayEarthTpSound(false);
            UE_LOGI("sky_sync: earthTp end sound %s", played ? "played" : "NOT played (sound not loaded)");
        }
        return;
    }
    // A tp run is sent at once, not on the next second: its start sound is a cue.
    if (now - g_lastBroadcast < kPushInterval && !g_tpSendNow) return;
    // A joiner is connected for tens of seconds before it has a world, and SkyState is not on the
    // pre-world allowlist, so the fan-out drops it per slot and reports no delivery. That is a
    // vacuous success for this lane, not a failure: QueueConnectBroadcastForSlot seeds the sky the
    // moment the slot goes world-ready. Asking with nobody eligible is what turned a real warning
    // into 24 false alarms per join. Gated after the interval so it costs nothing per frame.
    if (!s->AnyWorldReadyPeer()) return;
    coop::net::SkyStatePayload p{};
    if (!MakePayload(p)) return;
    g_lastBroadcast = now;
    g_tpSendNow = false;
    if (s->SendReliable(coop::net::ReliableKind::SkyState, &p, sizeof(p))) {
        static int s_n = 0;
        if ((s_n++ % 10) == 0)  // ~every 10s
            UE_LOGI("sky_sync: host sky yaw=%.1f moonPhase=%.3f eye=%d", p.skyYaw, p.moonPhase, p.eye);
    } else {
        UE_LOGW("sky_sync: SendReliable(SkyState) failed");
    }
}

void OnDisconnect() {
    g_lastBroadcast = {};
    if (g_eyeRefused)
        UE_LOGI("sky_sync: session end -- a client refused %llu setEye call(s) of its own",
                static_cast<unsigned long long>(g_eyeRefused));
    if (g_eyeApplyFailed)
        UE_LOGW("sky_sync: session end -- %llu sample(s) of the host's eye did not apply",
                static_cast<unsigned long long>(g_eyeApplyFailed));
    g_eyeRefused = 0;
    g_saidEyeRefused = false;
    g_eyeApplyFailed = 0;
    g_tpSendNow = false;
    g_seenTpSeq = -1;   // the next session's first sample is a baseline again
    g_tpOutDue = {};
}

}  // namespace coop::sky_sync
