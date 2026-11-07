// coop/interactables/toolbox_sync.cpp -- see coop/interactables/toolbox_sync.h.

#include "coop/interactables/toolbox_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/interactables/tool_action.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/world/basement_pipe_sync.h"

#include "ue_wrap/actors/toolbox.h"
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

namespace coop::toolbox_sync {
namespace {

namespace EL = coop::element;
namespace TB = ue_wrap::toolbox;
namespace N  = ue_wrap::profile::name;
namespace R  = ue_wrap::reflection;
namespace T  = coop::net::tool_action;
namespace sg = ue_wrap::script_gate;
namespace BP = coop::basement_pipe_sync;

UE_FACT_FN(kRowHandUse, "toolbox_sync", L"prop_toolbox_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowTick, "toolbox_sync", L"prop_toolbox_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowComplete, "toolbox_sync", L"prop_toolbox_C", L"completeSaw", Declared);
constexpr int kTagHandUse = 0x54424855;  // 'TBHU'
constexpr int kTagTick = 0x54425454;     // 'TBTT'
constexpr int kTagComplete = 0x54424343; // 'TBCC'

constexpr float    kReachUU = 400.0f;      // the look-at trace's reach, twice over
constexpr uint64_t kProgressMs = 250;      // the owner's mend, four times a second
constexpr uint64_t kProgressFloorMs = 100; // HOST: a client's progress faster than this is dropped
constexpr float    kEarlyShare = 0.75f;    // HOST: a mend done in less than this share of its time is not one
constexpr uint64_t kSlackMs = 500;         // and the latency the share is given on top
constexpr float    kHeldComplete = 0.999f; // CLIENT: the bar held short of full while the host ends the mend
constexpr float    kPowerToolShare = 1.0f / static_cast<float>(N::ToolboxPowerToolDivisor);
constexpr float    kTimeSlack = 1.1f;      // HOST: a claimed time may run this far over the pipe's own

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[3] = {};
bool g_settled = false;
int  g_applying = 0;  // this lane's own run of the kit's verbs: the watches let it through and send nothing

uint64_t g_sent = 0, g_ran = 0, g_refused = 0, g_unkeyed = 0;

struct Mend {
    std::wstring          key;     // the pipe's portable wire key
    ue_wrap::CachedObjRef pipe;
    ue_wrap::CachedObjRef kit;     // the owner's own kit (owner side only)
    uint8_t               owner = 0;
    float                 complete = 0.f;
    float                 timeSec = 0.f;
    int32_t               lives = 0;
    uint64_t              beganMs = 0;
    uint64_t              sentMs = 0;
    uint64_t              soundMs = 0;
    bool                  done = false;
};
std::vector<Mend> g_mends;

uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }
uint64_t NowMs() { return coop::net::NowMs(); }
bool Live(coop::net::Session* s) { return s && s->running() && s->connected(); }

Mend* ByKey(const std::wstring& key) {
    for (Mend& m : g_mends)
        if (m.key == key) return &m;
    return nullptr;
}
Mend* ByOwner(uint8_t owner) {
    for (Mend& m : g_mends)
        if (m.owner == owner) return &m;
    return nullptr;
}
Mend* ByKit(void* kit) {
    for (Mend& m : g_mends)
        if (m.kit.Is(kit)) return &m;
    return nullptr;
}
void Erase(const Mend* which) {
    for (size_t i = 0; i < g_mends.size(); ++i)
        if (&g_mends[i] == which) { g_mends.erase(g_mends.begin() + static_cast<long long>(i)); return; }
}

coop::net::ToolActionPayload Make(uint8_t op, const Mend& m) {
    coop::net::ToolActionPayload p{};
    p.kind = T::kToolbox;
    p.op = op;
    p.owner = m.owner;
    coop::net::WireKeyFromString(m.key, p.target);
    p.alpha = m.complete < 0.f ? 0.f : (m.complete > 1.5f ? 1.5f : m.complete);
    if (op == T::kBegin) {
        p.stress = m.timeSec < 0.f ? 0.f : (m.timeSec > 36000.f ? 36000.f : m.timeSec);
        p.lives = static_cast<uint8_t>(m.lives < 0 ? 0 : (m.lives > 255 ? 255 : m.lives));
    }
    return p;
}

// The kit's repair sound, played at the pipe for a mend this peer does not make: the owner's own is the game's, in 2D.
void PlayFixSound(void* pipe) {
    static void* s_att = nullptr;
    static bool  s_said = false;
    void* cue = TB::FixSound();
    if (!cue) {
        if (!s_said) { s_said = true; UE_LOGW("[TOOLBOX] the kit's repair sound is not loaded -- another player's mend is silent"); }
        return;
    }
    if (!s_att) s_att = ue_wrap::engine::SpawnSoundAttenuation(ue_wrap::engine::SoundAttenuationConfig{});
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(pipe, at)) return;
    ue_wrap::engine::PlaySoundAtLocation(pipe, cue, at, s_att);
}

