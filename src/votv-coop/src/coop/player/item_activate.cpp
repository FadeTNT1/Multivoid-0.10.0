// coop/player/item_activate.cpp -- flashlight sync (the item-activation lane). See
// coop/player/item_activate.h. The sender is a per-tick poll of the local player's beam (TickConnect),
// which sends an ItemActivate whenever what a peer would see changes; ApplyToPuppet is the
// receiver, called from event_feed's drain with the puppet.

#include "coop/player/item_activate.h"

#include "coop/element/element.h"
#include "coop/player/flashlight_click_sound.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/config/config.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>

namespace coop::item_activate {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

// The beam changes on more routes than a press: the F input, the hold-F mode change, an empty
// battery (the battery ticker's turnOffFlashlight calls 'Flashlight Update' from its own bytecode),
// an unequip (updateEquipment clears equipped_flashlight and the next update turns the beam off),
// and the equipped item's own brightness, reach and colour (updateFlashlightProperties). Most of
// those are Blueprint-to-Blueprint calls a ProcessEvent observer never sees, so the sender reads the
// state itself every tick and sends on a change of what a peer would see: a few field reads.

// The last-sent signature (state, mode and the beam's shape): the hold-F mode change keeps state
// on but changes the mode and cones, so a state-only dedupe would drop it. kNoSendYet
// means never sent. One atomic, so the load cannot see a torn flag-plus-signature (a split
// pair let DebugForceToggle on a worker thread update the flag before the signature and an
// poll dedupe a fresh packet).
inline constexpr uint64_t kNoSendYet = 0xFFFFFFFFFFFFFFFFULL;
std::atomic<uint64_t> g_lastSentSig{kNoSendYet};

// 0.10.0 turns the beam on and off with light_R's visibility alone ('Flashlight Update':
// SetVisibility(flashlight)); its Intensity is the equipped item's flashlight_brightness, set by the
// battery ticker's updateFlashlightProperties and unchanged by a toggle (0.20 for the stock
// flashlight, on and off alike, in every send of the host log). The receiver mirrors both.

// The hash of prop_equipment_flashlight_C, the packet's itemClassHash; the _a and _b variants
// both toggle the puppet's light_R and share it.
uint32_t g_flashlightClassHash = 0;

// Atomic: the sender's read races the setter on another thread.
std::atomic<coop::net::Session*> g_session{nullptr};

// The receiver's per-slot beam: what the peer last said (`want`) and what is on the engine now (the
// puppet and its light_R the beam was last applied to). The wanted state outlives the puppet: a puppet is
// destroyed and spawned again on a pocket trip, a level change, a GC'd body or a skin rebuild, and the
// packet is edge-triggered, so the old code (a one-shot pending slot, cleared on apply) left the new
// puppet dark until the owner pressed F again. TickConnect re-seeds any new puppet from `want`, repairs a
// light whose state drifted from it, and runs the per-tick conditioning the puppet's parked actor tick
// skips. On the host `want` is also the late-joiner replay cache (slot 0, the host's own beam, is never
// held here; QueueConnectBroadcastForSlot sends it). Game thread.
struct PeerBeam {
    bool     wantValid = false;
    coop::net::ItemActivatePayload want{};
    uint64_t wantSig = 0;
    void*    actor = nullptr;     // the puppet the beam is on; null = not applied to the current one
    void*    light = nullptr;     // that puppet's light_R
    uint64_t appliedSig = 0;
    // The conditioning mainPlayer_C's tick runs on its own light_R (see Condition).
    bool     shadowsKnown = false;
    bool     shadows = false;
    float    retractX = 0.f;      // light_R.RelativeLocation.X: -50 cm (pulled back) .. 0
    float    traceFrac = 1.f;     // the last look trace's hit distance over its length; 1 = clear
    uint64_t nextTraceMs = 0;
    uint64_t nextCheckMs = 0;
    uint64_t lastCondMs = 0;
    uint32_t heals = 0;
    bool     everBound = false;   // the beam reached some puppet of this occupancy at least once
    void*    darkened = nullptr;  // the last puppet made dark while no beam was known (see TickConnect)
};
PeerBeam g_beam[coop::players::kMaxPeers];

// The conditioning constants, from mainPlayer_C's ExecuteUbergraph (the 0.10.0 capture): the light's
// relative location is VInterpTo'd at speed 10 toward VLerp((-50,0,0), (0,0,0), frac^3), where frac is the
// look trace's hit distance over its length (1 when nothing is hit), and SetCastShadows(!bBlockingHit).
inline constexpr float    kRetractMaxCm         = 50.f;
inline constexpr float    kRetractInterpSpeed   = 10.f;
inline constexpr float    kArmLengthFallbackCm  = 200.f;   // only when mainPlayer_C.armLength does not read
inline constexpr uint64_t kTraceIntervalMs      = 100;     // the trace runs at 10 Hz per lit puppet; the interp each tick
inline constexpr uint64_t kCheckIntervalMs      = 500;     // the drift check per known slot

// The state last logged by the sender: a change of on and off is an event, a change of the beam's
// shape alone is logged only under the flashlight_log flag.
uint8_t g_lastLoggedState = 0xFF;

// Echo suppression around the receiver's apply, so a UFunction invoked on the puppet cannot
// bounce back as a wire packet. Atomic, as the session pointer.
std::atomic<bool> g_echoSuppress{false};

bool ProbeLogEnabled() {
    // Read once per process: the poll is hot.
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::flashlight_log);
    return s_enabled;
}

