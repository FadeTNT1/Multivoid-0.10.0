// coop/world/event_scene_sync.cpp -- see coop/world/event_scene_sync.h. The phase table, the wire and a client's
// apply; the watches are event_scene_refuse.cpp's, the prank's clients' props event_scene_agrav.cpp's.

#include "coop/world/event_scene_sync.h"

#include "event_scene_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/mirror_managers.h"  // WaMirrors
#include "coop/element/world_actor.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/world/event_sound_relay.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/actors/event_scenes.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::event_scene_sync {
namespace {

namespace R   = ue_wrap::reflection;
namespace P   = ue_wrap::profile;
namespace ES  = ue_wrap::event_scenes;
namespace ids = coop::net::event_scene_ids;
namespace ph  = coop::net::event_scene_phases;
namespace fl  = coop::net::event_scene_flags;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchesInstalled = false;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// A level-placed scene's target is found by name; the others are world-actor mirrors by element id.
bool IsLevelScene(uint8_t scene) { return scene == ids::kBoarShield || scene == ids::kAgrav; }

const wchar_t* LevelClassOf(uint8_t scene) {
    return scene == ids::kBoarShield ? P::name::UfoShieldShaderClass : P::name::TriggerAgravClass;
}

// The phase each scene may name.
uint8_t LastPhaseOf(uint8_t scene) {
    switch (scene) {
    case ids::kSoltoClean: return ph::kSoltoThrown;
    case ids::kObelisk:    return ph::kObeliskLanded;
    case ids::kBoarShield: return ph::kShieldFlyAway;
    case ids::kAgrav:      return ph::kAgravCloakOff;
    default:               return 0;
    }
}

// A phase that leaves the scene at its resting state: nothing is left for a joiner to see, so the row is dropped
// once sent.
bool IsRestingPhase(uint8_t scene, uint8_t phase) {
    return (scene == ids::kBoarShield && phase == ph::kShieldHidden) ||
           (scene == ids::kAgrav && phase == ph::kAgravCloakOff);
}

// ---- the host's table ----

struct HostScene {
    uint8_t      scene = 0;
    uint8_t      phase = 0;
    void*        actor = nullptr;
    int32_t      idx   = -1;
    uint32_t     eid   = 0;     // the world-actor element, once the lane has it
    std::wstring key;           // a level-placed target's short name
    bool         dirty = false; // a phase not sent yet
};
std::vector<HostScene> g_host;

coop::net::EventScenePhasePayload PayloadOf(const HostScene& h, bool snapshot) {
    coop::net::EventScenePhasePayload p{};
    p.scene = h.scene;
    p.phase = h.phase;
    p.flags = snapshot ? fl::kSnapshot : 0;
    p.eid = h.eid;
    if (IsLevelScene(h.scene)) coop::net::WireKeyFromString(h.key, p.key);
    return p;
}

// ---- a client's applied phases and held ones ----

std::unordered_map<uint32_t, uint8_t> g_appliedByEid;      // a mirror's newest applied phase
std::unordered_map<std::wstring, uint8_t> g_appliedByKey;  // a level actor's
struct Held {
    coop::net::EventScenePhasePayload p;
    long long sinceMs;
};
std::unordered_map<uint32_t, coop::net::EventScenePhasePayload> g_heldForMirror;  // the mirror not materialised yet
std::vector<Held> g_heldForLevel;   // the level actor not found yet
constexpr long long kLevelHoldMs = 30000;

void ApplySolto(void* actor, uint32_t eid, uint8_t phase, bool snapshot) {
    uint8_t& last = g_appliedByEid[eid];
    for (uint8_t step = static_cast<uint8_t>(last + 1); step <= phase; ++step) {
        bool ok = true;
        if (step == ph::kSoltoRevealed) ok = ES::SoltomiaReveal(actor);
        // A joiner lands on the cleaner standing: the montage it missed is not replayed from its start.
        else if (step == ph::kSoltoMontage) ok = snapshot ? true : ES::SoltomiaPlayMontage(actor);
        else if (step == ph::kSoltoThrown) ok = ES::SoltomiaThrowPresentation(actor);
        UE_LOGI("event_scene[soltoClean]: eid=%u phase %u%s applied%s", eid, step, snapshot ? " (snapshot)" : "",
                ok ? "" : " -- a verb did not dispatch");
    }
    if (phase > last) last = phase;
}

void ApplyToMirror(const coop::net::EventScenePhasePayload& p, void* actor) {
    const bool snapshot = (p.flags & fl::kSnapshot) != 0;
    if (p.scene == ids::kSoltoClean) {
        ApplySolto(actor, p.eid, p.phase, snapshot);
        return;
    }
    if (p.scene == ids::kObelisk) {
        uint8_t& last = g_appliedByEid[p.eid];
        if (last >= p.phase) return;
        last = p.phase;
        // A joiner's mirror is born rising: its begin-play's one-second check then takes the fallen branch.
        // A live mirror's begin-play registered the obelisk as a running event; the landing it refuses here would
        // have taken that back, so the presentation does (a joiner's fallen branch takes it back itself).
        const bool ok = snapshot ? ES::ObeliskMarkFallen(actor) : (ES::ObeliskLand(actor) & ES::EndOwnEvent(actor));
        UE_LOGI("event_scene[obelisk]: eid=%u landed%s applied%s", p.eid, snapshot ? " (snapshot)" : "",
                ok ? "" : " -- a verb did not dispatch");
    }
}

// True when the level actor was found and the phase applied.
bool ApplyToLevel(const coop::net::EventScenePhasePayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* actor = ES::FindLevelActor(LevelClassOf(p.scene), key);
    if (!actor) actor = ES::FindFirstLevelActor(LevelClassOf(p.scene));
    if (!actor) return false;
    const bool snapshot = (p.flags & fl::kSnapshot) != 0;
    bool ok = false;
    if (p.scene == ids::kBoarShield) {
        if (p.phase == ph::kShieldShown) ok = ES::UfoShieldSetActive(actor, true);
        else if (p.phase == ph::kShieldHidden) ok = ES::UfoShieldSetActive(actor, false);
        // A joiner lands after the exit: the shader is hidden.
        else ok = snapshot ? ES::UfoShieldSetActive(actor, false) : ES::UfoShieldFlyAway(actor);
    } else {
        ok = ES::AgravCloak(actor, p.phase == ph::kAgravCloakOn);
    }
    g_appliedByKey[key] = p.phase;
    UE_LOGI("event_scene[%s]: '%ls' phase %u%s applied%s", p.scene == ids::kBoarShield ? "boarShield" : "agrav",
            key.c_str(), p.phase, snapshot ? " (snapshot)" : "", ok ? "" : " -- a verb did not dispatch");
    return true;
}

void* LiveMirror(uint32_t eid) {
    coop::element::WorldActor* el = coop::element::WaMirrors().Get(eid);
    void* actor = el ? el->GetActor() : nullptr;
    return actor && R::IsLiveByIndex(actor, el->GetInternalIdx()) ? actor : nullptr;
}

}  // namespace

