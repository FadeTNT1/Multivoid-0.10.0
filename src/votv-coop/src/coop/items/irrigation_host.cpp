// coop/items/irrigation_host.cpp -- the host's half of the irrigation lane: a client's verbs judged and run on the
// host's copies, and the poll that sends every placed faucet's and hose's birth, change and end. See
// coop/items/irrigation_sync.h.

#include "irrigation_detail.h"

#include "coop/element/intent_authority.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cstddef>
#include <cmath>
#include <string>
#include <vector>

namespace coop::irrigation_sync::detail::host {
namespace {

namespace EL = coop::element;
namespace P  = ue_wrap::profile;

// Reach from the sender's body. A placement lands where the player's hit is; a hose's last point is where its layer
// stood (or the sprinkler it reached); a take is a use at arm's length. Generous: these catch the far side of the map.
constexpr float kPlaceReachUU = 1200.f;
constexpr float kHoseEndReachUU = 1500.f;
constexpr float kUseReachUU = 900.f;

// A bound on what one sender can make the host build or run: every faucet and hose is a save actor that persists.
constexpr int  kVerbsPerWindow = 30;
constexpr auto kVerbWindow = std::chrono::seconds(10);

constexpr auto kPollEvery = std::chrono::milliseconds(250);

struct Known {
    IR::Kind              kind = IR::Kind::None;
    std::wstring          key;
    ue_wrap::CachedObjRef ref;
    uint64_t              sig = 0;
    bool                  seen = false;
};
std::vector<Known> g_known;
std::chrono::steady_clock::time_point g_lastPoll{};

struct SlotBudget {
    std::chrono::steady_clock::time_point windowStart{};
    int used = 0;
};
SlotBudget g_budget[coop::net::kMaxPeers];

bool TakeBudget(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return false;
    SlotBudget& b = g_budget[slot];
    const auto now = std::chrono::steady_clock::now();
    if (now - b.windowStart > kVerbWindow) {
        b.windowStart = now;
        b.used = 0;
    }
    return ++b.used <= kVerbsPerWindow;
}

void* SenderBody(uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return (rp && rp->valid()) ? rp->GetActor() : nullptr;
}

bool AllFinite(const std::array<float, 10>& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

bool SendState(coop::net::Session& s, const State& st, int slot) {
    std::vector<uint8_t> blob;
    WriteState(blob, st);
    return slot < 0 ? coop::blob_chunks::SendBlob(&s, coop::net::ReliableKind::IrrigationState, NextBlobSeq(), blob)
                    : coop::blob_chunks::SendBlobToSlot(&s, slot, coop::net::ReliableKind::IrrigationState,
                                                        NextBlobSeq(), blob);
}

bool SendUpsert(coop::net::Session& s, void* a, IR::Kind k, const std::wstring& key, int slot) {
    State st;
    st.op = StateOp::Upsert;
    st.kind = k;
    if (!WireRecord(a, k, key, st.rec)) {
        UE_LOGW("[IRRIGATION] host: the %s key='%ls' did not give its record -- not sent", IR::KindName(k), key.c_str());
        return false;
    }
    const bool ok = SendState(s, st, slot);
    if (k == IR::Kind::Hose) {
        UE_LOGI("[IRRIGATION] host: hose key='%ls' -> %s (%zu points, faucet='%ls', sprinkler='%ls')%s", key.c_str(),
                slot < 0 ? "every peer" : "a joiner", st.rec.vectors.empty() ? size_t{0} : st.rec.vectors[0].size(),
                RecordLink(st.rec, 0).c_str(), RecordLink(st.rec, 1).c_str(), ok ? "" : " -- NOT SENT");
    } else {
        bool active = false;
        IR::ReadFaucetActive(a, active);
        UE_LOGI("[IRRIGATION] host: faucet key='%ls' -> %s (hose='%ls', active=%d)%s", key.c_str(),
                slot < 0 ? "every peer" : "a joiner", RecordLink(st.rec, 0).c_str(), active ? 1 : 0,
                ok ? "" : " -- NOT SENT");
    }
    return ok;
}

void SendRemove(coop::net::Session& s, IR::Kind k, const std::wstring& key, int slot) {
    State st;
    st.op = StateOp::Remove;
    st.kind = k;
    st.key = key;
    const bool ok = SendState(s, st, slot);
    UE_LOGI("[IRRIGATION] host: %s key='%ls' is gone -> %s%s", IR::KindName(k), key.c_str(),
            slot < 0 ? "every peer" : "its sender", ok ? "" : " -- NOT SENT");
}

void Refuse(coop::net::Session& s, uint8_t slot, const char* verb, IR::Kind k, const std::wstring& key,
            const char* why, bool dropSenderCopy) {
    UE_LOGW("[IRRIGATION] host: REFUSED slot %u's %s key='%ls' -- %s%s", static_cast<unsigned>(slot), verb,
            key.c_str(), why, dropSenderCopy ? " (its copy there is told to go)" : "");
    if (dropSenderCopy && KeyFits(key)) SendRemove(s, k, key, slot);
}

// The common gate of every verb: a live sender with a body here.
bool SenderReady(coop::net::Session& s, uint8_t slot, const char* verb, IR::Kind k, const std::wstring& key,
                 bool dropSenderCopy) {
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        Refuse(s, slot, verb, k, key, "the sender's player is not alive", dropSenderCopy);
        return false;
    }
    if (!EL::IntentTarget::ForClientIntent(s, slot, kUseReachUU).HasBody()) {
        Refuse(s, slot, verb, k, key, "the host has no body for the sender yet", dropSenderCopy);
        return false;
    }
    if (!TakeBudget(slot)) {
        Refuse(s, slot, verb, k, key, "the sender is over its irrigation budget", dropSenderCopy);
        return false;
    }
    return true;
}

void PlaceFaucet(coop::net::Session& s, uint8_t slot, const SaveRecord& in) {
    const std::wstring& key = in.key;
    const char* verb = "faucet placement";
    if (!KeyFits(key)) return Refuse(s, slot, verb, IR::Kind::Faucet, key, "no usable key", false);
    if (!SenderReady(s, slot, verb, IR::Kind::Faucet, key, true)) return;
    if (!AllFinite(in.xform)) return Refuse(s, slot, verb, IR::Kind::Faucet, key, "a transform that is not finite", true);
    const ue_wrap::FVector loc{in.xform[4], in.xform[5], in.xform[6]};
    const EL::IntentSubject reach = EL::IntentTarget::ForClientIntent(s, slot, kPlaceReachUU).AuthorizeSegment(loc, loc);
    if (!reach) return Refuse(s, slot, verb, IR::Kind::Faucet, key, EL::OutcomeName(reach.outcome), true);
    if (void* existing = Find(IR::Kind::Faucet, key)) {
        UE_LOGI("[IRRIGATION] host: slot %u's faucet key='%ls' already stands here -- re-sent", static_cast<unsigned>(slot),
                key.c_str());
        SendUpsert(s, existing, IR::Kind::Faucet, key, -1);
        return;
    }
    if (coop::prop_key_index::Find(key) || Find(IR::Kind::Hose, key))
        return Refuse(s, slot, verb, IR::Kind::Faucet, key, "the key names another actor here", true);
    // What of the sender's record the host takes: the transform, the key and `active`. Its hose link is the host's.
    SaveRecord r;
    r.className = P::name::IrrigationFaucetClass;
    r.xform = in.xform;
    r.key = key;
    const uint8_t active = (!in.bools.empty() && !in.bools[0].empty()) ? (in.bools[0][0] ? 1 : 0) : 0;
    r.bools.push_back({active});
    r.names.push_back({L"None"});
    bool created = false;
    void* a = ApplyFaucet(r, &ue_wrap::object_index::ClassByName, created);
    if (!a) return Refuse(s, slot, verb, IR::Kind::Faucet, key, "the spawn failed", true);
    UE_LOGI("[IRRIGATION] host: ACCEPTED slot %u's faucet key='%ls' at (%.0f,%.0f,%.0f) dist=%.0f", static_cast<unsigned>(slot),
            key.c_str(), loc.X, loc.Y, loc.Z, reach.distUU);
    Tick(s);
}

void CommitHose(coop::net::Session& s, uint8_t slot, const SaveRecord& in) {
    const std::wstring& key = in.key;
    const char* verb = "hose";
    if (!KeyFits(key)) return Refuse(s, slot, verb, IR::Kind::Hose, key, "no usable key", false);
    if (!SenderReady(s, slot, verb, IR::Kind::Hose, key, true)) return;
    if (!AllFinite(in.xform)) return Refuse(s, slot, verb, IR::Kind::Hose, key, "a transform that is not finite", true);
    if (in.vectors.empty() || in.vectors[0].size() < 2 || in.vectors[0].size() > kMaxHosePoints)
        return Refuse(s, slot, verb, IR::Kind::Hose, key, "a point count out of bounds", true);
    for (const auto& p : in.vectors[0])
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            return Refuse(s, slot, verb, IR::Kind::Hose, key, "a point that is not finite", true);
    const auto& end = in.vectors[0].back();
    const ue_wrap::FVector endLoc{end[0], end[1], end[2]};
    const EL::IntentSubject reach =
        EL::IntentTarget::ForClientIntent(s, slot, kHoseEndReachUU).AuthorizeSegment(endLoc, endLoc);
    if (!reach) return Refuse(s, slot, verb, IR::Kind::Hose, key, EL::OutcomeName(reach.outcome), true);
    if (coop::prop_key_index::Find(key) || Find(IR::Kind::Faucet, key))
        return Refuse(s, slot, verb, IR::Kind::Hose, key, "the key names another actor here", true);

    const std::wstring faucetKey = RecordLink(in, 0);
    void* faucet = Find(IR::Kind::Faucet, faucetKey);
    if (!faucetKey.empty() && !faucet)
        UE_LOGW("[IRRIGATION] host: slot %u's hose key='%ls' names faucet '%ls', which is not here -- laid unconnected",
                static_cast<unsigned>(slot), key.c_str(), faucetKey.c_str());
    if (faucet) {
        void* other = IR::FaucetHose(faucet);
        if (other && IR::KeyOf(other) != key)
            return Refuse(s, slot, verb, IR::Kind::Hose, key, "its faucet already has another hose", true);
    }
    const std::wstring sprinklerKey = RecordLink(in, 1);
    void* sprinkler = FindSprinkler(sprinklerKey);
    if (sprinkler) {
        void* other = IR::SprinklerHose(sprinkler);
        if (other && IR::KeyOf(other) != key) {
            UE_LOGW("[IRRIGATION] host: slot %u's hose key='%ls' names sprinkler '%ls', which another hose holds -- "
                    "left off", static_cast<unsigned>(slot), key.c_str(), sprinklerKey.c_str());
            sprinkler = nullptr;
        }
    }
    // What of the sender's record the host takes: the transform, the key, the points, and the links it judged.
    SaveRecord r;
    r.className = P::name::IrrigationHoseClass;
    r.xform = in.xform;
    r.key = key;
    r.vectors.push_back(in.vectors[0]);
    r.names.push_back({faucet ? faucetKey : std::wstring(L"None"), sprinkler ? sprinklerKey : std::wstring(L"None")});
    bool created = false;
    void* a = ApplyHose(r, &ue_wrap::object_index::ClassByName, created);
    if (!a) return Refuse(s, slot, verb, IR::Kind::Hose, key, "the spawn failed", true);
    UE_LOGI("[IRRIGATION] host: ACCEPTED slot %u's hose key='%ls' (%s, %zu points, faucet='%ls', sprinkler='%ls')",
            static_cast<unsigned>(slot), key.c_str(), created ? "built" : "updated", r.vectors[0].size(),
            faucet ? faucetKey.c_str() : L"", sprinkler ? sprinklerKey.c_str() : L"");
    Tick(s);
}

void TakeFaucet(coop::net::Session& s, uint8_t slot, const std::wstring& key) {
    const char* verb = "faucet take";
    void* a = Find(IR::Kind::Faucet, key);
    if (!a) return Refuse(s, slot, verb, IR::Kind::Faucet, key, "no faucet under that key here", true);
    if (!SenderReady(s, slot, verb, IR::Kind::Faucet, key, false)) return;
    const EL::IntentSubject reach = EL::IntentTarget::ForClientIntent(s, slot, kUseReachUU).Authorize(a);
    if (!reach) return Refuse(s, slot, verb, IR::Kind::Faucet, key, EL::OutcomeName(reach.outcome), false);
    // The faucet's own action 7, as the sender would have run it: the held faucet back at its place, the hose cut or
    // given back, the faucet gone. Its births ride the prop lanes; the poll sends the rest.
    const bool ok = ue_wrap::appliance::CallAction(a, SenderBody(slot), static_cast<uint8_t>(P::name::IrrigationFaucetActionTake));
    UE_LOGI("[IRRIGATION] host: slot %u took up faucet key='%ls' -- action 7 %s", static_cast<unsigned>(slot),
            key.c_str(), ok ? "ran" : "FAILED");
    Tick(s);
}

void TakeHose(coop::net::Session& s, uint8_t slot, const std::wstring& key, bool collect) {
    const char* verb = collect ? "hose collect" : "hose resume";
    void* a = Find(IR::Kind::Hose, key);
    if (!a) {
        UE_LOGI("[IRRIGATION] host: slot %u's %s of key='%ls' -- no such hose here (nothing to do)",
                static_cast<unsigned>(slot), verb, key.c_str());
        return;
    }
    if (!SenderReady(s, slot, verb, IR::Kind::Hose, key, false)) return;
    // The pickup is at either end of the hose (its start to collect, its end to resume): the nearer one is judged.
    std::vector<ue_wrap::FVector> pts;
    IR::ReadHosePoints(a, pts);
    const EL::IntentTarget target = EL::IntentTarget::ForClientIntent(s, slot, kUseReachUU);
    EL::IntentSubject reach = target.Authorize(a);
    if (!reach && !pts.empty()) reach = target.AuthorizeSegment(pts.front(), pts.front());
    if (!reach && !pts.empty()) reach = target.AuthorizeSegment(pts.back(), pts.back());
    if (!reach) return Refuse(s, slot, verb, IR::Kind::Hose, key, EL::OutcomeName(reach.outcome), false);
    if (!collect) {
        UE_LOGI("[IRRIGATION] host: slot %u resumed laying hose key='%ls' -- kept here until its new commit",
                static_cast<unsigned>(slot), key.c_str());
        return;
    }
    UnlinkAndDestroy(a);
    UE_LOGI("[IRRIGATION] host: slot %u collected hose key='%ls' -- destroyed here", static_cast<unsigned>(slot),
            key.c_str());
    Tick(s);
}

void DetachSprinkler(coop::net::Session& s, uint8_t slot, const std::wstring& key) {
    const char* verb = "sprinkler detach";
    void* a = Find(IR::Kind::Hose, key);
    void* sprinkler = a ? IR::HoseSprinkler(a) : nullptr;
    if (!sprinkler) {
        UE_LOGI("[IRRIGATION] host: slot %u's sprinkler detach of hose key='%ls' -- no sprinkler on it here",
                static_cast<unsigned>(slot), key.c_str());
        return;
    }
    if (!SenderReady(s, slot, verb, IR::Kind::Hose, key, false)) return;
    const EL::IntentSubject reach = EL::IntentTarget::ForClientIntent(s, slot, kUseReachUU).Authorize(sprinkler);
    if (!reach) return Refuse(s, slot, verb, IR::Kind::Hose, key, EL::OutcomeName(reach.outcome), false);
    IR::CallDetachSprinkler(a);
    UE_LOGI("[IRRIGATION] host: slot %u detached the sprinkler of hose key='%ls'", static_cast<unsigned>(slot),
            key.c_str());
    Tick(s);
}

Known* FindKnown(IR::Kind k, const std::wstring& key) {
    for (Known& n : g_known)
        if (n.kind == k && n.key == key) return &n;
    return nullptr;
}

}  // namespace

void OnCommit(coop::net::Session& s, uint8_t slot, Commit& c) {
    switch (c.op) {
        case CommitOp::PlaceFaucet: PlaceFaucet(s, slot, c.rec); break;
        case CommitOp::CommitHose: CommitHose(s, slot, c.rec); break;
        case CommitOp::TakeFaucet: TakeFaucet(s, slot, c.key); break;
        case CommitOp::TakeHose: TakeHose(s, slot, c.key, c.collect); break;
        case CommitOp::DetachSprinkler: DetachSprinkler(s, slot, c.key); break;
    }
}

void Tick(coop::net::Session& s) {
    g_lastPoll = std::chrono::steady_clock::now();
    for (Known& n : g_known) n.seen = false;
    // Faucets first: a hose's state links the faucet a receiver already holds.
    for (IR::Kind k : {IR::Kind::Faucet, IR::Kind::Hose}) {
        for (void* a : IR::AllOf(k)) {
            std::wstring key = IR::KeyOf(a);
            if (k == IR::Kind::Hose && !IR::HoseSettled(a)) {
                // A hose a player is laying here: it goes when it settles, and what stood before stays named.
                if (Known* n = FindKnown(k, key)) n->seen = true;
                continue;
            }
            if (key.empty()) {
                key = IR::RegisterKey(a);   // the game's own mint, which the host's save keeps
                if (KeyFits(key))
                    UE_LOGI("[IRRIGATION] host: a %s had no key -- the game minted '%ls'", IR::KindName(k), key.c_str());
            }
            if (!KeyFits(key)) continue;
            const uint64_t sig = Signature(a, k);
            Known* n = FindKnown(k, key);
            if (!n) {
                g_known.push_back(Known{k, key, {}, 0, true});
                n = &g_known.back();
                n->ref.Set(a);
            } else {
                n->seen = true;
                if (n->ref.Get() != a) n->ref.Set(a);
                if (n->sig == sig) continue;
            }
            n->sig = sig;
            SendUpsert(s, a, k, key, -1);
        }
    }
    for (size_t i = 0; i < g_known.size();) {
        Known& n = g_known[i];
        if (n.seen || n.ref.Alive()) { ++i; continue; }
        SendRemove(s, n.kind, n.key, -1);
        g_known.erase(g_known.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void SnapshotTo(coop::net::Session& s, int slot) {
    State begin;
    begin.op = StateOp::SnapBegin;
    SendState(s, begin, slot);
    uint32_t count = 0;
    for (IR::Kind k : {IR::Kind::Faucet, IR::Kind::Hose}) {
        for (void* a : IR::AllOf(k)) {
            if (k == IR::Kind::Hose && !IR::HoseSettled(a)) continue;
            const std::wstring key = IR::KeyOf(a);
            if (!KeyFits(key)) continue;
            if (SendUpsert(s, a, k, key, slot)) ++count;
        }
    }
    State end;
    end.op = StateOp::SnapEnd;
    end.count = count;
    SendState(s, end, slot);
    UE_LOGI("[IRRIGATION] host: snapshot to slot %d -- %u placed faucet(s) and hose(s)", slot, count);
}

void Reset() {
    g_known.clear();
    for (SlotBudget& b : g_budget) b = SlotBudget{};
}

bool PollDue() { return std::chrono::steady_clock::now() - g_lastPoll >= kPollEvery; }

}  // namespace coop::irrigation_sync::detail::host