// The local player's flashlight state and cone shape as a payload; direct memory reads on the
// calling thread (the poll on the game thread, DebugForceToggle inside its posted lambda).
// False if light_R is dead.
// The beam profile `ies` is, as an index into the profiles 'Flashlight Update' selects from;
// kNoIesProfile for none or one this table does not name. The objects are looked up once each.
uint8_t IesProfileIndex(void* ies) {
    namespace N = ue_wrap::profile::name;
    static void* s_profiles[std::size(N::FlashlightIesProfiles)] = {};
    if (!ies) return coop::net::kNoIesProfile;
    for (size_t i = 0; i < std::size(N::FlashlightIesProfiles); ++i) {
        if (!s_profiles[i] || !R::IsLive(s_profiles[i]))
            s_profiles[i] = R::FindObject(N::FlashlightIesProfiles[i], N::TextureLightProfileClass);
        if (s_profiles[i] == ies) return static_cast<uint8_t>(i);
    }
    return coop::net::kNoIesProfile;
}

void* IesProfileObject(uint8_t index) {
    namespace N = ue_wrap::profile::name;
    if (index >= std::size(N::FlashlightIesProfiles)) return nullptr;
    return R::FindObject(N::FlashlightIesProfiles[index], N::TextureLightProfileClass);
}

bool BuildPayloadFromLocal(void* mp, coop::net::ItemActivatePayload& out, coop::net::Session* session) {
    if (!mp || !session) return false;
    void* light_R = ue_wrap::engine::GetMainPlayerLightR(mp);
    if (!light_R) return false;

    ue_wrap::engine::MainPlayerFlashlightState fl{};
    if (!ue_wrap::engine::ReadMainPlayerFlashlightState(mp, fl)) return false;
    ue_wrap::engine::FlashlightSnapshot snap{};
    if (!ue_wrap::engine::ReadFlashlightSnapshot(light_R, snap)) return false;

    out = {};
    out.itemClassHash   = g_flashlightClassHash;
    // The sender's own Player element id; 0 before it is allocated (the boot window), and the
    // receiver then falls back to the sender slot. Ids are unique across the host and peer ranges.
    {
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        out.senderElementId =
            (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
    }
    // On is the beam the game shows: the bool, and a flashlight in an equipment slot (an unequip
    // leaves the bool set until the next update clears it).
    out.state           = (fl.flashlight && fl.equipped) ? 1 : 0;
    out.flags           = 0;  // no actor key
    out.mode            = fl.mode;
    out.actorKeyHash    = 0;
    out.intensity       = snap.intensity;
    out.outerConeAngle  = snap.outerConeAngle;
    out.innerConeAngle  = snap.innerConeAngle;
    out.attenuationRadius = snap.attenuationRadius;
    out.colorB = snap.color.B;
    out.colorG = snap.color.G;
    out.colorR = snap.color.R;
    out.colorA = snap.color.A;
    out.iesProfile = IesProfileIndex(snap.iesTexture);
    return true;
}

// FNV1a over the fields a peer sees, so the poll sends a change once.
uint64_t SignaturePayload(const coop::net::ItemActivatePayload& p) {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](uint64_t v) { h ^= v; h *= 0x100000001b3ULL; };
    mix(p.state);
    mix(p.mode);
    mix(reinterpret_cast<const uint32_t&>(p.intensity));
    mix(reinterpret_cast<const uint32_t&>(p.outerConeAngle));
    mix(reinterpret_cast<const uint32_t&>(p.innerConeAngle));
    mix(reinterpret_cast<const uint32_t&>(p.attenuationRadius));
    mix((uint32_t(p.colorB) << 24) | (uint32_t(p.colorG) << 16) | (uint32_t(p.colorR) << 8) | p.colorA);
    mix(p.iesProfile);
    return h;
}

