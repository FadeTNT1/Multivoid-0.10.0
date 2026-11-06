// coop/items/poster_sync.cpp -- see coop/items/poster_sync.h.

#include "coop/items/poster_sync.h"

#include "poster_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_hold.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_record_gates.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/prop_synth_key.h"
#include "coop/text/utf8_codec.h"

#include "ue_wrap/actors/poster.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::poster_sync {
namespace {

namespace EL = coop::element;
namespace E  = ue_wrap::engine;
namespace OP = coop::net::poster_intent_ops;
namespace SO = coop::net::poster_state_ops;
namespace SF = coop::net::poster_state_flags;
namespace P  = ue_wrap::profile;
namespace PO = ue_wrap::poster;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using detail::FileNameFromWire;
using detail::FileNameToWire;
using detail::KeyFits;
using detail::SayFallback;

UE_FACT_FN(kRowPut, "poster_sync", L"prop_poster_C", L"put", Declared);
UE_FACT_FN(kRowRolledPicker, "poster_sync", L"prop_poster_C", L"intComs_texturePickerApply", Declared);
UE_FACT_FN(kRowPlacedAction, "poster_sync", L"poster_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowPlacedPicker, "poster_sync", L"poster_C", L"intComs_texturePickerApply", Declared);
UE_FACT_FN(kRowSetPoster, "poster_sync", L"poster_C", L"setPoster", Declared);
constexpr int kTagPut = 0x50535431;   // 'PST1'
constexpr int kTagRolledPicker = 0x50535432;
constexpr int kTagPlacedAction = 0x50535433;
constexpr int kTagPlacedPicker = 0x50535434;
constexpr int kTagSetPoster = 0x50535435;

struct Watch {
    const ue_wrap::facts::Function* row;
    int                             tag;
    const char*                     what;
};
const Watch kWatches[] = {
    {&kRowPut, kTagPut, "prop_poster_C.put"},
    {&kRowRolledPicker, kTagRolledPicker, "prop_poster_C.intComs_texturePickerApply"},
    {&kRowPlacedAction, kTagPlacedAction, "poster_C.actionOptionIndex"},
    {&kRowPlacedPicker, kTagPlacedPicker, "poster_C.intComs_texturePickerApply"},
    {&kRowSetPoster, kTagSetPoster, "poster_C.setPoster"},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;
bool g_gatesRegistered = false;

constexpr float    kReachUU = 450.0f;
constexpr float    kMaxScale = 4.0f;
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t   kMaxPending = 8;
constexpr uint64_t kSayMs = 10000;
constexpr uint64_t kParkMs = 30000;
constexpr size_t   kMaxPlaced = 512;

std::atomic<coop::net::Session*> g_session{nullptr};

R::ParamOffset g_actionParam{L"action"};
R::ParamOffset g_playerParam{L"player"};
R::ParamOffset g_pickedParam{P::name::PosterPickedFileNameParam};

uint64_t g_sent = 0, g_ran = 0, g_denied = 0, g_dropped = 0, g_published = 0, g_removed = 0, g_fellBack = 0,
         g_records = 0;
bool g_fullSaid = false, g_placeSaid = false, g_fallbackSaid = false;

uint64_t NowMs() { return coop::net::NowMs(); }

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}
bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }

bool Finite(float v) { return std::isfinite(v); }

// ---- host: the placed posters -------------------------------------------------------------------------------------
struct HostRow {
    ue_wrap::CachedObjRef actor;
    std::wstring          key;
    bool                  dirty = true;
    bool                  sent = false;   // a state left for this row since it was last changed
    PO::Look              look;
    PO::Placement         at;
    std::wstring          cls;
};
std::vector<HostRow> g_host;
bool g_scanned = false;

HostRow* FindHost(void* w) {
    for (HostRow& r : g_host)
        if (r.actor.Is(w)) return &r;
    return nullptr;
}

HostRow* FindHostByKey(const std::wstring& key) {
    for (HostRow& r : g_host)
        if (r.key == key && r.actor.Alive()) return &r;
    return nullptr;
}

HostRow* NoteHost(void* w) {
    if (!PO::IsPlaced(w)) return nullptr;
    if (HostRow* r = FindHost(w)) {
        r->dirty = true;
        return r;
    }
    if (g_host.size() >= kMaxPlaced) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("[POSTER] host: %zu placed posters noted already -- a further one is not published (said once)",
                    kMaxPlaced);
        }
        return nullptr;
    }
    HostRow r;
    r.actor.Set(w);
    g_host.push_back(std::move(r));
    return &g_host.back();
}

