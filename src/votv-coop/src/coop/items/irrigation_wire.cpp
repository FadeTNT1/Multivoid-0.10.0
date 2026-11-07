// coop/items/irrigation_wire.cpp -- the irrigation lane's grammar, lookups and the apply both ends run. See
// coop/items/irrigation_detail.h.

#include "irrigation_detail.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>
#include <cstring>

namespace coop::irrigation_sync::detail {
namespace {

namespace IR = ue_wrap::irrigation;
namespace W = coop::save_record_wire;
namespace R = ue_wrap::reflection;

const std::wstring kNone = L"None";

bool KindValid(uint8_t k) { return k == static_cast<uint8_t>(IR::Kind::Faucet) || k == static_cast<uint8_t>(IR::Kind::Hose); }

std::wstring KeyOrNone(const std::wstring& k) { return k.empty() ? kNone : k; }

void RepaintHose(void* hose) {
    if (!hose) return;
    IR::CallUpdEffect(hose);
}

}  // namespace

bool KeyFits(const std::wstring& key) { return !key.empty() && key != kNone && key.size() <= 63; }

void WriteCommit(std::vector<uint8_t>& b, const Commit& c) {
    W::AppU8(b, kWireVersion);
    W::AppU8(b, static_cast<uint8_t>(c.op));
    switch (c.op) {
        case CommitOp::PlaceFaucet:
        case CommitOp::CommitHose: W::SerSave(b, c.rec); break;
        case CommitOp::TakeHose:
            W::AppWStr(b, c.key);
            W::AppU8(b, c.collect ? 1 : 0);
            break;
        default: W::AppWStr(b, c.key); break;
    }
}

bool ReadCommit(const std::vector<uint8_t>& b, Commit& out) {
    size_t o = 0;
    uint8_t ver = 0, op = 0;
    if (!W::RdU8(b, o, ver) || ver != kWireVersion || !W::RdU8(b, o, op)) return false;
    out.op = static_cast<CommitOp>(op);
    switch (out.op) {
        case CommitOp::PlaceFaucet:
        case CommitOp::CommitHose: return W::DeSave(b, o, out.rec);
        case CommitOp::TakeHose: {
            uint8_t c = 0;
            if (!W::RdWStr(b, o, out.key) || !W::RdU8(b, o, c)) return false;
            out.collect = c != 0;
            return true;
        }
        case CommitOp::TakeFaucet:
        case CommitOp::DetachSprinkler: return W::RdWStr(b, o, out.key);
        default: return false;
    }
}

void WriteState(std::vector<uint8_t>& b, const State& s) {
    W::AppU8(b, kWireVersion);
    W::AppU8(b, static_cast<uint8_t>(s.op));
    switch (s.op) {
        case StateOp::Upsert:
            W::AppU8(b, static_cast<uint8_t>(s.kind));
            W::SerSave(b, s.rec);
            break;
        case StateOp::Remove:
            W::AppU8(b, static_cast<uint8_t>(s.kind));
            W::AppWStr(b, s.key);
            break;
        case StateOp::SnapEnd: W::AppU32(b, s.count); break;
        default: break;
    }
}

bool ReadState(const std::vector<uint8_t>& b, State& out) {
    size_t o = 0;
    uint8_t ver = 0, op = 0, kind = 0;
    if (!W::RdU8(b, o, ver) || ver != kWireVersion || !W::RdU8(b, o, op)) return false;
    out.op = static_cast<StateOp>(op);
    switch (out.op) {
        case StateOp::Upsert:
            if (!W::RdU8(b, o, kind) || !KindValid(kind)) return false;
            out.kind = static_cast<IR::Kind>(kind);
            return W::DeSave(b, o, out.rec);
        case StateOp::Remove:
            if (!W::RdU8(b, o, kind) || !KindValid(kind)) return false;
            out.kind = static_cast<IR::Kind>(kind);
            return W::RdWStr(b, o, out.key);
        case StateOp::SnapBegin: return true;
        case StateOp::SnapEnd: return W::RdU32(b, o, out.count);
        default: return false;
    }
}

void* Find(IR::Kind k, const std::wstring& key) {
    if (!KeyFits(key)) return nullptr;
    for (void* a : IR::AllOf(k))
        if (IR::KeyOf(a) == key) return a;
    return nullptr;
}

void* FindSprinkler(const std::wstring& key) {
    if (!KeyFits(key)) return nullptr;
    void* a = coop::prop_key_index::Find(key);
    return IR::IsSprinkler(a) ? a : nullptr;
}

std::wstring SprinklerKey(void* sprinkler) {
    if (!IR::IsSprinkler(sprinkler)) return {};
    std::wstring k = ue_wrap::prop::GetInteractableKeyString(sprinkler);
    return k == kNone ? std::wstring{} : k;
}

bool WireRecord(void* a, IR::Kind k, const std::wstring& key, SaveRecord& out) {
    out = SaveRecord{};
    if (!IR::CaptureRecord(a, out)) return false;
    out.key = key;
    out.names.clear();
    if (k == IR::Kind::Faucet) {
        out.names.push_back({KeyOrNone(IR::KeyOf(IR::FaucetHose(a)))});
    } else {
        out.names.push_back({KeyOrNone(IR::KeyOf(IR::HoseFaucet(a))), KeyOrNone(SprinklerKey(IR::HoseSprinkler(a)))});
    }
    return true;
}

std::wstring RecordLink(const SaveRecord& r, size_t i) {
    if (r.names.empty() || r.names[0].size() <= i) return {};
    const std::wstring& s = r.names[0][i];
    return s == kNone ? std::wstring{} : s;
}

void LinkHose(void* hose, void* faucet, void* sprinkler) {
    if (IR::KindOf(hose) != IR::Kind::Hose) return;
    void* oldF = IR::HoseFaucet(hose);
    void* oldS = IR::HoseSprinkler(hose);
    if (oldF && oldF != faucet && IR::FaucetHose(oldF) == hose) {
        IR::SetFaucetHose(oldF, nullptr);
        IR::CallUpd(oldF);
    }
    if (oldS && oldS != sprinkler && IR::SprinklerHose(oldS) == hose) {
        IR::SetSprinklerHose(oldS, nullptr);
        IR::CallUpd(oldS);
    }
    if (faucet) {
        void* prev = IR::FaucetHose(faucet);
        if (prev && prev != hose && IR::HoseFaucet(prev) == faucet) {
            IR::SetHoseFaucet(prev, nullptr);
            RepaintHose(prev);
        }
    }
    if (sprinkler) {
        void* prev = IR::SprinklerHose(sprinkler);
        if (prev != hose) {
            if (prev && IR::HoseSprinkler(prev) == sprinkler) {
                IR::SetHoseSprinkler(prev, nullptr);
                RepaintHose(prev);
            }
            // connectHose freezes the sprinkler it takes: setPropProps(static false, frozen true, active false,
            // sleeping false).
            ue_wrap::prop::CallSetPropProps(sprinkler, /*isStatic=*/false, /*frozen=*/true, /*sleeping=*/false);
        }
    }
    IR::SetHoseFaucet(hose, faucet);
    if (faucet) IR::SetFaucetHose(faucet, hose);
    IR::SetHoseSprinkler(hose, sprinkler);
    if (sprinkler) IR::SetSprinklerHose(sprinkler, hose);
    // finish(): tick off, the spline meshes rebuilt from `points`, collision on, the faucet's upd (which repaints the
    // hose's water and its sprinkler). A hose with no faucet repaints itself, since finish() reaches no upd then.
    IR::CallFinish(hose);
    if (!faucet) RepaintHose(hose);
    if (sprinkler) IR::CallUpd(sprinkler);
}

void* ApplyFaucet(const SaveRecord& r, ue_wrap::save_record::ClassResolver resolve, bool& created) {
    created = false;
    void* a = Find(IR::Kind::Faucet, r.key);
    if (!a) {
        a = IR::Spawn(IR::Kind::Faucet, r, r.key);
        if (!a) return nullptr;
        created = true;
        // Its own loadData: the key, `active` and the saved hose key. Only on a birth here: `active` afterwards is the
        // appliance lane's, and a later state of this faucet carries a change of its hose alone.
        IR::ApplyRecord(a, r, resolve);
        IR::RegisterKey(a);
    }
    void* hose = Find(IR::Kind::Hose, RecordLink(r, 0));
    void* cur = IR::FaucetHose(a);
    if (hose != cur) {
        if (cur && IR::HoseFaucet(cur) == a) {
            IR::SetHoseFaucet(cur, nullptr);
            RepaintHose(cur);
        }
        IR::SetFaucetHose(a, hose);
        if (hose) IR::SetHoseFaucet(hose, a);
    }
    IR::CallUpd(a);
    return a;
}

void* ApplyHose(const SaveRecord& r, ue_wrap::save_record::ClassResolver resolve, bool& created) {
    created = false;
    void* a = Find(IR::Kind::Hose, r.key);
    if (!a) {
        a = IR::Spawn(IR::Kind::Hose, r, r.key);
        if (!a) return nullptr;
        created = true;
    }
    // Its own loadData: the key, `points` and the two saved link keys.
    IR::ApplyRecord(a, r, resolve);
    IR::RegisterKey(a);
    LinkHose(a, Find(IR::Kind::Faucet, RecordLink(r, 0)), FindSprinkler(RecordLink(r, 1)));
    return a;
}

void UnlinkAndDestroy(void* a) {
    const IR::Kind k = IR::KindOf(a);
    void* faucet = nullptr;
    void* sprinkler = nullptr;
    void* hose = nullptr;
    if (k == IR::Kind::Faucet) {
        hose = IR::FaucetHose(a);
        if (hose && IR::HoseFaucet(hose) == a) IR::SetHoseFaucet(hose, nullptr);
        else hose = nullptr;
    } else if (k == IR::Kind::Hose) {
        faucet = IR::HoseFaucet(a);
        if (faucet && IR::FaucetHose(faucet) == a) IR::SetFaucetHose(faucet, nullptr);
        else faucet = nullptr;
        sprinkler = IR::HoseSprinkler(a);
        if (sprinkler && IR::SprinklerHose(sprinkler) == a) IR::SetSprinklerHose(sprinkler, nullptr);
        else sprinkler = nullptr;
    } else {
        return;
    }
    ue_wrap::engine::DestroyActor(a);
    if (faucet) IR::CallUpd(faucet);
    if (sprinkler) IR::CallUpd(sprinkler);
    if (hose) RepaintHose(hose);
}

uint64_t Signature(void* a, IR::Kind k) {
    std::vector<uint8_t> b;
    W::AppWStr(b, IR::KeyOf(a));
    if (k == IR::Kind::Faucet) {
        W::AppWStr(b, IR::KeyOf(IR::FaucetHose(a)));
    } else {
        W::AppWStr(b, IR::KeyOf(IR::HoseFaucet(a)));
        W::AppWStr(b, SprinklerKey(IR::HoseSprinkler(a)));
        std::vector<ue_wrap::FVector> pts;
        IR::ReadHosePoints(a, pts);
        W::AppU32(b, static_cast<uint32_t>(pts.size()));
        for (const auto& p : pts) {
            W::AppF32(b, p.X);
            W::AppF32(b, p.Y);
            W::AppF32(b, p.Z);
        }
    }
    return coop::blob_chunks::Fnv64(b);
}

}  // namespace coop::irrigation_sync::detail