// The sender: the local player's beam as a peer would see it, sent when it differs from the last
// one sent. Game thread, every tick.
void PollLocal() {
    if (g_echoSuppress.load(std::memory_order_acquire)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    // ItemActivate is not a pre-world kind (session_lanes.h): a host whose only peers are still loading cannot
    // send it, and the old retry-and-warn every tick logged 2103 lines in 40 s of a joiner's load. Nobody to tell
    // is not a failure; the connect broadcast and the first change after a peer is ready carry the state.
    if (s->role() == coop::net::Role::Host && !s->AnyWorldReadyPeer()) return;
    void* mp = coop::players::Registry::Get().Local();
    if (!mp) return;
    coop::net::ItemActivatePayload p{};
    if (!BuildPayloadFromLocal(mp, p, s)) return;
    // What the owner's own light shows against what the wire says: the payload's on is the flashlight bool
    // and the equip, the light's is bVisible ('Flashlight Update' sets both together). A disagreement that
    // holds is a route this lane does not read; said on each change, never per tick.
    {
        static int s_lastMismatch = -1;
        E::FlashlightSnapshot own{};
        if (void* ownLight = E::GetMainPlayerLightR(mp); ownLight && E::ReadFlashlightSnapshot(ownLight, own)) {
            const int mismatch = (own.visible != (p.state != 0)) ? 1 : 0;
            if (mismatch != s_lastMismatch) {
                if (mismatch || s_lastMismatch == 1) {
                    UE_LOGI("flashlight: own light visible=%d hidden=%d vs sent state=%d (%s)",
                            own.visible ? 1 : 0, own.hiddenInGame ? 1 : 0, p.state,
                            mismatch ? "disagree" : "agree again");
                }
                s_lastMismatch = mismatch;
            }
        }
    }
    const uint64_t sig = SignaturePayload(p);
    // A hash equal to kNoSendYet is shifted by one so the sentinel stays unambiguous.
    const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
    if (g_lastSentSig.load(std::memory_order_acquire) == storeSig) return;
    static bool s_failSaid = false;   // once per run of failures, not once per retry
    if (!s->SendReliable(coop::net::ReliableKind::ItemActivate, &p, sizeof(p))) {
        if (!s_failSaid) {
            s_failSaid = true;
            UE_LOGW("flashlight: SendReliable failed (channel busy or not connected) -- retried each tick, said once");
        }
        return;
    }
    s_failSaid = false;
    g_lastSentSig.store(storeSig, std::memory_order_release);
    if (ProbeLogEnabled() || g_lastLoggedState != p.state) {
        g_lastLoggedState = p.state;
        UE_LOGI("flashlight: sent state=%d mode=%u Intensity=%.2f outerCone=%.1f innerCone=%.1f "
                "radius=%.0f color=%02X%02X%02X ies=%u (senderElementId=0x%08x)",
                p.state, p.mode, p.intensity, p.outerConeAngle, p.innerConeAngle,
                p.attenuationRadius, p.colorR, p.colorG, p.colorB, p.iesProfile, p.senderElementId);
    }
}

}  // namespace