void ResetOwnKit(void* kit) {
    if (!kit) return;
    ++g_applying;
    TB::CallCompleteSaw(kit);
    --g_applying;
}

// CLIENT: the host judged the owner's mend good -- the kit runs its own end on its own pipe, which takes the use, resets,
// says the uses and, with none left, destroys the kit. The pipe is the host's already (or about to be): it is burst here
// for the kit's toolboxFix to find, and its state call is let through.
void RunOwnEnd(Mend& m) {
    void* kit = m.kit.Get();
    void* pipe = m.pipe.Get();
    if (!kit) return;
    BP::ScopedApply allow;
    ++g_applying;
    bool damaged = false;
    if (pipe && TB::PipeDamaged(pipe, damaged) && !damaged) TB::PipeSetState(pipe, true);
    const bool ran = TB::RunFixEnd(kit);
    --g_applying;
    UE_LOGI("[TOOLBOX] client: the host ended the mend of '%ls'%s", m.key.c_str(), ran ? "" : " -- the kit did not run its end");
}

// ---- the owner's watches ---------------------------------------------------------------------------------------------
void OnHandUsePost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s) || !coop::hand_item::IsLocalHandViewActor(c.object)) return;
    void* pipe = TB::FixingActor(c.object);
    if (!pipe || !TB::IsPipe(pipe)) return;  // nothing fixable, or an ATV wheel: that one stays on this screen
    const std::wstring key = BP::KeyOf(pipe);
    if (key.empty() || key.size() > sizeof(coop::net::WireKey::data)) {
        if (++g_unkeyed <= 3) UE_LOGW("[TOOLBOX] a pipe with no key the wire carries -- its mend runs on this screen alone");
        return;
    }
    if (Mend* old = ByOwner(LocalSlot())) Erase(old);
    Mend m;
    m.key = key;
    m.pipe.Set(pipe);
    m.kit.Set(c.object);
    m.owner = LocalSlot();
    float complete = 0.f;
    TB::ReadFix(c.object, complete, m.timeSec, m.lives);
    m.beganMs = m.sentMs = NowMs();
    g_mends.push_back(m);
    coop::tool_action::Send(*s, Make(T::kBegin, m));
    ++g_sent;
    UE_LOGI("[TOOLBOX] %s: a mend of pipe '%ls' began, %.1f s, %d use(s) left",
            s->role() == coop::net::Role::Host ? "host" : "client", key.c_str(), m.timeSec, static_cast<int>(m.lives));
}

R::ParamOffset g_tickDelta{L"DeltaSeconds"};

sg::Verdict OnTickPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s) || !c.locals) return sg::Verdict::Run;
    Mend* m = ByKit(c.object);
    if (!m) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Client && m->done) return sg::Verdict::Cancel;  // waiting for the host's end
    float complete = 0.f, timeSec = 0.f;
    int32_t fixes = 0;
    const int32_t off = g_tickDelta.Of(c.function);
    if (off < 0 || !TB::ReadFix(c.object, complete, timeSec, fixes) || timeSec <= 0.f) return sg::Verdict::Run;
    float dt = 0.f;
    std::memcpy(&dt, c.locals + off, sizeof(dt));
    if (complete + dt / timeSec < 1.0f) return sg::Verdict::Run;
    m->complete = 1.0f;
    if (s->role() == coop::net::Role::Host) {
        m->done = true;  // the host's own tick ends its mend: completeSaw names it done
        return sg::Verdict::Run;
    }
    // A client's tick would mend its copy of the pipe and take the use: the host judges first.
    TB::WriteComplete(c.object, kHeldComplete);
    m->done = true;
    coop::net::ToolActionPayload out = Make(T::kEnd, *m);
    out.result = T::kDone;
    coop::tool_action::Send(*s, out);
    ++g_sent;
    UE_LOGI("[TOOLBOX] client: the mend of pipe '%ls' is through -- the host ends it", m->key.c_str());
    return sg::Verdict::Cancel;
}

