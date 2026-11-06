// coop/interactables/saw_sync.cpp -- see coop/interactables/saw_sync.h.

#include "coop/interactables/saw_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/interactables/tool_action.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/hacksaw.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace coop::saw_sync {
namespace {

namespace EL = coop::element;
namespace HS = ue_wrap::hacksaw;
namespace N  = ue_wrap::profile::name;
namespace R  = ue_wrap::reflection;
namespace T  = coop::net::tool_action;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowHandUse, "saw_sync", L"prop_hacksaw_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowTick, "saw_sync", L"prop_hacksaw_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowComplete, "saw_sync", L"prop_hacksaw_C", L"completeSaw", Declared);
constexpr int kTagHandUse = 0x53415755;  // 'SAWU'
constexpr int kTagTick = 0x53415754;
constexpr int kTagComplete = 0x53415743;

constexpr float    kReachUU = 400.0f;      // the look-at trace's 200 uu, twice over, as a door verb's
constexpr uint64_t kProgressMs = 250;      // the owner's cut, four times a second
constexpr uint64_t kProgressFloorMs = 100; // HOST: a client's progress faster than this is dropped
constexpr float    kEarlyShare = 0.75f;    // HOST: a cut done in less than this share of its time is not one
constexpr uint64_t kSlackMs = 500;         // and the latency the share is given on top
constexpr float    kHeldComplete = 0.999f; // CLIENT: the bar held short of full while the host ends the cut
constexpr float    kMaxTimeSec = 36000.f;

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[3] = {};
bool g_settled = false;
int  g_applying = 0;  // this lane's own run of the saw's verbs: the watches let it through and send nothing

uint64_t g_sent = 0, g_ran = 0, g_refused = 0, g_unkeyed = 0;

struct Cut {
    std::wstring          key;
    ue_wrap::CachedObjRef target;
    ue_wrap::CachedObjRef saw;       // the owner's own saw (owner side only)
    uint8_t               owner = 0;
    float                 complete = 0.f;
    float                 timeSec = 0.f;
    int32_t               amount = 1;
    uint64_t              beganMs = 0;
    uint64_t              sentMs = 0;
    uint64_t              soundMs = 0;
    bool                  done = false;   // the owner's tick reached the end (host), or its end went (client)
};
std::vector<Cut> g_cuts;

uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }
uint64_t NowMs() { return coop::net::NowMs(); }
bool Live(coop::net::Session* s) { return s && s->running() && s->connected(); }

Cut* ByKey(const std::wstring& key) {
    for (Cut& c : g_cuts)
        if (c.key == key) return &c;
    return nullptr;
}
Cut* ByOwner(uint8_t owner) {
    for (Cut& c : g_cuts)
        if (c.owner == owner) return &c;
    return nullptr;
}
Cut* BySaw(void* saw) {
    for (Cut& c : g_cuts)
        if (c.saw.Is(saw)) return &c;
    return nullptr;
}
void Erase(const Cut* which) {
    for (size_t i = 0; i < g_cuts.size(); ++i)
        if (&g_cuts[i] == which) { g_cuts.erase(g_cuts.begin() + static_cast<long long>(i)); return; }
}

coop::net::ToolActionPayload Make(uint8_t op, const Cut& c) {
    coop::net::ToolActionPayload p{};
    p.kind = T::kSaw;
    p.op = op;
    p.owner = c.owner;
    coop::net::WireKeyFromString(c.key, p.target);
    p.alpha = c.complete < 0.f ? 0.f : (c.complete > 1.5f ? 1.5f : c.complete);
    if (op == T::kBegin) {
        p.stress = c.timeSec < 0.f ? 0.f : (c.timeSec > kMaxTimeSec ? kMaxTimeSec : c.timeSec);
        p.lives = static_cast<uint8_t>(c.amount < 1 ? 1 : (c.amount > 255 ? 255 : c.amount));
    }
    return p;
}

// The saw's sound, played at the cut for a cut this peer does not make: the owner's own is the game's, in 2D.
void PlaySawSound(void* target) {
    static ue_wrap::CachedObjRef s_cue;
    static void* s_att = nullptr;
    static bool s_said = false;
    void* cue = s_cue.Get();
    if (!cue) {
        cue = R::FindObject(N::SawSoundCue, L"SoundCue");
        if (cue) s_cue.Set(cue);
    }
    if (!cue) {
        if (!s_said) { s_said = true; UE_LOGW("[SAW] the saw sound '%ls' is not loaded -- another player's cut is silent", N::SawSoundCue); }
        return;
    }
    if (!s_att) s_att = ue_wrap::engine::SpawnSoundAttenuation(ue_wrap::engine::SoundAttenuationConfig{});
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(target, at)) return;
    ue_wrap::engine::PlaySoundAtLocation(target, cue, at, s_att);
}