bool DebugForceToggle(void* mp) {
    if (!mp) return false;

    // The field flip runs on the game thread (UObject memory), posted and awaited from this worker
    // thread; the autotest calls this from its worker, never inside a posted lambda. The local
    // light_R's visibility is driven too, so the sender's own light toggles: the BP path is gated
    // by input state reflection cannot reach, and the bool plus SetVisibility is the pair
    // 'Flashlight Update' writes.
    auto done = std::make_shared<std::atomic<int>>(0);
    auto newStateOut = std::make_shared<std::atomic<bool>>(false);
    GT::Post([mp, done, newStateOut] {
        // Read and flip through the wrappers.
        ue_wrap::engine::MainPlayerFlashlightState cur{};
        if (!ue_wrap::engine::ReadMainPlayerFlashlightState(mp, cur)) {
            done->store(1, std::memory_order_release);
            return;
        }
        const bool newState = !cur.flashlight;
        ue_wrap::engine::WriteMainPlayerFlashlight(mp, newState);

        // The local light_R's visibility follows the new state, through the same setter the puppet
        // receiver uses.
        if (void* light_R = ue_wrap::engine::GetMainPlayerLightR(mp)) {
            ue_wrap::engine::SetSceneComponentVisibility(light_R, newState, false);
        }
        newStateOut->store(newState, std::memory_order_release);
        UE_LOGI("flashlight: DebugForceToggle wrote flashlight=%d (and light_R visibility) on %p (LAN test path)",
                newState ? 1 : 0, mp);
        done->store(1, std::memory_order_release);
    });
    while (done->load(std::memory_order_acquire) == 0) ::Sleep(1);
    const bool newState = newStateOut->load(std::memory_order_acquire);

    // SendReliable queues inside the channel and fails only on a malformed packet.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) {
        UE_LOGW("flashlight: DebugForceToggle session not connected -- skipping wire send");
        return newState;
    }

    // The cone shape is whatever the local light_R carries: this bypasses the BP, so the cones do
    // not reflect a real mode change, which the autotest does not exercise.
    coop::net::ItemActivatePayload p{};
    if (!BuildPayloadFromLocal(mp, p, s)) {
        UE_LOGW("flashlight: DebugForceToggle BuildPayloadFromLocal failed (light_R dead?)");
        return newState;
    }

    if (s->SendReliable(coop::net::ReliableKind::ItemActivate, &p, sizeof(p))) {
        const uint64_t sig = SignaturePayload(p);
        const uint64_t storeSig = (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
        g_lastSentSig.store(storeSig, std::memory_order_release);
        UE_LOGI("flashlight: DebugForceToggle sent state=%d mode=%u Intensity=%.2f outerCone=%.1f "
                "(senderElementId=0x%08x)",
                p.state, p.mode, p.intensity, p.outerConeAngle, p.senderElementId);
    } else {
        UE_LOGW("flashlight: DebugForceToggle SendReliable failed (state=%d)", p.state);
    }
    return newState;
}

uint32_t HashClassName(const wchar_t* s) {
    // FNV-1a 32-bit over the UTF-16 bytes; the same encoding on every peer.
    uint32_t h = 0x811c9dc5u;
    if (!s) return h;
    for (; *s; ++s) {
        const wchar_t w = *s;
        h ^= static_cast<uint8_t>(w & 0xFF);
        h *= 0x01000193u;
        h ^= static_cast<uint8_t>((w >> 8) & 0xFF);
        h *= 0x01000193u;
    }
    return h;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_flashlightClassHash == 0)
        g_flashlightClassHash = HashClassName(ue_wrap::profile::name::FlashlightEquipmentClass);
}