void OnCompletePost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s)) return;
    Mend* m = ByKit(c.object);
    if (!m) return;
    const bool host = s->role() == coop::net::Role::Host;
    // A client whose end already went waits for the host's word; its own reset here (a lost aim) is still the end of
    // its screen's mend.
    if (!(m->done && !host)) {
        coop::net::ToolActionPayload out = Make(T::kEnd, *m);
        out.result = (host && m->done) ? T::kDone : T::kCancel;
        coop::tool_action::Send(*s, out);
        ++g_sent;
        UE_LOGI("[TOOLBOX] %s: the mend of pipe '%ls' %s", host ? "host" : "client", m->key.c_str(),
                out.result == T::kDone ? "is done" : "stopped");
    }
    Erase(m);
}

void RegisterWatches() {
    if (g_reg[0] == Reg::Pending) g_reg[0] = sg::WatchName(kRowHandUse, kTagHandUse, nullptr, &OnHandUsePost) ? Reg::Registered : Reg::Refused;
    if (g_reg[1] == Reg::Pending) g_reg[1] = sg::WatchName(kRowTick, kTagTick, &OnTickPre, nullptr) ? Reg::Registered : Reg::Refused;
    if (g_reg[2] == Reg::Pending) g_reg[2] = sg::WatchName(kRowComplete, kTagComplete, nullptr, &OnCompletePost) ? Reg::Registered : Reg::Refused;
}

void Settle() {
    if (g_settled) return;
    // Nothing is judged before the watches are registered (Tick can run ahead of Install): none pending then
    // reads as every watch dead.
    if (g_reg[0] == Reg::Pending || g_reg[1] == Reg::Pending || g_reg[2] == Reg::Pending) return;
    const bool live = g_reg[0] == Reg::Registered && g_reg[1] == Reg::Registered && g_reg[2] == Reg::Registered &&
                      sg::NameWatchLive(kRowHandUse, kTagHandUse) && sg::NameWatchLive(kRowTick, kTagTick) &&
                      sg::NameWatchLive(kRowComplete, kTagComplete);
    if (live) {
        g_settled = true;
        UE_LOGI("[TOOLBOX] the repair kit's hand use, tick and reset watches are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[TOOLBOX] a repair kit watch is dead (refused, or the class declares no such body) -- a mend is seen only "
                "where it is made");
    }
}

// ---- host: the judge ---------------------------------------------------------------------------------------------
void Refuse(coop::net::Session& s, uint8_t slot, const coop::net::ToolActionPayload& in, const char* why) {
    ++g_refused;
    UE_LOGI("[TOOLBOX] host: REFUSE slot %u's %s on pipe '%ls' -- %s", static_cast<unsigned>(slot),
            in.op == T::kBegin ? "mend" : "end of its mend", coop::net::StringFromWireKey(in.target).c_str(), why);
    coop::net::ToolActionPayload out = in;
    out.op = T::kRefuse;
    out.owner = slot;
    out.result = 0;
    coop::tool_action::SendToSlot(s, slot, out);
}

void EndBroadcast(coop::net::Session& s, const Mend& m, uint8_t result, int skip) {
    coop::net::ToolActionPayload out = Make(T::kEnd, m);
    out.result = result;
    coop::tool_action::Send(s, out, skip);
}