void ResetOwnSaw(void* saw) {
    if (!saw) return;
    ++g_applying;
    HS::CallCompleteSaw(saw);
    --g_applying;
}

// ---- the owner's watches -----------------------------------------------------------------------------------------
void OnHandUsePost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s) || !coop::hand_item::IsLocalHandViewActor(c.object)) return;
    void* target = HS::SawingProp(c.object);
    if (!target) return;  // the saw refused the prop (its hint said why)
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(target);
    if (key.empty() || key == L"None" || key.size() > sizeof(coop::net::WireKey::data)) {
        if (++g_unkeyed <= 3)
            UE_LOGW("[SAW] the cut of %ls, which has no key the wire carries -- it runs on this screen alone",
                    R::ClassNameOf(target).c_str());
        return;
    }
    if (Cut* old = ByOwner(LocalSlot())) Erase(old);
    Cut cut;
    cut.key = key;
    cut.target.Set(target);
    cut.saw.Set(c.object);
    cut.owner = LocalSlot();
    float complete = 0.f;
    HS::ReadCut(c.object, complete, cut.timeSec);
    cut.beganMs = cut.sentMs = NowMs();
    g_cuts.push_back(cut);
    coop::tool_action::Send(*s, Make(T::kBegin, cut));
    ++g_sent;
    UE_LOGI("[SAW] %s: a cut of key='%ls' (%ls) began, %.1f s", s->role() == coop::net::Role::Host ? "host" : "client",
            key.c_str(), R::ClassNameOf(target).c_str(), cut.timeSec);
}

R::ParamOffset g_tickDelta{L"DeltaSeconds"};

sg::Verdict OnTickPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s) || !c.locals) return sg::Verdict::Run;
    Cut* cut = BySaw(c.object);
    if (!cut) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Client && cut->done) return sg::Verdict::Cancel;  // waiting for the host's end
    float complete = 0.f, timeSec = 0.f;
    const int32_t off = g_tickDelta.Of(c.function);
    if (off < 0 || !HS::ReadCut(c.object, complete, timeSec) || timeSec <= 0.f) return sg::Verdict::Run;
    float dt = 0.f;
    std::memcpy(&dt, c.locals + off, sizeof(dt));
    if (complete + dt / timeSec < 1.0f) return sg::Verdict::Run;
    cut->complete = 1.0f;
    if (s->role() == coop::net::Role::Host) {
        cut->done = true;  // the host's own tick ends its cut: completeSaw names it done
        return sg::Verdict::Run;
    }
    // A client's tick would spawn the scraps and destroy the prop: the host does, on its copy.
    HS::WriteComplete(c.object, kHeldComplete);
    cut->done = true;
    coop::net::ToolActionPayload out = Make(T::kEnd, *cut);
    out.result = T::kDone;
    coop::tool_action::Send(*s, out);
    ++g_sent;
    UE_LOGI("[SAW] client: the cut of key='%ls' is through -- the host ends it", cut->key.c_str());
    return sg::Verdict::Cancel;
}

void OnCompletePost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s)) return;
    Cut* cut = BySaw(c.object);
    if (!cut) return;
    const bool host = s->role() == coop::net::Role::Host;
    // A client whose end already went waits for the host's word; its own reset here (a lost aim, the saw's destroy)
    // is still the end of its screen's cut.
    if (!(cut->done && !host)) {
        coop::net::ToolActionPayload out = Make(T::kEnd, *cut);
        out.result = (host && cut->done) ? T::kDone : T::kCancel;
        coop::tool_action::Send(*s, out);
        ++g_sent;
        UE_LOGI("[SAW] %s: the cut of key='%ls' %s", host ? "host" : "client", cut->key.c_str(),
                out.result == T::kDone ? "is done" : "stopped");
    }
    Erase(cut);
}

void RegisterWatches() {
    if (g_reg[0] == Reg::Pending) g_reg[0] = sg::WatchName(kRowHandUse, kTagHandUse, nullptr, &OnHandUsePost) ? Reg::Registered : Reg::Refused;
    if (g_reg[1] == Reg::Pending) g_reg[1] = sg::WatchName(kRowTick, kTagTick, &OnTickPre, nullptr) ? Reg::Registered : Reg::Refused;
    if (g_reg[2] == Reg::Pending) g_reg[2] = sg::WatchName(kRowComplete, kTagComplete, nullptr, &OnCompletePost) ? Reg::Registered : Reg::Refused;
}

void Settle() {
    if (g_settled) return;
    const bool live = g_reg[0] == Reg::Registered && g_reg[1] == Reg::Registered && g_reg[2] == Reg::Registered &&
                      sg::NameWatchLive(kRowHandUse, kTagHandUse) && sg::NameWatchLive(kRowTick, kTagTick) &&
                      sg::NameWatchLive(kRowComplete, kTagComplete);
    if (live) {
        g_settled = true;
        UE_LOGI("[SAW] the hacksaw's hand use, tick and reset watches are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[SAW] a hacksaw watch is dead (refused, or the class declares no such body) -- a cut is seen only "
                "where it is made");
    }
}

