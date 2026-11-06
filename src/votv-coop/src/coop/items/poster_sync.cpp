// coop/items/poster_sync.cpp -- see coop/items/poster_sync.h.

#include "coop/items/poster_sync.h"

#include "coop/items/poster_put_ledger.h"
#include "poster_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
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
namespace OF = coop::net::poster_intent_flags;
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
constexpr uint64_t kHeldWaitMs = 3000;   // how long the host waits for a sender's hand mirror to show the poster
constexpr uint64_t kPutWaitMs = coop::poster_put::ClientPending::kExpiryMs;
constexpr int32_t  kMaxLookType = 1023;
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
    coop::poster_put::HostLast last;   // the put last answered: a repeat is answered again, an abort takes it back
    ue_wrap::CachedObjRef      lastPlaced;
    bool     heldWaiting = false;      // a put waits for the sender's hand mirror to show a rolled poster
    uint64_t heldSinceMs = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

const char* OpName(uint8_t op) {
    switch (op) {
        case OP::kPut: return "put";
        case OP::kTakeDown: return "take-down";
        case OP::kPicture: return "picture";
        case OP::kPutAbort: return "put-abort";
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

void SendPutResult(coop::net::Session& s, uint8_t slot, uint16_t requestId, bool ok, const std::wstring& key,
                   bool consumed) {
    coop::net::PosterStatePayload r{};
    r.op = ok ? SO::kPutAccepted : SO::kPutRefused;
    r.flags = (ok && consumed) ? SF::kConsumed : 0;
    r.requestId = requestId;
    if (ok) coop::net::WireKeyFromString(key, r.key);
    if (s.SendReliableToSlot(slot, coop::net::ReliableKind::PosterState, &r, sizeof(r)))
        UE_LOGI("[POSTER] host: answered slot %u's put #%u -- %s key='%ls'", static_cast<unsigned>(slot),
                static_cast<unsigned>(requestId), ok ? "ACCEPTED" : "refused", ok ? key.c_str() : L"");
    else
        UE_LOGW("[POSTER] host: slot %u's put #%u answer was not sent", static_cast<unsigned>(slot),
                static_cast<unsigned>(requestId));
}

// One put answered: kept for a repeat and for an abort, told to the sender, a refusal said.
void FinishPut(coop::net::Session& s, uint8_t slot, const coop::net::PosterIntentPayload& p, bool ok,
               const std::wstring& placedKey, bool consumed, const char* why) {
    Slot& sl = g_slots[slot];
    sl.last.Record(p.requestId, ok, consumed, placedKey);
    sl.heldWaiting = false;
    if (!ok) {
        // A put refusal is always said: the Deny line is rate-limited, and a refused put is what a test looks for.
        UE_LOGI("[POSTER] host: REFUSED slot %u's put #%u -- %s", static_cast<unsigned>(slot),
                static_cast<unsigned>(p.requestId), why ? why : "?");
        Deny(slot, p.op, coop::net::StringFromWireKey(p.key), why);
    }
    SendPutResult(s, slot, p.requestId, ok, placedKey, consumed);
}

// A client's put. Two routes to the rolled poster it means: a rolled world prop its key names in the host's key index
// (a prop on the floor), else the poster in the sender's hand, which no key index holds (a hand is never the world
// prop a key names, and the world prop died at the pickup) and which the host knows by that slot's hand mirror. The
// held poster's look is the intent's: the hand mirror carries none. True when done (run or refused), false to wait
// for the sender's body or hand.
bool ExecutePut(coop::net::Session& s, const coop::net::PosterIntentPayload& p, uint8_t slot) {
    Slot& sl = g_slots[slot];
    if (sl.last.Matches(p.requestId)) {   // a repeat of a request already answered
        SendPutResult(s, slot, p.requestId, sl.last.ok, sl.last.key, sl.last.consumed);
        return true;
    }
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        FinishPut(s, slot, p, false, {}, false, "the sender's player is not alive");
        return true;
    }
    const EL::IntentTarget reach = EL::IntentTarget::ForClientIntent(s, slot, kReachUU);
    if (!reach.HasBody()) {
        if (!sl.waitSaid) {
            sl.waitSaid = true;
            UE_LOGI("[POSTER] host: slot %u's poster verbs wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    PO::Placement at;
    at.location = {p.locX, p.locY, p.locZ};
    at.rotation = {p.pitch, p.yaw, p.roll};
    at.scale = {p.scaleX, p.scaleY, p.scaleZ};
    if (!reach.AuthorizeSegment(at.location, at.location)) {
        FinishPut(s, slot, p, false, {}, false, "the placement is out of the sender's reach");
        return true;
    }

    void* worldProp = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    const bool world = PO::IsRolled(worldProp);
    void* cls = nullptr;
    PO::Look look;
    if (world) {
        const EL::IntentSubject subject = reach.Authorize(worldProp);
        if (!subject) {
            FinishPut(s, slot, p, false, {}, false, EL::OutcomeName(subject.outcome));
            return true;
        }
        if (const coop::prop_hold::Holder h = coop::prop_hold::WhoHolds(worldProp, slot);
            h != coop::prop_hold::Holder::None) {
            FinishPut(s, slot, p, false, {}, false, coop::prop_hold::HolderWhy(h));
            return true;
        }
        cls = PO::PosterClassOf(worldProp);
        if (!PO::IsPlacedClass(cls) || !PO::ReadLook(worldProp, look)) {
            FinishPut(s, slot, p, false, {}, false, "the rolled poster names no placed class or its look did not read");
            return true;
        }
    } else {
        void* held = coop::hand_item::MirrorActorForSlot(slot);
        if (!PO::IsRolled(held)) {
            const uint64_t now = NowMs();
            if (!sl.heldWaiting) {
                sl.heldWaiting = true;
                sl.heldSinceMs = now;
            }
            if (now - sl.heldSinceMs < kHeldWaitMs) return false;   // the hand state may still be on its way
            FinishPut(s, slot, p, false, {}, false, "the sender's hand shows no rolled poster here");
            return true;
        }
        cls = PO::PosterClassOf(held);
        if (!PO::IsPlacedClass(cls)) {
            FinishPut(s, slot, p, false, {}, false, "the held poster names no placed class");
            return true;
        }
        look.type = p.type;
        look.custom = (p.flags & OF::kCustom) != 0;
        if (!FileNameFromWire(p.fileName, look.fileName) || (look.custom && look.fileName == L"None")) {
            FinishPut(s, slot, p, false, {}, false, "the held poster's look is not well-formed");
            return true;
        }
    }
    const std::wstring placedKey = coop::prop_synth_key::RandomKeyString();
    void* w = PO::SpawnPlaced(cls, at, look, false, placedKey);
    if (!w) {
        FinishPut(s, slot, p, false, {}, false, "the placed poster did not spawn");
        return true;
    }
    if (world) E::DestroyActor(worldProp);   // the prop lane carries its destroy
    NoteHost(w);
    sl.lastPlaced.Set(w);
    FinishPut(s, slot, p, true, placedKey, world, nullptr);
    ++g_ran;
    UE_LOGI("[POSTER] host: put slot %u's %s poster on the wall as %ls key='%ls' type=%d file='%ls'%s (#%llu, req %u)",
            static_cast<unsigned>(slot), world ? "rolled world" : "held", R::ClassNameOf(w).c_str(), placedKey.c_str(),
            look.type, look.fileName.c_str(), look.custom ? " custom" : "", static_cast<unsigned long long>(g_ran),
            static_cast<unsigned>(p.requestId));
    return true;
}

// A client could not finish its own put: the placed poster that put made goes back, once.
bool ExecutePutAbort(coop::net::Session& s, const coop::net::PosterIntentPayload& p, uint8_t slot) {
    Slot& sl = g_slots[slot];
    if (!sl.last.TakeBack(p.requestId)) {
        Deny(slot, p.op, coop::net::StringFromWireKey(p.key), "no put of that request is there to give back");
        return true;
    }
    void* w = sl.lastPlaced.Get();
    const std::wstring key = sl.last.key;
    if (w && PO::IsPlaced(w)) {
        for (size_t i = 0; i < g_host.size(); ++i)
            if (g_host[i].actor.Is(w)) {
                g_host.erase(g_host.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        E::DestroyActor(w);
    }
    SendRemoved(s, key);
    UE_LOGI("[POSTER] host: slot %u gave put #%u back -- key='%ls' is off the wall", static_cast<unsigned>(slot),
            static_cast<unsigned>(p.requestId), key.c_str());
    return true;
}

// The intent's verdict: true when done (run or refused), false to wait for the sender's body.
bool Execute(coop::net::Session& s, const coop::net::PosterIntentPayload& p, uint8_t slot) {
    if (p.op == OP::kPut) return ExecutePut(s, p, slot);
    if (p.op == OP::kPutAbort) return ExecutePutAbort(s, p, slot);
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* target = PlacedByKey(key);
    if (!PO::IsPlaced(target)) {
        Deny(slot, p.op, key, "no placed poster holds the key");
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
    UE_LOGI("[POSTER] client: sent %s on key='%ls' (#%llu, req %u, type %d)", OpName(p.op), key.c_str(),
            static_cast<unsigned long long>(g_sent), static_cast<unsigned>(p.requestId), p.type);
    return true;
}

// ---- the client's put -----------------------------------------------------------------------------------------------
// The put waits for the host's verdict: while it does, the held poster stays in the hand and a second click is
// swallowed. An accepted put is then finished here by the game's own put() (which places the wall poster and consumes
// the held one, the hand and the inventory both), once; the wall poster it placed takes the host's canonical key. A put
// the hand can no longer finish is given back, and the host takes the poster off the wall.
struct HeldPut {
    ue_wrap::CachedObjRef held;
    PO::Look              look;
    PO::Placement         at;
};
coop::poster_put::ClientPending g_putPending;
HeldPut g_putHeld;
bool g_replaying = false;   // our own put() runs: its watch lets it through

void SendPutAbort(coop::net::Session* s, uint16_t requestId, const std::wstring& key) {
    coop::net::PosterIntentPayload p{};
    p.op = OP::kPutAbort;
    p.requestId = requestId;
    if (KeyFits(key)) coop::net::WireKeyFromString(key, p.key);
    s->SendReliable(coop::net::ReliableKind::PosterIntent, &p, sizeof(p));
    UE_LOGI("[POSTER] client: gave put #%u back to the host", static_cast<unsigned>(requestId));
}

// A client's put of a rolled poster it holds. put() itself places nothing without a blocking hit or on a simulating
// body, so those two run as written (they return).
sg::Verdict OnPutPre(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (g_replaying || !IsClient(s) || !c.object) return sg::Verdict::Run;
    void* holder = PO::HoldingPlayer(c.object);
    if (!holder || !coop::players::Registry::Get().IsLocal(holder)) return sg::Verdict::Run;
    void* hitComp = E::ReadMainPlayerHitComponent(holder);
    if (!hitComp || E::IsComponentSimulatingPhysics(hitComp)) {
        UE_LOGI("[POSTER] client: put() with %s -- it places nothing, runs as written",
                hitComp ? "its aim on a simulating body" : "no blocking hit");
        return sg::Verdict::Run;
    }
    const uint64_t now = NowMs();
    if (g_putPending.Open(now)) {
        UE_LOGI("[POSTER] client: put() while put #%u waits for the host -- swallowed",
                static_cast<unsigned>(g_putPending.Id()));
        return sg::Verdict::Cancel;   // one put at a time: this click waits with the first
    }
    PO::Placement at;
    PO::Look look;
    coop::net::PosterIntentPayload p{};
    if (!PO::ReadPlacement(c.object, at) || !PO::ReadLook(c.object, look) || look.type < 0 ||
        !FileNameToWire(look.fileName, p.fileName)) {
        if (!g_placeSaid) {
            g_placeSaid = true;
            UE_LOGW("[POSTER] client: a put had no readable placement or look -- it runs here alone (said once)");
        }
        return sg::Verdict::Run;
    }
    std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (!KeyFits(key)) key.clear();   // a held poster may carry none: the host knows it by the sender's hand
    const uint16_t id = g_putPending.Begin(now);
    p.op = OP::kPut;
    p.flags = look.custom ? OF::kCustom : 0;
    p.requestId = id;
    p.type = look.type;
    if (!key.empty()) coop::net::WireKeyFromString(key, p.key);
    p.locX = at.location.X;
    p.locY = at.location.Y;
    p.locZ = at.location.Z;
    p.pitch = at.rotation.Pitch;
    p.yaw = at.rotation.Yaw;
    p.roll = at.rotation.Roll;
    p.scaleX = at.scale.X;
    p.scaleY = at.scale.Y;
    p.scaleZ = at.scale.Z;
    if (!SendIntent(s, p, key)) {
        g_putPending.Reset();
        return sg::Verdict::Run;
    }
    g_putHeld.held.Set(c.object);
    g_putHeld.look = look;
    g_putHeld.at = at;
    return sg::Verdict::Cancel;
}

// The poster the client's own put() just placed: the new placed poster nearest `at`, one not in `before`.
void* NewPlacedNear(const std::vector<void*>& before, const ue_wrap::FVector& at) {
    void* best = nullptr;
    double bestD = 0;
    for (void* w : PO::AllPlaced()) {
        bool old = false;
        for (void* b : before)
            if (b == w) {
                old = true;
                break;
            }
        if (old) continue;
        PO::Placement t;
        if (!PO::ReadTransform(w, t)) continue;
        const double dx = t.location.X - at.X, dy = t.location.Y - at.Y, dz = t.location.Z - at.Z;
        const double d = dx * dx + dy * dy + dz * dz;
        if (!best || d < bestD) {
            best = w;
            bestD = d;
        }
    }
    return best;
}

// The host's verdict on this client's put. Taken once: a stale or repeated answer finds nothing waiting.
void OnPutResult(coop::net::Session* s, const coop::net::PosterStatePayload& r) {
    UE_LOGI("[POSTER] client: the host %s put #%u key='%ls'", r.op == SO::kPutAccepted ? "ACCEPTED" : "refused",
            static_cast<unsigned>(r.requestId), coop::net::StringFromWireKey(r.key).c_str());
    if (!g_putPending.Take(r.requestId)) {
        UE_LOGI("[POSTER] client: put answer #%u matches no waiting put -- ignored", static_cast<unsigned>(r.requestId));
        return;
    }
    const HeldPut put = g_putHeld;
    g_putHeld = HeldPut{};
    if (r.op == SO::kPutRefused) {
        UE_LOGI("[POSTER] client: the host refused put #%u -- the poster stays in the hand",
                static_cast<unsigned>(r.requestId));
        return;
    }
    const std::wstring key = coop::net::StringFromWireKey(r.key);
    if (r.flags & SF::kConsumed) {   // the host consumed the rolled world prop: its placed state does the rest
        UE_LOGI("[POSTER] client: put #%u placed key='%ls' by the host", static_cast<unsigned>(r.requestId),
                key.c_str());
        return;
    }
    // Still the same poster in the same hand, with the look the host placed?
    void* held = put.held.Get();
    PO::Look now;
    void* holder = held ? PO::HoldingPlayer(held) : nullptr;
    if (!held || !PO::IsRolled(held) || !holder || !coop::players::Registry::Get().IsLocal(holder) ||
        !coop::hand_item::IsLocalHandViewActor(held) || !PO::ReadLook(held, now) || now != put.look) {
        UE_LOGW("[POSTER] client: the held poster changed while put #%u waited -- giving it back",
                static_cast<unsigned>(r.requestId));
        SendPutAbort(s, r.requestId, key);
        return;
    }
    const std::vector<void*> before = PO::AllPlaced();
    g_replaying = true;
    const bool ran = PO::CallPut(held);
    g_replaying = false;
    void* mine = ran ? NewPlacedNear(before, put.at.location) : nullptr;
    if (!mine) {
        UE_LOGW("[POSTER] client: the put could not finish here -- giving put #%u back", static_cast<unsigned>(r.requestId));
        SendPutAbort(s, r.requestId, key);
        return;
    }
    if (void* canon = detail::MirrorPlaced(key); canon && canon != mine) {
        E::DestroyActor(mine);   // the host's state got here first and stands already
    } else if (!detail::MirrorAdopt(key, mine)) {
        UE_LOGW("[POSTER] client: put #%u's wall poster could not take key='%ls'", static_cast<unsigned>(r.requestId),
                key.c_str());
        return;
    }
    UE_LOGI("[POSTER] client: put #%u finished here -- the wall poster is key='%ls'", static_cast<unsigned>(r.requestId),
            key.c_str());
}

void ExpirePut() {
    if (g_putPending.Expire(NowMs())) {
        g_putHeld = HeldPut{};
        UE_LOGW("[POSTER] client: no answer to a put in %llu s -- it is dropped here (a late answer is ignored)",
                static_cast<unsigned long long>(kPutWaitMs / 1000));
    }
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
        ExpirePut();
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
    if (p.op == OP::kPut || p.op == OP::kPutAbort)
        UE_LOGI("[POSTER] host: received slot %u's %s #%u key='%ls' type=%d at (%.0f,%.0f,%.0f)",
                static_cast<unsigned>(senderSlot), OpName(p.op), static_cast<unsigned>(p.requestId),
                coop::net::StringFromWireKey(p.key).c_str(), p.type, static_cast<double>(p.locX),
                static_cast<double>(p.locY), static_cast<double>(p.locZ));
    auto copy = p;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) {
        Deny(senderSlot, p.op, coop::net::StringFromWireKey(p.key), "queue full");
        if (p.op == OP::kPut) SendPutResult(session, senderSlot, p.requestId, false, {}, false);   // it need not wait
    }
}

void OnState(const coop::net::PosterStatePayload& p) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s)) return;
    if (p.op == SO::kPutAccepted || p.op == SO::kPutRefused) {
        OnPutResult(s, p);
        return;
    }
    detail::MirrorState(p);
}

bool WellFormed(const coop::net::PosterIntentPayload& p) {
    if ((p.flags & ~OF::kKnown) != 0) return false;
    if (p.key.len > sizeof(p.key.data) || p.fileName.len > sizeof(p.fileName.data)) return false;
    // Only a put and its abort carry a request id, and only a put carries a look; a put's key may be empty (a held
    // poster may have none), every other verb names a placed poster by its key.
    const bool asksPut = p.op == OP::kPut || p.op == OP::kPutAbort;
    if (asksPut != coop::poster_put::ValidRequestId(p.requestId)) return false;
    if (!asksPut && p.key.len == 0) return false;
    if (p.op != OP::kPut && (p.flags != 0 || p.type != 0)) return false;
    switch (p.op) {
        case OP::kPut: {
            const float f[] = {p.locX, p.locY, p.locZ, p.pitch, p.yaw, p.roll, p.scaleX, p.scaleY, p.scaleZ};
            for (float v : f)
                if (!Finite(v)) return false;
            const float sc[] = {p.scaleX, p.scaleY, p.scaleZ};
            for (float v : sc)
                if (v <= 0.f || v > kMaxScale) return false;
            if (p.type < 0 || p.type > kMaxLookType) return false;
            return (p.flags & OF::kCustom) ? p.fileName.len > 0 : true;
        }
        case OP::kTakeDown: return p.fileName.len == 0;
        case OP::kPicture: return p.fileName.len > 0;
        case OP::kPutAbort: return p.fileName.len == 0;
        default: return false;
    }
}

bool WellFormed(const coop::net::PosterStatePayload& p) {
    if ((p.flags & ~SF::kKnown) != 0) return false;
    if (p.key.len > sizeof(p.key.data) || p.cls.len > sizeof(p.cls.data) || p.fileName.len > sizeof(p.fileName.data))
        return false;
    const bool answersPut = p.op == SO::kPutAccepted || p.op == SO::kPutRefused;
    if (answersPut != coop::poster_put::ValidRequestId(p.requestId)) return false;
    if ((p.flags & SF::kConsumed) && p.op != SO::kPutAccepted) return false;
    switch (p.op) {
        case SO::kPutAccepted: return p.key.len > 0;
        case SO::kPutRefused: return true;
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
    g_putPending.Reset();
    g_putHeld = HeldPut{};
    detail::MirrorReset();
    g_sent = g_ran = g_denied = g_dropped = g_published = g_removed = 0;
    g_fellBack = g_records = 0;
    g_fullSaid = g_placeSaid = g_fallbackSaid = false;
}

}  // namespace coop::poster_sync