namespace {

uint64_t NowMs() { return ::GetTickCount64(); }

uint64_t StoredSig(const coop::net::ItemActivatePayload& p) {
    const uint64_t sig = SignaturePayload(p);
    return (sig == kNoSendYet) ? (kNoSendYet - 1) : sig;
}

// Forget what is on the engine for a slot (its puppet is gone or about to be replaced); the wanted beam
// stays, so the next puppet is seeded from it.
void UnbindBeam(PeerBeam& b) {
    b.actor = nullptr;
    b.light = nullptr;
    b.appliedSig = 0;
    b.shadowsKnown = false;
    b.traceFrac = 1.f;
    b.nextTraceMs = 0;
    b.nextCheckMs = 0;
    b.lastCondMs = 0;
}

// Put `b.want` on `puppet`'s light_R. `why` names the route for the log; `audible` false makes a re-seed
// or a repair silent (only a press clicks). The puppet's light_R is driven through the engine's own
// setters only: the puppet's flashlight bool is not written (it would run the updateFlashlight ubergraph,
// an "Invalid prop" emitter), no component is added, and flashlightStateChanged is not broadcast.
// SetVisibility is what 'Flashlight Update' itself toggles: its OnVisibilityChanged marks the render
// state dirty, which builds the scene proxy; the intensity stays the item's brightness either way.
bool ApplyBeam(uint8_t slot, void* puppet, const char* why, bool audible) {
    PeerBeam& b = g_beam[slot];
    const coop::net::ItemActivatePayload& p = b.want;
    void* light = E::GetMainPlayerLightR(puppet);
    if (!light) {
        static uint32_t s_missing = 0;
        if (s_missing++ < 8) {
            UE_LOGW("flashlight: slot %u puppet %p has no live light_R -- beam not applied (%s)",
                    static_cast<unsigned>(slot), puppet, why);
        }
        return false;
    }
    const bool newState = (p.state != 0);
    const bool newBinding = (b.actor != puppet || b.light != light);

    E::FlashlightSnapshot before{};
    const bool haveBefore = E::ReadFlashlightSnapshot(light, before);

    g_echoSuppress.store(true, std::memory_order_release);

    bool cycleVisibility = false;
    if (newBinding) {
        UnbindBeam(b);
        // The light's own resting offset, so the conditioning eases from where it is.
        b.retractX = E::GetComponentRelativeLocation(light).X;
        // A distance cut on the light would drop a remote beam seen from across the yard (the owner's beam
        // always sits at their own camera, so the game never meets it). No setter exists; the raw write
        // reaches the proxy on its next recreation, forced below by a visibility cycle when already shown.
        const float maxDraw = E::ReadLightMaxDrawDistance(light);
        if (maxDraw > 0.f && E::WriteLightMaxDrawDistance(light, 0.f)) {
            cycleVisibility = haveBefore && before.visible;
            UE_LOGI("flashlight: slot %u puppet light MaxDrawDistance %.0f -> 0 (no distance cut)",
                    static_cast<unsigned>(slot), maxDraw);
        }
        UE_LOGI("flashlight: slot %u bound to puppet=%p light_R=%p (%s; relX=%.1f visible=%d hidden=%d "
                "maxDraw=%.0f)",
                static_cast<unsigned>(slot), puppet, light, why, b.retractX,
                haveBefore ? static_cast<int>(before.visible) : -1,
                haveBefore ? static_cast<int>(before.hiddenInGame) : -1, maxDraw);
    }

    // A light hidden in game never renders, whatever bVisible says; nothing on the puppet should set it,
    // so it is undone and said.
    if (newState && haveBefore && before.hiddenInGame) {
        E::SetComponentVisible(light, true, false);
        UE_LOGW("flashlight: slot %u puppet light_R was hidden in game -- unhidden", static_cast<unsigned>(slot));
    }
    if (cycleVisibility) E::SetSceneComponentVisibility(light, false, false);

    if (!E::SetSceneComponentVisibility(light, newState, false)) {
        UE_LOGW("flashlight: SetSceneComponentVisibility failed (UFunction unresolved or light_R dead)");
    }
    // The sender's exact brightness while on (the item's flashlight_brightness, 0.2 for the stock one);
    // off is 0 on top of the hidden light.
    const float targetIntensity = newState ? p.intensity : 0.f;
    if (!E::SetLightIntensity(light, targetIntensity)) {
        UE_LOGW("flashlight: SetLightIntensity failed (UFunction unresolved or light_R dead)");
    }
    // The beam's shape: the cones (the hold-F mode), the reach, the colour and the profile. Only when on.
    if (newState) {
        if (p.outerConeAngle > 0.f) E::SetSpotLightOuterConeAngle(light, p.outerConeAngle);
        if (p.innerConeAngle >= 0.f) E::SetSpotLightInnerConeAngle(light, p.innerConeAngle);
        if (p.attenuationRadius > 0.f) E::SetLightAttenuationRadius(light, p.attenuationRadius);
        E::SetLightColor(light, ue_wrap::FColor{p.colorB, p.colorG, p.colorR, p.colorA});
        if (p.iesProfile != coop::net::kNoIesProfile) {
            if (void* ies = IesProfileObject(p.iesProfile)) E::SetLightIESTexture(light, ies);
        }
    }

    g_echoSuppress.store(false, std::memory_order_release);

    b.actor = puppet;
    b.light = light;
    b.appliedSig = b.wantSig;
    b.everBound = true;
    b.nextCheckMs = NowMs() + kCheckIntervalMs;

    UE_LOGI("flashlight: applied slot=%u (%s) puppet=%p state=%d Intensity=%.2f outerCone=%.1f "
            "innerCone=%.1f radius=%.0f color=%02X%02X%02X ies=%u mode=%u (senderElementId=0x%08x)",
            static_cast<unsigned>(slot), why, puppet, newState ? 1 : 0, targetIntensity,
            p.outerConeAngle, p.innerConeAngle, p.attenuationRadius, p.colorR, p.colorG, p.colorB,
            p.iesProfile, p.mode, p.senderElementId);

    // The positional click on a press; a re-seed or a repair only records the state.
    coop::flashlight_click_sound::PlayIfStateChanged(puppet, slot, newState, audible);
    return true;
}

// The per-tick conditioning mainPlayer_C's own tick gives light_R and a puppet's parked tick never does:
// its look trace (`arm`: armLength along the view) pulls the light back up to 50 cm as the trace hits
// something nearer (VLerp((-50,0,0), 0, frac^3), VInterpTo at speed 10) and turns its shadows off while
// it hits. Without it a remote beam keeps its spawn-time shadows and offset against a wall, a door or a
// crate, where the owner's own beam is pulled back and unshadowed. The trace runs at 10 Hz from the
// puppet's camera along the streamed aim against WorldStatic and WorldDynamic (no pawns, so the puppet
// never hits itself); the offset is eased every tick. Only while lit and drawn upright.
void Condition(uint8_t slot, PeerBeam& b, coop::RemotePlayer& rp, uint64_t now) {
    void* const puppet = b.actor;
    void* const light = b.light;
    const float dt = b.lastCondMs ? std::min(0.1f, static_cast<float>(now - b.lastCondMs) / 1000.f) : 0.f;
    b.lastCondMs = now;

    if (now >= b.nextTraceMs) {
        b.nextTraceMs = now + kTraceIntervalMs;
        ue_wrap::FVector eye{};
        if (rp.TryGetEyePosition(eye)) {
            const ue_wrap::FVector dir = rp.GetSyncedAimDirection();
            float len = E::ReadMainPlayerArmLength(puppet);
            if (!(len > 1.f) || !std::isfinite(len)) len = kArmLengthFallbackCm;
            const ue_wrap::FVector end{eye.X + dir.X * len, eye.Y + dir.Y * len, eye.Z + dir.Z * len};
            ue_wrap::trace::Hit hit{};
            if (ue_wrap::trace::LineHitStatDyn(puppet, eye, end, &hit)) {
                float frac = 1.f;
                if (hit.blocked) {
                    const float dx = hit.point.X - eye.X, dy = hit.point.Y - eye.Y, dz = hit.point.Z - eye.Z;
                    frac = std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz) / len, 0.f, 1.f);
                }
                b.traceFrac = frac;
                const bool wantShadows = !hit.blocked;
                if (!b.shadowsKnown || b.shadows != wantShadows) {
                    E::SetLightCastShadows(light, wantShadows);
                    if (ProbeLogEnabled() || !b.shadowsKnown) {
                        UE_LOGI("flashlight: slot %u beam shadows=%d (look trace %s at %.0f%% of %.0f cm)",
                                static_cast<unsigned>(slot), wantShadows ? 1 : 0,
                                hit.blocked ? "hit" : "clear", frac * 100.f, len);
                    }
                    b.shadows = wantShadows;
                    b.shadowsKnown = true;
                }
            }
        }
    }

    const float a = b.traceFrac * b.traceFrac * b.traceFrac;
    const float target = -kRetractMaxCm * (1.f - a);
    const float step = std::clamp(dt * kRetractInterpSpeed, 0.f, 1.f);
    const float next = (std::fabs(target - b.retractX) < 0.05f) ? target : b.retractX + (target - b.retractX) * step;
    if (std::fabs(next - b.retractX) > 0.01f) {
        E::SetComponentRelativeLocation(light, ue_wrap::FVector{next, 0.f, 0.f});
        b.retractX = next;
    }
}

