// coop/interactables/pry_sync.cpp -- see coop/interactables/pry_sync.h.

#include "coop/interactables/pry_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/element/portable_identity.h"
#include "coop/interactables/interactable_sync.h"  // a pryable door's key
#include "coop/interactables/tool_action.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/devices/pryable.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::pry_sync {
namespace {

namespace EL = coop::element;
namespace N  = ue_wrap::profile::name;
namespace PY = ue_wrap::pryable;
namespace R  = ue_wrap::reflection;
namespace T  = coop::net::tool_action;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowBegin, "pry_sync", L"pryingCrowbar_C", L"begin", Declared);
UE_FACT_FN(kRowEnd, "pry_sync", L"pryingCrowbar_C", L"end", Declared);
UE_FACT_FN(kRowSnap, "pry_sync", L"pryingCrowbar_C", L"snap", Declared);
constexpr int kTagBegin = 0x50525942;  // 'PRYB'
constexpr int kTagEnd = 0x50525945;
constexpr int kTagSnap = 0x50525953;

// A crowbar goes in where the player stands, so the bound is the look-at trace's (200 uu, twice over) and the
// authority's pose allowance; a pry is never judged closer than a door verb is.
constexpr float    kReachUU = 400.0f;
constexpr uint64_t kProgressMs = 100;        // the owner's bend, at most ten a second
constexpr uint64_t kProgressFloorMs = 40;    // HOST: a client's bend faster than this is dropped
constexpr uint64_t kDoneMinMs = 250;         // a pry finished sooner than this after its begin is not one
constexpr uint64_t kIndexRetryMs = 1000;     // a key no child answers rebuilds the index at most this often

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[3] = {};
bool g_settled = false;
int  g_applying = 0;  // this lane's own end/snap/show on a child: its watches let it run and send nothing

uint64_t g_sent = 0, g_shown = 0, g_ran = 0, g_refused = 0, g_unnamed = 0;

struct Pry {
    std::wstring           key;
    ue_wrap::CachedObjRef  child;
    uint8_t                owner = 0;
    PY::State              state;          // what this peer last sent (owner) or showed (receiver)
    uint64_t               sentMs = 0;
    uint64_t               beganMs = 0;
};
std::vector<Pry> g_pries;

uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }
uint64_t NowMs() { return coop::net::NowMs(); }

Pry* ByKey(const std::wstring& key) {
    for (Pry& p : g_pries)
        if (p.key == key) return &p;
    return nullptr;
}
Pry* ByChild(void* child) {
    for (Pry& p : g_pries)
        if (p.child.Is(child)) return &p;
    return nullptr;
}
void Erase(const Pry* which) {
    for (size_t i = 0; i < g_pries.size(); ++i)
        if (&g_pries[i] == which) { g_pries.erase(g_pries.begin() + static_cast<long long>(i)); return; }
}

// ---- the child's name on the wire ---------------------------------------------------------------------------------
// A pryable door is named by the door lane's own key (its game key is minted per process); anything else by the
// portable identity, its pryable's key or level name and the child's component.
std::wstring KeyOf(void* child) {
    std::wstring comp;
    void* parent = ue_wrap::engine::ParentActorOf(child, &comp);
    if (parent && ue_wrap::door::IsDoor(parent) && !comp.empty()) {
        const std::wstring door = coop::interactable_sync::DoorKey(parent);
        if (!door.empty()) {
            wchar_t buf[24];
            std::swprintf(buf, 24, L"md_%016llx",
                          static_cast<unsigned long long>(EL::IdentityHash(L"d:" + door + L"/" + comp)));
            return buf;
        }
    }
    return EL::PortableWireKey(child);
}

std::unordered_map<std::wstring, ue_wrap::CachedObjRef> g_index;
uint64_t g_indexBuiltMs = 0;

void IndexOne(void*, void* child) {
    const std::wstring key = KeyOf(child);
    if (!key.empty()) g_index[key].Set(child);
}