// The key a placed poster is named by. One it has is kept; one without gets a random key, written into the actor so
// the host's save and a joiner's load carry it.
bool EnsureKey(HostRow& r, void* w) {
    if (!r.key.empty()) return true;
    std::wstring key = PO::KeyOf(w);
    if (key.empty()) {
        const std::wstring fresh = coop::prop_synth_key::RandomKeyString();
        if (!PO::SetKey(w, fresh)) return false;
        key = PO::KeyOf(w);
        if (key.empty()) return false;
        UE_LOGI("[POSTER] host: minted key='%ls' for a keyless placed poster", key.c_str());
    }
    if (!KeyFits(key)) {
        UE_LOGW("[POSTER] host: a placed poster's key '%ls' does not fit the wire -- not published", key.c_str());
        return false;
    }
    r.key = std::move(key);
    return true;
}

bool FillPlaced(const HostRow& r, bool snapshot, coop::net::PosterStatePayload& p) {
    if (r.cls.empty() || r.cls.size() > 31) return false;
    p = coop::net::PosterStatePayload{};
    p.op = SO::kPlaced;
    p.flags = static_cast<uint8_t>((r.look.custom ? SF::kCustom : 0) | (snapshot ? SF::kSnapshot : 0));
    p.type = r.look.type;
    p.index = r.look.index;
    coop::net::WireKeyFromString(r.key, p.key);
    coop::net::WireKeyFromString(r.cls, p.cls);
    p.locX = r.at.location.X;
    p.locY = r.at.location.Y;
    p.locZ = r.at.location.Z;
    p.pitch = r.at.rotation.Pitch;
    p.yaw = r.at.rotation.Yaw;
    p.roll = r.at.rotation.Roll;
    p.scaleX = r.at.scale.X;
    p.scaleY = r.at.scale.Y;
    p.scaleZ = r.at.scale.Z;
    if (!FileNameToWire(r.look.fileName, p.fileName)) {
        UE_LOGW("[POSTER] host: key='%ls' file name '%ls' is past the wire's 95 bytes -- sent without it",
                r.key.c_str(), r.look.fileName.c_str());
        std::memset(&p.fileName, 0, sizeof(p.fileName));
    }
    return true;
}

// Read a row's actor into it; false when a read failed (the row is retried next tick).
bool Capture(HostRow& r, void* w) {
    PO::Look look;
    PO::Placement at;
    if (!PO::ReadLook(w, look) || !PO::ReadTransform(w, at)) return false;
    r.look = std::move(look);
    r.at = at;
    r.cls = R::ClassNameOf(w);
    return true;
}

void SendRemoved(coop::net::Session& s, const std::wstring& key) {
    coop::net::PosterStatePayload p{};
    p.op = SO::kRemoved;
    coop::net::WireKeyFromString(key, p.key);
    if (s.SendReliable(coop::net::ReliableKind::PosterState, &p, sizeof(p))) {
        ++g_removed;
        UE_LOGI("[POSTER] host: key='%ls' is gone -- removed everywhere", key.c_str());
    }
}