// Whether the light on the engine still shows what the slot wants. Something other than this lane
// (a Blueprint on the puppet, a replaced component) changing it is repaired by the caller.
bool BeamDrifted(const PeerBeam& b, void* puppet, const char** what) {
    void* light = E::GetMainPlayerLightR(puppet);
    if (light != b.light) { *what = "light_R replaced"; return true; }
    E::FlashlightSnapshot snap{};
    if (!E::ReadFlashlightSnapshot(light, snap)) return false;
    const bool on = (b.want.state != 0);
    if (snap.visible != on) { *what = on ? "light hidden" : "light shown"; return true; }
    if (on && snap.hiddenInGame) { *what = "hidden in game"; return true; }
    if (on && std::fabs(snap.intensity - b.want.intensity) > 0.01f) { *what = "intensity changed"; return true; }
    return false;
}

}  // namespace

void ApplyToPuppet(void* puppetActor, const coop::net::ItemActivatePayload& payload,
                   uint8_t senderPeerSlot) {
    if (!puppetActor || !R::IsLive(puppetActor)) {
        UE_LOGW("flashlight: ApplyToPuppet called with invalid puppet=%p", puppetActor);
        return;
    }
    if (payload.itemClassHash != g_flashlightClassHash) {
        UE_LOGW("flashlight: ApplyToPuppet classHash mismatch (got 0x%08X, "
                "expected 0x%08X = flashlight) -- dropping", payload.itemClassHash,
                g_flashlightClassHash);
        return;
    }
    if (senderPeerSlot >= coop::players::kMaxPeers) return;
    PeerBeam& b = g_beam[senderPeerSlot];
    b.want = payload;
    b.wantValid = true;
    b.wantSig = StoredSig(payload);
    ApplyBeam(senderPeerSlot, puppetActor, "direct", /*audible=*/true);
}