namespace detail {

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }

bool IsHost() {
    auto* s = Session();
    return s && s->running() && s->role() == coop::net::Role::Host;
}

bool IsActiveClient() {
    auto* s = Session();
    return s && s->running() && s->role() == coop::net::Role::Client;
}

void NoteHostPhase(uint8_t scene, uint8_t phase, void* actor) {
    if (!IsHost() || !actor || phase == 0 || phase > LastPhaseOf(scene)) return;
    const int32_t idx = R::InternalIndexOf(actor);
    HostScene* row = nullptr;
    for (HostScene& h : g_host)
        if (h.scene == scene && h.actor == actor && h.idx == idx) { row = &h; break; }
    if (!row) {
        g_host.push_back(HostScene{});
        row = &g_host.back();
        row->scene = scene;
        row->actor = actor;
        row->idx = idx;
        if (IsLevelScene(scene)) row->key = ES::ShortName(actor);
    }
    // The cleaner's phases only advance; a re-entered body (the overlap is a DoOnce) adds nothing.
    if (scene == ids::kSoltoClean && phase <= row->phase) return;
    row->phase = phase;
    row->dirty = true;
    UE_LOGI("event_scene: host scene %u reached phase %u on %p", scene, phase, actor);
}

}  // namespace detail

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::event_sound_relay::Install(session);
    if (g_watchesInstalled) return;
    g_watchesInstalled = true;
    detail::InstallWatches();
}