void* Resolve(const std::wstring& key) {
    auto it = g_index.find(key);
    if (it != g_index.end())
        if (void* c = it->second.Get()) return c;
    const uint64_t now = NowMs();
    if (g_indexBuiltMs != 0 && now - g_indexBuiltMs < kIndexRetryMs) return nullptr;
    g_indexBuiltMs = now;
    g_index.clear();
    PY::ForEachPryingCrowbar(&IndexOne, nullptr);
    it = g_index.find(key);
    return it != g_index.end() ? it->second.Get() : nullptr;
}

// ---- the payload -------------------------------------------------------------------------------------------------
coop::net::ToolActionPayload Make(uint8_t op, const std::wstring& key, uint8_t owner, const PY::State& st) {
    coop::net::ToolActionPayload p{};
    p.kind = T::kPry;
    p.op = op;
    p.owner = owner;
    coop::net::WireKeyFromString(key, p.target);
    p.pivot[0] = st.pivot.X; p.pivot[1] = st.pivot.Y; p.pivot[2] = st.pivot.Z;
    p.alpha = st.alpha;
    p.stress = st.stress;
    p.pitch = st.pitch;
    p.angle = st.angle;
    p.lives = st.lives;
    p.flags = static_cast<uint8_t>((st.straining ? T::kStraining : 0) | (st.strainSound ? T::kStrainSound : 0) |
                                   (st.sparked ? T::kSparked : 0));
    return p;
}

PY::State StateOf(const coop::net::ToolActionPayload& p) {
    PY::State st;
    st.pivot = {p.pivot[0], p.pivot[1], p.pivot[2]};
    st.alpha = p.alpha;
    st.stress = p.stress;
    st.pitch = p.pitch;
    st.angle = p.angle;
    st.lives = p.lives;
    st.stuck = true;
    st.straining = (p.flags & T::kStraining) != 0;
    st.strainSound = (p.flags & T::kStrainSound) != 0;
    st.sparked = (p.flags & T::kSparked) != 0;
    return st;
}

bool Moved(const PY::State& a, const PY::State& b) {
    return std::fabs(a.alpha - b.alpha) > 0.005f || std::fabs(a.pitch - b.pitch) > 0.05f ||
           std::fabs(a.angle - b.angle) > 0.05f || std::fabs(a.stress - b.stress) > 0.01f ||
           a.straining != b.straining || a.strainSound != b.strainSound || a.sparked != b.sparked;
}

bool Live(coop::net::Session* s) { return s && s->running() && s->connected(); }

const char* ResultName(uint8_t r) {
    switch (r) {
    case T::kCancel: return "taken back";
    case T::kDone: return "pried open";
    case T::kSnap: return "snapped";
    case T::kDropped: return "dropped";
    default: return "?";
    }
}

// ---- a receiver's copy -------------------------------------------------------------------------------------------
void Show(void* child, const PY::State& st) {
    ++g_applying;
    PY::ShowStuck(child, st.pivot, st.lives);
    PY::ShowProgress(child, st, PY::State{});
    --g_applying;
    ++g_shown;
}

// Hide a copy: the game's end with nothing given back and no player's input touched.
void Hide(void* child) {
    ++g_applying;
    PY::CallEnd(child, false, false, true, nullptr, false);
    --g_applying;
}

// The local player's own crowbar back into its hand: the take-back end on its own pry.
void TakeBack(void* child) {
    ++g_applying;
    PY::CallEnd(child, false, false, false, coop::players::Registry::Get().Local(), true);
    --g_applying;
}

// ---- the owner's watches -----------------------------------------------------------------------------------------
R::ParamOffset g_beginPlayer{L"mainPlayer"};