void ApplyToPuppetOrDefer(uint8_t senderPeerSlot, void* puppetActor,
                          const coop::net::ItemActivatePayload& p) {
    if (senderPeerSlot >= coop::players::kMaxPeers) {
        UE_LOGW("flashlight: ApplyToPuppetOrDefer senderPeerSlot=%u out of range -- dropping",
                static_cast<unsigned>(senderPeerSlot));
        return;
    }
    if (p.itemClassHash != g_flashlightClassHash) {
        UE_LOGW("flashlight: slot %u classHash mismatch (got 0x%08X, expected 0x%08X = flashlight) -- dropping",
                static_cast<unsigned>(senderPeerSlot), p.itemClassHash, g_flashlightClassHash);
        return;
    }
    // The wanted beam is kept for the slot whatever happens to the puppet: the host replays it to a later
    // joiner, and every peer seeds a new puppet from it.
    PeerBeam& b = g_beam[senderPeerSlot];
    b.want = p;
    b.wantValid = true;
    b.wantSig = StoredSig(p);
    if (puppetActor && R::IsLive(puppetActor)) {
        // A repeat of what the puppet already shows (a replay, a re-send) changes nothing on the engine.
        if (b.actor == puppetActor && b.appliedSig == b.wantSig &&
            E::GetMainPlayerLightR(puppetActor) == b.light) {
            if (ProbeLogEnabled()) {
                UE_LOGI("flashlight: slot %u repeat of the applied beam (state=%d) -- nothing to do",
                        static_cast<unsigned>(senderPeerSlot), p.state);
            }
            return;
        }
        ApplyBeam(senderPeerSlot, puppetActor, "packet", /*audible=*/true);
        return;
    }
    // No puppet yet (the reliable packet beat the first pose): TickConnect seeds the puppet once it exists.
    UE_LOGI("flashlight: slot %u puppet not ready (senderElementId=0x%08x) -- beam held (state=%d Intensity=%.2f)",
            static_cast<unsigned>(senderPeerSlot), p.senderElementId, p.state, p.intensity);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) {
        UE_LOGW("flashlight: QueueConnectBroadcastForSlot called with no session");
        return;
    }
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) {
        UE_LOGW("flashlight: QueueConnectBroadcastForSlot peerSlot=%d out of range", peerSlot);
        return;
    }
    void* mp = coop::players::Registry::Get().Local();
    if (!mp) {
        // No local player yet (the menu): nothing to broadcast; the player spawns in the loading
        // splash before the session connects, so this is rare.
        UE_LOGI("flashlight: QueueConnectBroadcastForSlot(slot=%d) no local mp yet -- no broadcast",
                peerSlot);
        return;
    }
    coop::net::ItemActivatePayload p{};
    if (!BuildPayloadFromLocal(mp, p, s)) {
        UE_LOGW("flashlight: QueueConnectBroadcastForSlot(slot=%d) BuildPayloadFromLocal failed",
                peerSlot);
        return;
    }
    // Off is not sent: the puppet spawns off, and the poll ships a later toggle.
    if (p.state == 0) {
        UE_LOGI("flashlight: QueueConnectBroadcastForSlot(slot=%d) local state is OFF -- "
                "skipping (puppet default is OFF; no replay needed)", peerSlot);
        return;
    }
    // To the one slot: the connected peers got the state on the toggle.
    s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::ItemActivate, &p, sizeof(p));
    g_lastSentSig.store(StoredSig(p), std::memory_order_release);
    UE_LOGI("flashlight: connect-broadcast slot=%d sent state=%d Intensity=%.2f "
            "outerCone=%.1f mode=%u senderElementId=0x%08x",
            peerSlot, p.state, p.intensity, p.outerConeAngle, p.mode, p.senderElementId);
}