void PumpHost(coop::net::Session& s) {
    if (!g_scanned) {
        g_scanned = true;
        int n = 0;
        for (void* w : PO::AllPlaced())
            if (NoteHost(w)) ++n;
        if (n) UE_LOGI("[POSTER] host: %d placed poster(s) in the world at the session's start", n);
    }
    const bool anyReady = s.connected() && s.AnyWorldReadyPeer();
    for (size_t i = 0; i < g_host.size();) {
        HostRow& r = g_host[i];
        void* w = r.actor.Get();
        if (!w) {
            if (!r.key.empty() && s.connected()) SendRemoved(s, r.key);
            g_host.erase(g_host.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
        if (!r.dirty) continue;
        if (!EnsureKey(r, w)) {
            r.dirty = false;
            continue;
        }
        const PO::Look was = r.look;
        const PO::Placement wasAt = r.at;
        const std::wstring wasCls = r.cls;
        if (!Capture(r, w)) continue;   // a read that failed this tick is tried again
        r.dirty = false;
        const bool changed = !r.sent || r.look != was || r.cls != wasCls || r.at.location.X != wasAt.location.X ||
                             r.at.location.Y != wasAt.location.Y || r.at.location.Z != wasAt.location.Z ||
                             r.at.rotation.Pitch != wasAt.rotation.Pitch || r.at.rotation.Yaw != wasAt.rotation.Yaw ||
                             r.at.rotation.Roll != wasAt.rotation.Roll || r.at.scale.X != wasAt.scale.X ||
                             r.at.scale.Y != wasAt.scale.Y || r.at.scale.Z != wasAt.scale.Z;
        if (!changed) continue;
        // Nobody in the world to tell: a joiner gets it in its snapshot.
        if (!anyReady) {
            r.sent = true;
            continue;
        }
        coop::net::PosterStatePayload p{};
        if (!FillPlaced(r, false, p)) continue;
        if (!s.SendReliable(coop::net::ReliableKind::PosterState, &p, sizeof(p))) {
            r.dirty = true;   // the transport refused: next tick
            continue;
        }
        r.sent = true;
        ++g_published;
        UE_LOGI("[POSTER] host: key='%ls' %ls type=%d file='%ls'%s at (%.0f,%.0f,%.0f) scale (%.2f,%.2f,%.2f)",
                r.key.c_str(), r.cls.c_str(), r.look.type, r.look.fileName.c_str(), r.look.custom ? " custom" : "",
                static_cast<double>(r.at.location.X), static_cast<double>(r.at.location.Y),
                static_cast<double>(r.at.location.Z), static_cast<double>(r.at.scale.X),
                static_cast<double>(r.at.scale.Y), static_cast<double>(r.at.scale.Z));
    }
}

// ---- host: the intents --------------------------------------------------------------------------------------------
struct Slot {
    coop::net::IntentQueue<coop::net::PosterIntentPayload> queue;
    bool     waitSaid = false;
    uint64_t nextSayMs = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

const char* OpName(uint8_t op) {
    switch (op) {
        case OP::kPut: return "put";
        case OP::kTakeDown: return "take-down";
        case OP::kPicture: return "picture";
        default: return "?";
    }
}

void Deny(uint8_t slot, uint8_t op, const std::wstring& key, const char* why) {
    ++g_denied;
    Slot& sl = g_slots[slot];
    const uint64_t now = NowMs();
    if (now < sl.nextSayMs) return;
    sl.nextSayMs = now + kSayMs;
    UE_LOGI("[POSTER] host: DENY slot %u's %s on key='%ls' -- %s", static_cast<unsigned>(slot), OpName(op),
            key.c_str(), why);
}

// The placed poster a key names: the noted row's, or a scan's (a poster the setPoster watch has not seen yet).
void* PlacedByKey(const std::wstring& key) {
    if (HostRow* r = FindHostByKey(key)) return r->actor.Get();
    for (void* w : PO::AllPlaced())
        if (PO::KeyOf(w) == key) {
            if (HostRow* r = NoteHost(w)) r->key = key;
            return w;
        }
    return nullptr;
}

void* SenderBody(uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return (rp && rp->valid()) ? rp->GetActor() : nullptr;
}

// The intent's verdict: true when done (run or refused), false to wait for the sender's body.
bool Execute(coop::net::Session& s, const coop::net::PosterIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* target = p.op == OP::kPut ? coop::prop_key_index::Find(key) : PlacedByKey(key);
    if (p.op == OP::kPut ? !PO::IsRolled(target) : !PO::IsPlaced(target)) {
        Deny(slot, p.op, key, p.op == OP::kPut ? "no rolled poster holds the key" : "no placed poster holds the key");
        return true;
    }
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        Deny(slot, p.op, key, "the sender's player is not alive");
        return true;
    }
    const EL::IntentTarget reach = EL::IntentTarget::ForClientIntent(s, slot, kReachUU);
    const EL::IntentSubject subject = reach.Authorize(target);
    if (subject.outcome == EL::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[POSTER] host: slot %u's poster verbs wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        Deny(slot, p.op, key, EL::OutcomeName(subject.outcome));
        return true;
    }

    if (p.op == OP::kPut) {
        if (const coop::prop_hold::Holder h = coop::prop_hold::WhoHolds(target, slot);
            h != coop::prop_hold::Holder::None) {
            Deny(slot, p.op, key, coop::prop_hold::HolderWhy(h));
            return true;
        }
        PO::Placement at;
        at.location = {p.locX, p.locY, p.locZ};
        at.rotation = {p.pitch, p.yaw, p.roll};
        at.scale = {p.scaleX, p.scaleY, p.scaleZ};
        const EL::IntentSubject place = reach.AuthorizeSegment(at.location, at.location);
        if (!place) {
            Deny(slot, p.op, key, "the placement is out of the sender's reach");
            return true;
        }
        void* cls = PO::PosterClassOf(target);
        PO::Look look;
        if (!PO::IsPlacedClass(cls) || !PO::ReadLook(target, look)) {
            Deny(slot, p.op, key, "the rolled poster names no placed class or its look did not read");
            return true;
        }
        void* w = PO::SpawnPlaced(cls, at, look, false, coop::prop_synth_key::RandomKeyString());
        if (!w) {
            Deny(slot, p.op, key, "the placed poster did not spawn");
            return true;
        }
        E::DestroyActor(target);   // the rolled prop: the prop lane carries its destroy, the sender's hand included
        NoteHost(w);
        ++g_ran;
        UE_LOGI("[POSTER] host: put slot %u's rolled poster key='%ls' on the wall as %ls (#%llu)",
                static_cast<unsigned>(slot), key.c_str(), R::ClassNameOf(w).c_str(),
                static_cast<unsigned long long>(g_ran));
        return true;
    }

    if (p.op == OP::kTakeDown) {
        void* cls = PO::PosterClassOf(target);
        PO::Look look;
        void* body = SenderBody(slot);
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (!PO::IsRolledClass(cls) || !PO::ReadLook(target, look) || !body || !E::TryGetActorLocation(body, loc) ||
            !E::TryGetActorRotation(body, rot)) {
            Deny(slot, p.op, key, "the placed poster names no rolled class, or a read failed");
            return true;
        }
        void* rolled = PO::SpawnRolled(cls, loc, rot, look);
        if (!rolled) {
            Deny(slot, p.op, key, "the rolled poster did not spawn");
            return true;
        }
        E::DestroyActor(target);
        for (size_t i = 0; i < g_host.size(); ++i)
            if (g_host[i].key == key) {
                g_host.erase(g_host.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        SendRemoved(s, key);
        ++g_ran;
        UE_LOGI("[POSTER] host: took key='%ls' down for slot %u -- the rolled poster is at its feet (#%llu)",
                key.c_str(), static_cast<unsigned>(slot), static_cast<unsigned long long>(g_ran));
        return true;
    }

    // Picture.
    std::wstring file;
    PO::Look look;
    if (!FileNameFromWire(p.fileName, file) || file == L"None") {
        Deny(slot, p.op, key, "the file name is not well-formed");
        return true;
    }
    if (!PO::ReadLook(target, look) || !look.custom) {
        Deny(slot, p.op, key, "the poster takes no custom picture");
        return true;
    }
    bool fell = false;
    if (!PO::WriteFileName(target, file) || !PO::Redraw(target, &fell)) {
        Deny(slot, p.op, key, "the host's copy did not take the picture");
        return true;
    }
    if (fell) SayFallback(key, file);
    NoteHost(target);
    ++g_ran;
    UE_LOGI("[POSTER] host: slot %u set key='%ls' to '%ls' (#%llu)", static_cast<unsigned>(slot), key.c_str(),
            file.c_str(), static_cast<unsigned long long>(g_ran));
    return true;
}

// ---- the rolled poster's record -----------------------------------------------------------------------------------
// Its loadData restores the type and the file name and does not redraw; the record lane lands it through here.
bool ApplyRolledRecord(void* actor, const ue_wrap::save_record::SaveRecord& rec) {
    if (!ue_wrap::save_record::ApplyRecord(actor, rec)) return false;
    bool fell = false;
    PO::Redraw(actor, &fell);
    ++g_records;
    PO::Look look;
    if (fell && PO::ReadLook(actor, look)) SayFallback(ue_wrap::prop::GetInteractableKeyString(actor), look.fileName);
    return true;
}

// A client's republish of a rolled poster's record (its picker): only from a live sender in reach.
bool AcceptsClientRecord(void* actor, uint8_t senderSlot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return true;
    if (coop::players::Registry::Get().LifeOf(senderSlot) != coop::players::PuppetLife::Alive) {
        Deny(senderSlot, OP::kPicture, ue_wrap::prop::GetInteractableKeyString(actor), "a record from a dead sender");
        return false;
    }
    const EL::IntentSubject subject = EL::IntentTarget::ForClientIntent(*s, senderSlot, kReachUU).Authorize(actor);
    if (!subject) {
        Deny(senderSlot, OP::kPicture, ue_wrap::prop::GetInteractableKeyString(actor), EL::OutcomeName(subject.outcome));
        return false;
    }
    return true;
}

// ---- the watches --------------------------------------------------------------------------------------------------
bool SendIntent(coop::net::Session* s, const coop::net::PosterIntentPayload& p, const std::wstring& key) {
    if (!s->SendReliable(coop::net::ReliableKind::PosterIntent, &p, sizeof(p))) {
        UE_LOGW("[POSTER] client: the %s on key='%ls' was not sent -- it runs here alone", OpName(p.op), key.c_str());
        return false;
    }
    ++g_sent;
    UE_LOGI("[POSTER] client: sent %s on key='%ls' (#%llu)", OpName(p.op), key.c_str(),
            static_cast<unsigned long long>(g_sent));
    return true;
}

// A client's put of a rolled poster it holds. put() itself places nothing without a blocking hit or on a simulating
// body, so those two run as written (they return).
sg::Verdict OnPutPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s) || !c.object) return sg::Verdict::Run;
    void* holder = PO::HoldingPlayer(c.object);
    if (!holder || !coop::players::Registry::Get().IsLocal(holder)) return sg::Verdict::Run;
    void* hitComp = E::ReadMainPlayerHitComponent(holder);
    if (!hitComp || E::IsComponentSimulatingPhysics(hitComp)) return sg::Verdict::Run;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    PO::Placement at;
    if (!KeyFits(key) || !PO::ReadPlacement(c.object, at)) {
        if (!g_placeSaid) {
            g_placeSaid = true;
            UE_LOGW("[POSTER] client: a put had no key or no readable placement -- it runs here alone (said once)");
        }
        return sg::Verdict::Run;
    }
    coop::net::PosterIntentPayload p{};
    p.op = OP::kPut;
    coop::net::WireKeyFromString(key, p.key);
    p.locX = at.location.X;
    p.locY = at.location.Y;
    p.locZ = at.location.Z;
    p.pitch = at.rotation.Pitch;
    p.yaw = at.rotation.Yaw;
    p.roll = at.rotation.Roll;
    p.scaleX = at.scale.X;
    p.scaleY = at.scale.Y;
    p.scaleZ = at.scale.Z;
    return SendIntent(s, p, key) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// A picture picked on a rolled poster: its record goes out (a client's as an intent the host re-publishes).
void OnRolledPickerPost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || !c.object || c.fromOurCode) return;
    if (s->role() == coop::net::Role::Host && !s->AnyWorldReadyPeer()) return;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (!KeyFits(key)) return;
    if (coop::prop_save_data::Publish(s, c.object, key))
        UE_LOGI("[POSTER] %s: the rolled poster key='%ls' took a new picture -- its record went out",
                s->role() == coop::net::Role::Client ? "client" : "host", key.c_str());
}