void OnBeginPost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s) || !PY::IsPryingCrowbar(c.object)) return;
    const int32_t off = g_beginPlayer.Of(c.function);
    void* player = (off >= 0 && c.locals) ? *reinterpret_cast<void* const*>(c.locals + off) : nullptr;
    if (!player || !coop::players::Registry::Get().IsLocal(player)) return;
    PY::State st;
    if (!PY::Read(c.object, st) || !st.stuck) return;  // the begin failed: no crowbar went in
    const std::wstring key = KeyOf(c.object);
    if (key.empty() || key.size() > sizeof(coop::net::WireKey::data)) {
        if (++g_unnamed <= 3)
            UE_LOGW("[PRY] a crowbar went into %ls, which has no name both peers know -- the pry stays on this screen",
                    R::ClassNameOf(PY::ParentOf(c.object)).c_str());
        return;
    }
    if (Pry* old = ByKey(key)) Erase(old);
    Pry p;
    p.key = key;
    p.child.Set(c.object);
    p.owner = LocalSlot();
    p.state = st;
    p.sentMs = p.beganMs = NowMs();
    g_pries.push_back(p);
    coop::tool_action::Send(*s, Make(T::kBegin, key, p.owner, st));
    ++g_sent;
    UE_LOGI("[PRY] %s: a crowbar went into key='%ls' (%ls), %u lives", s->role() == coop::net::Role::Host ? "host" : "client",
            key.c_str(), R::ClassNameOf(PY::ParentOf(c.object)).c_str(), static_cast<unsigned>(st.lives));
}

struct EndOffsets { void* fn = nullptr; PY::EndParams p; };
EndOffsets g_endOff;

sg::Verdict OnEndPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s) || !c.locals) return sg::Verdict::Run;
    Pry* p = ByChild(c.object);
    if (!p) return sg::Verdict::Run;
    // A copy's end runs from this lane alone: no local verb ends another player's pry.
    if (p->owner != LocalSlot()) return sg::Verdict::Cancel;
    if (g_endOff.fn != c.function) { g_endOff.fn = c.function; g_endOff.p = PY::EndParamsOf(c.function); }
    const PY::EndParams& o = g_endOff.p;
    if (o.pryOpen < 0 || o.dropSelf < 0 || o.destroyed < 0) return sg::Verdict::Run;
    uint8_t* pry = c.locals + o.pryOpen;
    uint8_t* drop = c.locals + o.dropSelf;
    uint8_t* destroyed = c.locals + o.destroyed;
    const uint8_t result = *pry ? T::kDone : (*destroyed ? T::kSnap : (*drop ? T::kDropped : T::kCancel));
    PY::State st = p->state;
    PY::Read(c.object, st);
    // A client's pry and dropped crowbar change the world: the host runs them on its copy, and here the end only
    // tidies (nothing given back, no crowbarOpen). The crowbar taken back is this player's own and runs here.
    if (s->role() == coop::net::Role::Client && (result == T::kDone || result == T::kDropped)) {
        *pry = 0;
        *drop = 0;
        *destroyed = 1;
    }
    coop::net::ToolActionPayload out = Make(T::kEnd, p->key, p->owner, st);
    out.result = result;
    coop::tool_action::Send(*s, out);
    ++g_sent;
    UE_LOGI("[PRY] %s: the crowbar in key='%ls' %s", s->role() == coop::net::Role::Host ? "host" : "client",
            p->key.c_str(), ResultName(result));
    Erase(p);
    return sg::Verdict::Run;
}

sg::Verdict OnSnapPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_applying > 0 || !Live(s)) return sg::Verdict::Run;
    Pry* p = ByChild(c.object);
    if (!p) return sg::Verdict::Run;
    if (p->owner != LocalSlot()) return sg::Verdict::Cancel;
    // The host's own snap runs whole: its end names the snap.
    if (s->role() == coop::net::Role::Host) return sg::Verdict::Run;
    // A client's snap spawns a crowbar or its gibs: the host's, from the lives this pry began with.
    PY::State st = p->state;
    PY::Read(c.object, st);
    coop::net::ToolActionPayload out = Make(T::kEnd, p->key, p->owner, st);
    out.result = T::kSnap;
    coop::tool_action::Send(*s, out);
    ++g_sent;
    UE_LOGI("[PRY] client: the crowbar in key='%ls' snapped -- the host breaks it", p->key.c_str());
    void* child = c.object;
    Erase(p);
    ++g_applying;
    PY::CallEnd(child, false, false, true, coop::players::Registry::Get().Local(), true);
    --g_applying;
    return sg::Verdict::Cancel;
}