void HostBegin(coop::net::Session& s, const coop::net::ToolActionPayload& in, uint8_t slot, const std::wstring& key) {
    void* pipe = BP::FindPipe(key);
    if (!pipe || !R::IsLive(pipe)) return Refuse(s, slot, in, "no pipe here holds the key");
    if (!coop::hand_item::HeldClassIs(slot, N::ToolboxClass)) return Refuse(s, slot, in, "the sender holds no repair kit");
    if (ByKey(key)) return Refuse(s, slot, in, "another player is mending it");
    bool damaged = false;
    if (!TB::PipeDamaged(pipe, damaged) || !damaged) return Refuse(s, slot, in, "the pipe is not burst");
    if (in.lives < 1) return Refuse(s, slot, in, "the kit has no use left");
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(pipe);
    if (!subject) return Refuse(s, slot, in, EL::OutcomeName(subject.outcome));
    if (Mend* old = ByOwner(slot)) Erase(old);
    Mend m;
    m.key = key;
    m.pipe.Set(pipe);
    m.owner = slot;
    m.lives = in.lives;
    // The pipe's own fix time bounds the sender's claim; the power tool in the other hand makes it a fifth.
    float pipeTime = 0.f;
    if (TB::PipeFixTime(pipe, pipeTime) && pipeTime > 0.f) {
        const float lo = pipeTime * kPowerToolShare * 0.9f, hi = pipeTime * kTimeSlack;
        m.timeSec = in.stress < lo ? lo : (in.stress > hi ? hi : in.stress);
    } else {
        m.timeSec = in.stress;
    }
    m.beganMs = m.sentMs = m.soundMs = NowMs();
    g_mends.push_back(m);
    coop::tool_action::Send(s, Make(T::kBegin, m), slot);
    UE_LOGI("[TOOLBOX] host: slot %u mends pipe '%ls': %.1f s, %d use(s)", static_cast<unsigned>(slot), key.c_str(),
            m.timeSec, static_cast<int>(m.lives));
}

void HostEnd(coop::net::Session& s, const coop::net::ToolActionPayload& in, uint8_t slot, Mend& mend) {
    const Mend m = mend;
    Erase(&mend);
    if (in.result != T::kDone) {
        EndBroadcast(s, m, T::kCancel, slot);
        return;
    }
    const uint64_t took = NowMs() - m.beganMs;
    const uint64_t least = static_cast<uint64_t>(m.timeSec * 1000.f * kEarlyShare);
    void* pipe = m.pipe.Get();
    bool damaged = false;
    const char* why = nullptr;
    if (took + kSlackMs < least) why = "the mend ended sooner than the pipe takes";
    else if (!pipe) why = "the pipe is gone here";
    else if (!TB::PipeDamaged(pipe, damaged) || !damaged) why = "the pipe is already mended";
    else if (!coop::hand_item::HeldClassIs(slot, N::ToolboxClass)) why = "the host shows no repair kit in the sender's hand";
    if (why) {
        Refuse(s, slot, in, why);
        EndBroadcast(s, m, T::kCancel, slot);
        return;
    }
    // The sender's end goes first: its kit must find the pipe still burst for its own toolboxFix to take the use.
    ++g_ran;
    EndBroadcast(s, m, T::kDone, -1);
    bool fixed = false;
    void* mirror = coop::hand_item::MirrorActorForSlot(slot);
    ++g_applying;
    const bool ran = TB::PipeToolboxFix(pipe, TB::IsToolbox(mirror) ? mirror : nullptr, fixed);
    --g_applying;
    UE_LOGI("[TOOLBOX] host: slot %u's mend of pipe '%ls' after %llu ms -- %s", static_cast<unsigned>(slot), m.key.c_str(),
            static_cast<unsigned long long>(took), (ran && fixed) ? "the pipe is mended" : "the pipe's own fix did not take");
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
    if (!Live(&session) || g_mends.empty()) return;
    const uint64_t now = NowMs();
    const uint8_t self = LocalSlot();
    const bool host = session.role() == coop::net::Role::Host;
    for (size_t i = 0; i < g_mends.size();) {
        Mend& m = g_mends[i];
        void* pipe = m.pipe.Get();
        // HOST: a client's mend ends with its pipe, or with the kit leaving the sender's hand.
        if (host && m.owner != self && (!pipe || !coop::hand_item::HeldClassIs(m.owner, N::ToolboxClass))) {
            EndBroadcast(session, m, T::kCancel, -1);
            g_mends.erase(g_mends.begin() + static_cast<long long>(i));
            continue;
        }
        if (m.owner == self) {
            float complete = 0.f, timeSec = 0.f;
            int32_t fixes = 0;
            void* kit = m.kit.Get();
            if (kit && !m.done && now - m.sentMs >= kProgressMs && TB::ReadFix(kit, complete, timeSec, fixes) &&
                std::fabs(complete - m.complete) > 0.005f) {
                m.complete = complete;
                m.sentMs = now;
                coop::tool_action::Send(session, Make(T::kProgress, m));
            }
        } else if (pipe && now - m.soundMs >= static_cast<uint64_t>(N::ToolboxFixSoundPeriodSec * 1000.f)) {
            m.soundMs = now;
            PlayFixSound(pipe);
        }
        ++i;
    }
}