// A client's take-down (action 7) of a placed poster.
sg::Verdict OnPlacedActionPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s) || !c.object || !c.locals) return sg::Verdict::Run;
    const int32_t off = g_actionParam.Of(c.function);
    if (off < 0 || c.locals[off] != P::name::PosterActionTakeDown) return sg::Verdict::Run;
    const int32_t pOff = g_playerParam.Of(c.function);
    void* player = pOff >= 0 ? *reinterpret_cast<void* const*>(c.locals + pOff) : nullptr;
    if (player && !coop::players::Registry::Get().IsLocal(player)) return sg::Verdict::Run;
    const std::wstring key = PO::KeyOf(c.object);
    if (!KeyFits(key)) {
        UE_LOGW("[POSTER] client: a placed poster with no key was taken down -- it runs here alone");
        return sg::Verdict::Run;
    }
    coop::net::PosterIntentPayload p{};
    p.op = OP::kTakeDown;
    coop::net::WireKeyFromString(key, p.key);
    return SendIntent(s, p, key) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// A client's custom picture on a placed poster.
sg::Verdict OnPlacedPickerPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s) || !c.object || !c.locals) return sg::Verdict::Run;
    const int32_t off = g_pickedParam.Of(c.function);
    const std::wstring key = PO::KeyOf(c.object);
    if (off < 0 || !KeyFits(key)) return sg::Verdict::Run;
    R::FName picked{0, 0};
    std::memcpy(&picked, c.locals + off, sizeof(picked));
    coop::net::PosterIntentPayload p{};
    p.op = OP::kPicture;
    coop::net::WireKeyFromString(key, p.key);
    const std::wstring file = R::ToString(picked);
    if (file.empty() || file == L"None" || !FileNameToWire(file, p.fileName)) {
        UE_LOGW("[POSTER] client: the picked file '%ls' cannot cross the wire -- it shows here alone", file.c_str());
        return sg::Verdict::Run;
    }
    return SendIntent(s, p, key) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// Every placement, load and picture change of a placed poster ends in setPoster: the host notes it.