void RegisterWatches() {
    if (g_reg[0] == Reg::Pending) g_reg[0] = sg::WatchName(kRowBegin, kTagBegin, nullptr, &OnBeginPost) ? Reg::Registered : Reg::Refused;
    if (g_reg[1] == Reg::Pending) g_reg[1] = sg::WatchName(kRowEnd, kTagEnd, &OnEndPre, nullptr) ? Reg::Registered : Reg::Refused;
    if (g_reg[2] == Reg::Pending) g_reg[2] = sg::WatchName(kRowSnap, kTagSnap, &OnSnapPre, nullptr) ? Reg::Registered : Reg::Refused;
}

void Settle() {
    if (g_settled) return;
    // Nothing is judged before the watches are registered (Tick can run ahead of Install): none pending then
    // reads as every watch dead.
    if (g_reg[0] == Reg::Pending || g_reg[1] == Reg::Pending || g_reg[2] == Reg::Pending) return;
    const bool live = g_reg[0] == Reg::Registered && g_reg[1] == Reg::Registered && g_reg[2] == Reg::Registered &&
                      sg::NameWatchLive(kRowBegin, kTagBegin) && sg::NameWatchLive(kRowEnd, kTagEnd) &&
                      sg::NameWatchLive(kRowSnap, kTagSnap);
    if (live) {
        g_settled = true;
        UE_LOGI("[PRY] the prying crowbar's begin, end and snap watches are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[PRY] a prying crowbar watch is dead (refused, or the class declares no such body) -- a pry is seen "
                "only where it is made");
    }
}

// ---- host: the judge ---------------------------------------------------------------------------------------------
void Refuse(coop::net::Session& s, uint8_t slot, const coop::net::ToolActionPayload& in, const char* why) {
    ++g_refused;
    UE_LOGI("[PRY] host: REFUSE slot %u's %s on key='%ls' -- %s", static_cast<unsigned>(slot),
            in.op == T::kBegin ? "crowbar" : "end", coop::net::StringFromWireKey(in.target).c_str(), why);
    if (in.op != T::kBegin) return;
    coop::net::ToolActionPayload out = in;
    out.op = T::kRefuse;
    out.owner = slot;
    out.result = 0;
    coop::tool_action::SendToSlot(s, slot, out);
}

void HostBegin(coop::net::Session& s, const coop::net::ToolActionPayload& in, uint8_t slot, const std::wstring& key) {
    void* child = Resolve(key);
    if (!child) return Refuse(s, slot, in, "no prying crowbar here answers the key");
    if (ByKey(key)) return Refuse(s, slot, in, "another player's crowbar is in it");
    PY::State here;
    if (!PY::Read(child, here) || here.stuck) return Refuse(s, slot, in, "a crowbar is already in it here");
    if (in.lives < 1 || in.lives > N::CrowbarMaxLives) return Refuse(s, slot, in, "the lives are out of range");
    if (!coop::hand_item::LastHeldClassIs(slot, N::CrowbarClass))
        return Refuse(s, slot, in, "the sender's hand did not hold a crowbar last");
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(child);
    if (!subject) return Refuse(s, slot, in, EL::OutcomeName(subject.outcome));
    Pry p;
    p.key = key;
    p.child.Set(child);
    p.owner = slot;
    p.state = StateOf(in);
    p.sentMs = p.beganMs = NowMs();
    Show(child, p.state);
    g_pries.push_back(p);
    coop::net::ToolActionPayload out = in;
    out.owner = slot;
    coop::tool_action::Send(s, out, slot);
    UE_LOGI("[PRY] host: slot %u's crowbar is in key='%ls' (%ls)", static_cast<unsigned>(slot), key.c_str(),
            R::ClassNameOf(PY::ParentOf(child)).c_str());
}