void ReplayPeerStatesToSlot(int newSlot) {
    // Every other client's wanted beam, replayed to a new client so it converges; ItemActivate is
    // edge-triggered, and the joiner cannot learn these otherwise. Host only.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (newSlot < 1 || newSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    for (int peer = 1; peer < static_cast<int>(coop::players::kMaxPeers); ++peer) {
        if (peer == newSlot) continue;
        const PeerBeam& b = g_beam[peer];
        if (!b.wantValid) continue;
        const coop::net::ItemActivatePayload& p = b.want;
        // Off is skipped: the joiner's puppet for that peer spawns off.
        if (p.state == 0) continue;
        // The sender slot is the origin peer, so the joiner routes the action to that peer's puppet
        // and the eid-range trust check sees a peer-range id from a peer slot.
        s->SendReliableToSlot(newSlot, coop::net::ReliableKind::ItemActivate,
                              &p, sizeof(p), static_cast<uint8_t>(peer));
        UE_LOGI("flashlight: replayed peer %d state=%d (Intensity=%.2f "
                "senderElementId=0x%08x) to late joiner slot %d",
                peer, p.state, p.intensity, p.senderElementId, newSlot);
    }
}

void TickConnect() {
    PollLocal();

    // The receiver: every slot with a known beam is kept on its current puppet.
    const uint64_t now = NowMs();
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        PeerBeam& b = g_beam[slot];
        auto* rp = coop::players::Registry::Get().Puppet(slot);
        void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
        if (!b.wantValid) {
            // No beam known for the slot (its owner never had it on since we joined): a new puppet is made
            // dark once, so a light the class leaves visible never shows a beam its owner does not have.
            if (puppet && puppet != b.darkened && R::IsLive(puppet)) {
                b.darkened = puppet;
                if (void* light = E::GetMainPlayerLightR(puppet)) {
                    E::FlashlightSnapshot snap{};
                    if (E::ReadFlashlightSnapshot(light, snap) && snap.visible) {
                        E::SetSceneComponentVisibility(light, false, false);
                        UE_LOGI("flashlight: slot %u new puppet %p spawned with its light shown and no beam "
                                "known -- hidden", static_cast<unsigned>(slot), puppet);
                    }
                }
            }
            continue;
        }
        if (!puppet || !R::IsLive(puppet)) {
            if (b.actor) {
                UE_LOGI("flashlight: slot %u puppet gone -- beam (state=%d) waits for the next one",
                        static_cast<unsigned>(slot), b.want.state);
                UnbindBeam(b);
            }
            continue;
        }
        if (puppet != b.actor) {
            // A first puppet (the packet beat it) or a replacement: seeded from the wanted beam, silently.
            const char* why = b.everBound ? "new puppet" : "held packet";
            ApplyBeam(slot, puppet, why, /*audible=*/false);
            continue;
        }
        if (now >= b.nextCheckMs) {
            b.nextCheckMs = now + kCheckIntervalMs;
            const char* what = "";
            if (BeamDrifted(b, puppet, &what)) {
                ++b.heals;
                if (b.heals <= 5 || (b.heals % 50) == 0) {
                    UE_LOGW("flashlight: slot %u beam drifted (%s; want state=%d) -- re-applied (repair #%u)",
                            static_cast<unsigned>(slot), what, b.want.state, b.heals);
                }
                ApplyBeam(slot, puppet, "repair", /*audible=*/false);
                continue;
            }
        }
        if (b.want.state != 0 && b.light && !rp->IsRagdollDisplayed()) Condition(slot, b, *rp, now);
    }
}

void OnDisconnect() {
    int cleared = 0;
    for (uint8_t i = 0; i < coop::players::kMaxPeers; ++i) {
        if (g_beam[i].wantValid) ++cleared;
        g_beam[i] = PeerBeam{};
    }
    if (cleared > 0) UE_LOGI("flashlight: OnDisconnect cleared %d slot beam(s)", cleared);
}

void OnDisconnectForSlot(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // A leaver's beam goes, so a peer reusing the slot does not inherit its flashlight.
    PeerBeam& b = g_beam[peerSlot];
    if (b.wantValid) {
        UE_LOGI("flashlight: peer slot %d left -- clearing its beam (state=%d)", peerSlot, b.want.state);
    }
    b = PeerBeam{};
}

void OnSlotLeftWorld(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // Still in the session (a pocket trip): the beam stays wanted, so its puppet is lit again when it comes
    // back and a joiner in the meantime still gets it. Only the binding to the old puppet goes.
    PeerBeam& b = g_beam[peerSlot];
    if (!b.wantValid) return;
    UE_LOGI("flashlight: peer slot %d left the world -- beam (state=%d) kept for its return",
            peerSlot, b.want.state);
    UnbindBeam(b);
}

}  // namespace coop::item_activate