void OnSetPosterPost(const sg::Call& c) {
    if (IsHost(g_session.load(std::memory_order_acquire)) && c.object) NoteHost(c.object);
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        const Watch& w = kWatches[i];
        sg::PreFn pre = nullptr;
        sg::PostFn post = nullptr;
        switch (w.tag) {
            case kTagPut: pre = &OnPutPre; break;
            case kTagRolledPicker: post = &OnRolledPickerPost; break;
            case kTagPlacedAction: pre = &OnPlacedActionPre; break;
            case kTagPlacedPicker: pre = &OnPlacedPickerPre; break;
            default: post = &OnSetPosterPost; break;
        }
        if (sg::WatchName(*w.row, w.tag, pre, post)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[POSTER] the script-body gate refused the watch on %s -- that poster verb stays on its own peer",
                w.what);
    }
}

void Settle() {
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("[POSTER] the poster watches are live: put, both pickers, take-down and setPoster");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[POSTER] %d of %d poster watches are dead -- those verbs stay on their own peer", kWatchCount - live,
                kWatchCount);
    }
}

}  // namespace

namespace detail {

bool FileNameToWire(const std::wstring& name, coop::net::PosterFileName& out) {
    std::memset(&out, 0, sizeof(out));
    if (name.empty() || name == L"None") return true;
    const std::string u = coop::text::ToUtf8Exact(name);
    if (u.size() > sizeof(out.data)) return false;
    std::memcpy(out.data, u.data(), u.size());
    out.len = static_cast<uint8_t>(u.size());
    return true;
}

bool FileNameFromWire(const coop::net::PosterFileName& in, std::wstring& out) {
    if (in.len > sizeof(in.data)) return false;
    if (in.len == 0) {
        out = L"None";
        return true;
    }
    std::wstring w;
    if (!coop::text::FromUtf8Strict(in.data, in.len, &w)) return false;
    out = std::move(w);
    return true;
}

bool KeyFits(const std::wstring& key) { return !key.empty() && key != L"None" && key.size() <= 31; }

void SayFallback(const std::wstring& key, const std::wstring& file) {
    ++g_fellBack;
    if (g_fallbackSaid) return;
    g_fallbackSaid = true;
    UE_LOGI("[POSTER] key='%ls': the custom image '%ls' is not in this player's posters folder -- drawn with its preset "
            "picture here (said once; images are not transferred)", key.c_str(), file.c_str());
}

}  // namespace detail

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    RegisterWatches();
    if (!g_gatesRegistered) {
        g_gatesRegistered = true;
        coop::prop_record_gates::RegisterApplier(P::name::PosterRolledClass, &ApplyRolledRecord);
        coop::prop_record_gates::RegisterIntentGate(P::name::PosterRolledClass, &AcceptsClientRecord);
    }
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (!session.running()) return;
    if (session.role() == coop::net::Role::Host) {
        for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
            Slot& sl = g_slots[slot];
            sl.queue.Serve(kBudget, NowMs, [&](const coop::net::PosterIntentPayload& p) {
                if (!Execute(session, p, slot)) return coop::net::Served::Wait;
                sl.waitSaid = false;
                return coop::net::Served::Done;
            });
        }
        PumpHost(session);
    } else {
        detail::MirrorTick();
    }
}