void HostEnd(coop::net::Session& s, const coop::net::ToolActionPayload& in, uint8_t slot, Pry& p) {
    void* child = p.child.Get();
    uint8_t result = in.result;
    // The sender's own screen has already tidied its crowbar away, so an end the host does not believe still gives
    // the crowbar back, to the floor, rather than losing it.
    if (result == T::kDone && (in.alpha < 0.95f || NowMs() - p.beganMs < kDoneMinMs)) {
        UE_LOGW("[PRY] host: slot %u's pry of key='%ls' at alpha %.2f after %llu ms is not one -- its crowbar drops",
                static_cast<unsigned>(slot), p.key.c_str(), in.alpha, static_cast<unsigned long long>(NowMs() - p.beganMs));
        result = T::kDropped;
    } else if (result == T::kSnap && in.stress < 0.95f) {
        UE_LOGW("[PRY] host: slot %u's snap of key='%ls' at strain %.2f is not one -- its crowbar drops",
                static_cast<unsigned>(slot), p.key.c_str(), in.stress);
        result = T::kDropped;
    }
    const uint8_t lives = p.state.lives;
    const std::wstring key = p.key;
    Erase(&p);
    if (child) {
        ++g_applying;
        switch (result) {
        case T::kDone:    PY::CallEnd(child, true, false, false, nullptr, false); break;
        case T::kSnap:    PY::CallSnap(child, lives); break;
        case T::kDropped: PY::CallEnd(child, false, true, false, nullptr, false); break;
        default:          PY::CallEnd(child, false, false, true, nullptr, false); break;
        }
        --g_applying;
        ++g_ran;
    }
    coop::net::ToolActionPayload out = in;
    out.owner = slot;
    out.result = result;
    coop::tool_action::Send(s, out, slot);
    UE_LOGI("[PRY] host: slot %u's crowbar in key='%ls' %s%s", static_cast<unsigned>(slot), key.c_str(),
            ResultName(result), child ? "" : " (its child is gone here)");
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
    if (!Live(&session) || g_pries.empty()) return;
    const uint64_t now = NowMs();
    const uint8_t self = LocalSlot();
    for (size_t i = 0; i < g_pries.size();) {
        Pry& p = g_pries[i];
        void* child = p.child.Get();
        if (!child) {
            // The pryable went (a destroy, a world change): the session ends with it, and its owner's crowbar too.
            if (session.role() == coop::net::Role::Host || p.owner == self) {
                coop::net::ToolActionPayload out = Make(T::kEnd, p.key, p.owner, p.state);
                out.result = T::kCancel;
                if (session.role() == coop::net::Role::Host) coop::tool_action::Send(session, out);
            }
            g_pries.erase(g_pries.begin() + static_cast<long long>(i));
            continue;
        }
        if (p.owner == self && now - p.sentMs >= kProgressMs) {
            PY::State st;
            if (PY::Read(child, st) && Moved(st, p.state)) {
                p.state = st;
                p.sentMs = now;
                coop::tool_action::Send(session, Make(T::kProgress, p.key, p.owner, st));
            }
        }
        ++i;
    }
}

void OnIntent(coop::net::Session& session, const coop::net::ToolActionPayload& in, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(in.target);
    if (in.op == T::kBegin) return HostBegin(session, in, slot, key);
    Pry* p = ByKey(key);
    if (!p || p->owner != slot) {
        if (in.op == T::kEnd) Refuse(session, slot, in, "the sender has no crowbar in it");
        return;
    }
    if (in.op == T::kProgress) {
        const uint64_t now = NowMs();
        if (now - p->sentMs < kProgressFloorMs) return;
        void* child = p->child.Get();
        if (!child) return;
        const PY::State st = StateOf(in);
        ++g_applying;
        PY::ShowProgress(child, st, p->state);
        --g_applying;
        p->state = st;
        p->sentMs = now;
        coop::net::ToolActionPayload out = in;
        out.owner = slot;
        coop::tool_action::Send(session, out, slot);
        return;
    }
    if (in.op == T::kEnd) HostEnd(session, in, slot, *p);
}