// ---- host: the judge ---------------------------------------------------------------------------------------------
void Refuse(coop::net::Session& s, uint8_t slot, const coop::net::ToolActionPayload& in, const char* why) {
    ++g_refused;
    UE_LOGI("[SAW] host: REFUSE slot %u's %s on key='%ls' -- %s", static_cast<unsigned>(slot),
            in.op == T::kBegin ? "cut" : "end of its cut", coop::net::StringFromWireKey(in.target).c_str(), why);
    coop::net::ToolActionPayload out = in;
    out.op = T::kRefuse;
    out.owner = slot;
    out.result = 0;
    coop::tool_action::SendToSlot(s, slot, out);
}

void EndBroadcast(coop::net::Session& s, const Cut& c, uint8_t result, int skip) {
    coop::net::ToolActionPayload out = Make(T::kEnd, c);
    out.result = result;
    coop::tool_action::Send(s, out, skip);
}

void HostBegin(coop::net::Session& s, const coop::net::ToolActionPayload& in, uint8_t slot, const std::wstring& key) {
    void* target = coop::prop_key_index::Find(key);
    if (!target || !R::IsLive(target) || !ue_wrap::prop::IsDescendantOfProp(target))
        return Refuse(s, slot, in, "no prop here holds the key");
    if (ue_wrap::engine::IsChildActor(target)) return Refuse(s, slot, in, "the prop is a child actor");
    if (!coop::hand_item::HeldClassIs(slot, N::HacksawClass)) return Refuse(s, slot, in, "the sender holds no hacksaw");
    if (ByKey(key)) return Refuse(s, slot, in, "another player is cutting it");
    if (!HS::TargetSawable(target)) return Refuse(s, slot, in, "the prop is not sawable");
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(target);
    if (!subject) return Refuse(s, slot, in, EL::OutcomeName(subject.outcome));
    if (Cut* old = ByOwner(slot)) Erase(old);
    Cut cut;
    cut.key = key;
    cut.target.Set(target);
    cut.owner = slot;
    if (!HS::MeasureCut(target, cut.timeSec, cut.amount)) {
        // The measure needs the mesh's volume and the material's density; without them the sender's own time is
        // taken, bounded, and the cut leaves the one scrap the game leaves for any small prop.
        cut.timeSec = in.stress;
        cut.amount = 1;
        UE_LOGW("[SAW] host: the cut of key='%ls' could not be measured here -- its time is the sender's %.1f s",
                key.c_str(), cut.timeSec);
    }
    cut.beganMs = cut.sentMs = cut.soundMs = NowMs();
    g_cuts.push_back(cut);
    coop::net::ToolActionPayload out = Make(T::kBegin, cut);
    coop::tool_action::Send(s, out, slot);
    UE_LOGI("[SAW] host: slot %u cuts key='%ls' (%ls): %.1f s, %d scraps", static_cast<unsigned>(slot), key.c_str(),
            R::ClassNameOf(target).c_str(), cut.timeSec, static_cast<int>(cut.amount));
}

void HostEnd(coop::net::Session& s, const coop::net::ToolActionPayload& in, uint8_t slot, Cut& cut) {
    const Cut c = cut;
    Erase(&cut);
    if (in.result != T::kDone) {
        EndBroadcast(s, c, T::kCancel, slot);
        return;
    }
    const uint64_t took = NowMs() - c.beganMs;
    const uint64_t least = static_cast<uint64_t>(c.timeSec * 1000.f * kEarlyShare);
    void* target = c.target.Get();
    void* saw = coop::hand_item::MirrorActorForSlot(slot);
    const char* why = nullptr;
    if (took + kSlackMs < least) why = "the cut ended sooner than the prop takes";
    else if (!target) why = "the prop is gone here";
    else if (!HS::IsHacksaw(saw)) why = "the host shows no hacksaw in the sender's hand";
    if (!why) {
        ++g_applying;
        const bool ran = HS::RunCutEnd(saw, target, c.amount);
        --g_applying;
        if (!ran) why = "the saw refused the prop's material here";
    }
    if (why) {
        Refuse(s, slot, in, why);
        EndBroadcast(s, c, T::kCancel, slot);
        return;
    }
    ++g_ran;
    EndBroadcast(s, c, T::kDone, -1);
    UE_LOGI("[SAW] host: ran slot %u's cut of key='%ls' after %llu ms -- %d scraps", static_cast<unsigned>(slot),
            c.key.c_str(), static_cast<unsigned long long>(took), static_cast<int>(c.amount));
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (session) RegisterWatches();
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (!Live(&session) || g_cuts.empty()) return;
    const uint64_t now = NowMs();
    const uint8_t self = LocalSlot();
    const bool host = session.role() == coop::net::Role::Host;
    for (size_t i = 0; i < g_cuts.size();) {
        Cut& c = g_cuts[i];
        void* target = c.target.Get();
        // HOST: a client's cut ends with its prop, or with the hacksaw leaving the sender's hand.
        if (host && c.owner != self && (!target || !coop::hand_item::HeldClassIs(c.owner, N::HacksawClass))) {
            EndBroadcast(session, c, T::kCancel, -1);
            g_cuts.erase(g_cuts.begin() + static_cast<long long>(i));
            continue;
        }
        if (c.owner == self) {
            float complete = 0.f, timeSec = 0.f;
            void* saw = c.saw.Get();
            if (saw && !c.done && now - c.sentMs >= kProgressMs && HS::ReadCut(saw, complete, timeSec) &&
                std::fabs(complete - c.complete) > 0.005f) {
                c.complete = complete;
                c.sentMs = now;
                coop::tool_action::Send(session, Make(T::kProgress, c));
            }
        } else if (target && now - c.soundMs >= static_cast<uint64_t>(N::HacksawSoundPeriodSec * 1000.f)) {
            c.soundMs = now;
            PlaySawSound(target);
        }
        ++i;
    }
}