void OnPeerWorldReady(int slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    // Every placed poster in the world now, the ones no watch has seen included.
    g_scanned = true;
    for (void* w : PO::AllPlaced()) NoteHost(w);
    int sent = 0;
    for (HostRow& r : g_host) {
        void* w = r.actor.Get();
        if (!w || !EnsureKey(r, w) || !Capture(r, w)) continue;
        coop::net::PosterStatePayload p{};
        if (!FillPlaced(r, true, p)) continue;
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::PosterState, &p, sizeof(p))) ++sent;
    }
    coop::net::PosterStatePayload end{};
    end.op = SO::kSnapshotEnd;
    s->SendReliableToSlot(slot, coop::net::ReliableKind::PosterState, &end, sizeof(end));
    UE_LOGI("[POSTER] host: slot %d's world is up -- sent %d placed poster(s) and the snapshot's end", slot, sent);
}

void OnIntent(coop::net::Session& session, const coop::net::PosterIntentPayload& p, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    auto copy = p;
    if (!sl.queue.Push(std::move(copy), kMaxPending))
        Deny(senderSlot, p.op, coop::net::StringFromWireKey(p.key), "queue full");
}

void OnState(const coop::net::PosterStatePayload& p) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return;
    detail::MirrorState(p);
}

bool WellFormed(const coop::net::PosterIntentPayload& p) {
    if (p.reserved[0] || p.reserved[1] || p.reserved[2]) return false;
    if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || p.fileName.len > sizeof(p.fileName.data)) return false;
    switch (p.op) {
        case OP::kPut: {
            const float f[] = {p.locX, p.locY, p.locZ, p.pitch, p.yaw, p.roll, p.scaleX, p.scaleY, p.scaleZ};
            for (float v : f)
                if (!Finite(v)) return false;
            const float sc[] = {p.scaleX, p.scaleY, p.scaleZ};
            for (float v : sc)
                if (v <= 0.f || v > kMaxScale) return false;
            return p.fileName.len == 0;
        }
        case OP::kTakeDown: return p.fileName.len == 0;
        case OP::kPicture: return p.fileName.len > 0;
        default: return false;
    }
}