void OnState(coop::net::Session&, const coop::net::ToolActionPayload& in) {
    const std::wstring key = coop::net::StringFromWireKey(in.target);
    const uint8_t self = LocalSlot();
    Pry* p = ByKey(key);
    switch (in.op) {
    case T::kBegin: {
        if (in.owner == self) return;
        void* child = Resolve(key);
        if (!child) {
            UE_LOGW("[PRY] client: slot %u's crowbar is in key='%ls', which no prying crowbar here answers",
                    static_cast<unsigned>(in.owner), key.c_str());
            return;
        }
        // Both players put a crowbar in at once and the host took the other's: this one comes back to the hand
        // before the other's is shown.
        if (p && p->owner == self) {
            Erase(p);
            TakeBack(child);
            UE_LOGI("[PRY] client: slot %u's crowbar went into key='%ls' first -- this player's comes back",
                    static_cast<unsigned>(in.owner), key.c_str());
        } else if (p) {
            Erase(p);
        }
        Pry np;
        np.key = key;
        np.child.Set(child);
        np.owner = in.owner;
        np.state = StateOf(in);
        np.beganMs = np.sentMs = NowMs();
        Show(child, np.state);
        g_pries.push_back(np);
        return;
    }
    case T::kProgress: {
        if (!p || p->owner == self) return;
        void* child = p->child.Get();
        if (!child) return;
        const PY::State st = StateOf(in);
        ++g_applying;
        PY::ShowProgress(child, st, p->state);
        --g_applying;
        p->state = st;
        return;
    }
    case T::kEnd: {
        if (!p || p->owner == self) return;
        void* child = p->child.Get();
        Erase(p);
        if (child) Hide(child);
        return;
    }
    case T::kRefuse: {
        if (!p || p->owner != self) return;
        void* child = p->child.Get();
        Erase(p);
        if (child) TakeBack(child);
        UE_LOGI("[PRY] client: the host refused the crowbar in key='%ls' -- it comes back to the hand", key.c_str());
        return;
    }
    default: return;
    }
}

void ConnectReplayForSlot(coop::net::Session& session, int slot) {
    for (const Pry& p : g_pries) {
        coop::tool_action::SendToSlot(session, slot, Make(T::kBegin, p.key, p.owner, p.state));
    }
    if (!g_pries.empty())
        UE_LOGI("[PRY] host: %zu stuck crowbars to slot %d's join", g_pries.size(), slot);
}

void OnPeerLeft(coop::net::Session& session, uint8_t slot) {
    for (size_t i = 0; i < g_pries.size();) {
        if (g_pries[i].owner != slot) { ++i; continue; }
        const Pry p = g_pries[i];
        g_pries.erase(g_pries.begin() + static_cast<long long>(i));
        if (void* child = p.child.Get()) {
            ++g_applying;
            PY::CallEnd(child, false, true, false, nullptr, false);
            --g_applying;
        }
        coop::net::ToolActionPayload out = Make(T::kEnd, p.key, p.owner, p.state);
        out.result = T::kDropped;
        coop::tool_action::Send(session, out, slot);
        UE_LOGI("[PRY] host: slot %u left with its crowbar in key='%ls' -- it drops", static_cast<unsigned>(slot),
                p.key.c_str());
    }
}

void OnDisconnect() {
    if (g_sent || g_shown || g_ran || g_refused)
        UE_LOGI("[PRY] session end -- sent=%llu shown=%llu host-run=%llu refused=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_shown),
                static_cast<unsigned long long>(g_ran), static_cast<unsigned long long>(g_refused));
    g_sent = g_shown = g_ran = g_refused = g_unnamed = 0;
    g_pries.clear();
    g_index.clear();
    g_indexBuiltMs = 0;
    g_applying = 0;
}

}  // namespace coop::pry_sync