void OnIntent(coop::net::Session& session, const coop::net::ToolActionPayload& in, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(in.target);
    if (in.op == T::kBegin) return HostBegin(session, in, slot, key);
    Cut* c = ByKey(key);
    if (!c || c->owner != slot) {
        if (in.op == T::kEnd && in.result == T::kDone) Refuse(session, slot, in, "the sender cuts nothing there");
        return;
    }
    if (in.op == T::kProgress) {
        const uint64_t now = NowMs();
        if (now - c->sentMs < kProgressFloorMs) return;
        c->sentMs = now;
        c->complete = in.alpha;
        coop::tool_action::Send(session, Make(T::kProgress, *c), slot);
        return;
    }
    if (in.op == T::kEnd) HostEnd(session, in, slot, *c);
}

void OnState(coop::net::Session&, const coop::net::ToolActionPayload& in) {
    const std::wstring key = coop::net::StringFromWireKey(in.target);
    const uint8_t self = LocalSlot();
    switch (in.op) {
    case T::kBegin: {
        if (in.owner == self) return;
        if (Cut* old = ByOwner(in.owner)) Erase(old);
        Cut c;
        c.key = key;
        c.owner = in.owner;
        c.complete = in.alpha;
        c.timeSec = in.stress;
        c.amount = in.lives;
        if (void* t = coop::prop_key_index::Find(key)) c.target.Set(t);
        c.beganMs = c.sentMs = c.soundMs = NowMs();
        g_cuts.push_back(c);
        return;
    }
    case T::kProgress:
        if (Cut* c = ByKey(key); c && c->owner != self) c->complete = in.alpha;
        return;
    case T::kEnd:
    case T::kRefuse: {
        Cut* c = ByKey(key);
        if (!c) return;
        void* saw = c->owner == self ? c->saw.Get() : nullptr;
        const bool own = c->owner == self;
        Erase(c);
        if (own) {
            ResetOwnSaw(saw);
            if (in.op == T::kRefuse)
                UE_LOGI("[SAW] client: the host refused the cut of key='%ls' -- the saw stops", key.c_str());
        }
        return;
    }
    default: return;
    }
}

void ConnectReplayForSlot(coop::net::Session& session, int slot) {
    for (const Cut& c : g_cuts) coop::tool_action::SendToSlot(session, slot, Make(T::kBegin, c));
    if (!g_cuts.empty()) UE_LOGI("[SAW] host: %zu running cuts to slot %d's join", g_cuts.size(), slot);
}

void OnPeerLeft(coop::net::Session& session, uint8_t slot) {
    for (size_t i = 0; i < g_cuts.size();) {
        if (g_cuts[i].owner != slot) { ++i; continue; }
        EndBroadcast(session, g_cuts[i], T::kCancel, slot);
        g_cuts.erase(g_cuts.begin() + static_cast<long long>(i));
    }
}

void OnDisconnect() {
    if (g_sent || g_ran || g_refused)
        UE_LOGI("[SAW] session end -- sent=%llu host-run=%llu refused=%llu", static_cast<unsigned long long>(g_sent),
                static_cast<unsigned long long>(g_ran), static_cast<unsigned long long>(g_refused));
    g_sent = g_ran = g_refused = g_unkeyed = 0;
    g_cuts.clear();
    g_applying = 0;
}

}  // namespace coop::saw_sync