void OnIntent(coop::net::Session& session, const coop::net::ToolActionPayload& in, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(in.target);
    if (in.op == T::kBegin) return HostBegin(session, in, slot, key);
    Mend* m = ByKey(key);
    if (!m || m->owner != slot) {
        if (in.op == T::kEnd && in.result == T::kDone) Refuse(session, slot, in, "the sender mends nothing there");
        return;
    }
    if (in.op == T::kProgress) {
        const uint64_t now = NowMs();
        if (now - m->sentMs < kProgressFloorMs) return;
        m->sentMs = now;
        m->complete = in.alpha;
        coop::tool_action::Send(session, Make(T::kProgress, *m), slot);
        return;
    }
    if (in.op == T::kEnd) HostEnd(session, in, slot, *m);
}

void OnState(coop::net::Session&, const coop::net::ToolActionPayload& in) {
    const std::wstring key = coop::net::StringFromWireKey(in.target);
    const uint8_t self = LocalSlot();
    switch (in.op) {
    case T::kBegin: {
        if (in.owner == self) return;
        if (Mend* old = ByOwner(in.owner)) Erase(old);
        Mend m;
        m.key = key;
        m.owner = in.owner;
        m.complete = in.alpha;
        m.timeSec = in.stress;
        m.lives = in.lives;
        if (void* p = BP::FindPipe(key)) m.pipe.Set(p);
        m.beganMs = m.sentMs = m.soundMs = NowMs();
        g_mends.push_back(m);
        return;
    }
    case T::kProgress:
        if (Mend* m = ByKey(key); m && m->owner != self) m->complete = in.alpha;
        return;
    case T::kEnd:
    case T::kRefuse: {
        Mend* m = ByKey(key);
        if (!m) return;
        const Mend copy = *m;
        const bool own = copy.owner == self;
        Erase(m);
        if (!own) return;
        Mend local = copy;
        if (in.op == T::kEnd && in.result == T::kDone) {
            RunOwnEnd(local);
        } else {
            ResetOwnKit(local.kit.Get());
            UE_LOGI("[TOOLBOX] client: the %s of the mend of pipe '%ls' -- the kit stops", in.op == T::kRefuse ? "host's refusal" : "host's cancel",
                    key.c_str());
        }
        return;
    }
    default: return;
    }
}

void ConnectReplayForSlot(coop::net::Session& session, int slot) {
    for (const Mend& m : g_mends) coop::tool_action::SendToSlot(session, slot, Make(T::kBegin, m));
    if (!g_mends.empty()) UE_LOGI("[TOOLBOX] host: %zu running mend(s) to slot %d's join", g_mends.size(), slot);
}

void OnPeerLeft(coop::net::Session& session, uint8_t slot) {
    for (size_t i = 0; i < g_mends.size();) {
        if (g_mends[i].owner != slot) { ++i; continue; }
        EndBroadcast(session, g_mends[i], T::kCancel, slot);
        g_mends.erase(g_mends.begin() + static_cast<long long>(i));
    }
}

void OnDisconnect() {
    if (g_sent || g_ran || g_refused)
        UE_LOGI("[TOOLBOX] session end -- sent=%llu host-run=%llu refused=%llu", static_cast<unsigned long long>(g_sent),
                static_cast<unsigned long long>(g_ran), static_cast<unsigned long long>(g_refused));
    g_sent = g_ran = g_refused = g_unkeyed = 0;
    g_mends.clear();
    g_applying = 0;
}

}  // namespace coop::toolbox_sync