void Tick() {
    auto* s = detail::Session();
    if (!s || !s->running()) return;
    if (s->role() == coop::net::Role::Host) {
        detail::CrawlerHostTick();
        coop::event_sound_relay::Tick();
        for (size_t i = 0; i < g_host.size();) {
            HostScene& h = g_host[i];
            if (!R::IsLiveByIndex(h.actor, h.idx)) {
                g_host.erase(g_host.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (h.dirty) {
                if (!IsLevelScene(h.scene) && h.eid == 0) h.eid = coop::world_actor_sync::HostEidOf(h.actor);
                if (IsLevelScene(h.scene) || h.eid != 0) {
                    const coop::net::EventScenePhasePayload p = PayloadOf(h, false);
                    if (s->AnyWorldReadyPeer()) s->SendReliable(coop::net::ReliableKind::EventScenePhase, &p, sizeof(p));
                    h.dirty = false;
                    if (IsRestingPhase(h.scene, h.phase)) {
                        g_host.erase(g_host.begin() + static_cast<std::ptrdiff_t>(i));
                        continue;
                    }
                }
            }
            ++i;
        }
        return;
    }
    if (g_heldForLevel.empty()) return;
    const long long now = NowMs();
    for (size_t i = 0; i < g_heldForLevel.size();) {
        if (ApplyToLevel(g_heldForLevel[i].p) || now - g_heldForLevel[i].sinceMs > kLevelHoldMs) {
            g_heldForLevel.erase(g_heldForLevel.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = detail::Session();
    if (!s || s->role() != coop::net::Role::Host || slot < 1) return;
    int sent = 0;
    for (HostScene& h : g_host) {
        if (!R::IsLiveByIndex(h.actor, h.idx) || h.phase == 0) continue;
        if (!IsLevelScene(h.scene) && h.eid == 0) h.eid = coop::world_actor_sync::HostEidOf(h.actor);
        if (!IsLevelScene(h.scene) && h.eid == 0) continue;
        const coop::net::EventScenePhasePayload p = PayloadOf(h, true);
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::EventScenePhase, &p, sizeof(p))) ++sent;
    }
    if (sent > 0) UE_LOGI("event_scene: %d live scene phase(s) to joiner slot %d", sent, slot);
}

bool WellFormed(const coop::net::EventScenePhasePayload& p) {
    if (p.scene == 0 || p.scene > ids::kLast) return false;
    if (p.phase == 0 || p.phase > LastPhaseOf(p.scene)) return false;
    if ((p.flags & ~fl::kKnown) != 0 || p.reserved != 0) return false;
    if (p.key.len > sizeof(p.key.data)) return false;
    // A mirror scene names its element and no key; a level scene names its key and no element.
    if (IsLevelScene(p.scene)) return p.eid == 0 && p.key.len > 0;
    return p.eid != 0 && p.key.len == 0;
}

void OnPhase(const coop::net::EventScenePhasePayload& p) {
    if (!detail::IsActiveClient()) return;
    if (IsLevelScene(p.scene)) {
        if (!ApplyToLevel(p)) {
            UE_LOGI("event_scene: scene %u phase %u -- its level actor is not here yet, held", p.scene, p.phase);
            g_heldForLevel.push_back(Held{p, NowMs()});
        }
        return;
    }
    if (void* actor = LiveMirror(p.eid)) {
        ApplyToMirror(p, actor);
        return;
    }
    // Its world-actor spawn rides the same lane ahead of it, so this is a materialise that failed or is pending.
    UE_LOGI("event_scene: scene %u phase %u for eid=%u -- no mirror yet, held for its birth", p.scene, p.phase, p.eid);
    auto it = g_heldForMirror.find(p.eid);
    if (it == g_heldForMirror.end() || it->second.phase < p.phase) g_heldForMirror[p.eid] = p;
}

void OnMirrorMaterialized(uint32_t eid, void* actor) {
    if (g_heldForMirror.empty()) return;
    auto it = g_heldForMirror.find(eid);
    if (it == g_heldForMirror.end()) return;
    const coop::net::EventScenePhasePayload p = it->second;
    g_heldForMirror.erase(it);
    ApplyToMirror(p, actor);
}

void OnDisconnect() {
    g_host.clear();
    g_appliedByEid.clear();
    g_appliedByKey.clear();
    g_heldForMirror.clear();
    g_heldForLevel.clear();
    detail::AgravOnDisconnect();
    detail::CrawlerOnDisconnect();
    coop::event_sound_relay::OnDisconnect();
}

}  // namespace coop::event_scene_sync