bool WellFormed(const coop::net::PosterStatePayload& p) {
    if (p.reserved[0] || p.reserved[1] || (p.flags & ~SF::kKnown) != 0) return false;
    if (p.key.len > sizeof(p.key.data) || p.cls.len > sizeof(p.cls.data) || p.fileName.len > sizeof(p.fileName.data))
        return false;
    switch (p.op) {
        case SO::kPlaced: {
            if (p.key.len == 0 || p.cls.len == 0) return false;
            const float f[] = {p.locX, p.locY, p.locZ, p.pitch, p.yaw, p.roll, p.scaleX, p.scaleY, p.scaleZ};
            for (float v : f)
                if (!Finite(v)) return false;
            const float sc[] = {p.scaleX, p.scaleY, p.scaleZ};
            for (float v : sc)
                if (v <= 0.f || v > kMaxScale) return false;
            return true;
        }
        case SO::kRemoved: return p.key.len > 0;
        case SO::kSnapshotEnd: return true;
        default: return false;
    }
}

void CountDropped() { ++g_dropped; }

void OnDisconnect() {
    const detail::MirrorCounts m = detail::Counts();
    if (g_sent || g_ran || g_denied || g_dropped || g_published || g_removed || m.applied || m.spawned ||
        m.destroyed || m.dropped || g_records)
        UE_LOGI("[POSTER] session end -- verbs sent=%llu run=%llu denied=%llu dropped=%llu; placed states "
                "published=%llu removed=%llu applied=%llu spawned=%llu destroyed=%llu; rolled records landed=%llu; "
                "custom images drawn as presets=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_denied), static_cast<unsigned long long>(g_dropped + m.dropped),
                static_cast<unsigned long long>(g_published), static_cast<unsigned long long>(g_removed),
                static_cast<unsigned long long>(m.applied), static_cast<unsigned long long>(m.spawned),
                static_cast<unsigned long long>(m.destroyed), static_cast<unsigned long long>(g_records),
                static_cast<unsigned long long>(g_fellBack));
    g_host.clear();
    g_scanned = false;
    detail::MirrorReset();
    g_sent = g_ran = g_denied = g_dropped = g_published = g_removed = 0;
    g_fellBack = g_records = 0;
    g_fullSaid = g_placeSaid = g_fallbackSaid = false;
}

}  // namespace coop::poster_sync
