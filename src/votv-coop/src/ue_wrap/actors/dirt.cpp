// ue_wrap/actors/dirt.cpp -- see ue_wrap/actors/dirt.h.

#include "ue_wrap/actors/dirt.h"

#include "ue_wrap/actors/inventory_item_codec.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstring>

namespace ue_wrap::dirt {
namespace {

namespace R = reflection;
namespace N = profile::name;
namespace codec = inventory_item_codec;

void* ClassOfName(const wchar_t* name) { return object_index::ClassByName(name); }

bool Descends(void* cls, void* base) { return cls && base && R::IsDescendantOfAny(cls, &base, 1); }

bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }
bool Finite(const FRotator& r) { return std::isfinite(r.Pitch) && std::isfinite(r.Yaw) && std::isfinite(r.Roll); }

// The derived classes of a base, rebuilt when the index's class set moves.
struct Census {
    uint64_t           version = ~0ull;
    void*              base = nullptr;
    std::vector<void*> classes;
};

struct ClassCollect {
    void*               base;
    std::vector<void*>* out;
};

void CollectClass(void* ctx, void* cls, void*) {
    auto* c = static_cast<ClassCollect*>(ctx);
    if (Descends(cls, c->base)) c->out->push_back(cls);
}

std::vector<void*> Instances(Census& c, void* base) {
    std::vector<void*> out;
    if (!base) return out;
    const uint64_t v = object_index::ClassSetVersion();
    if (c.version != v || c.base != base) {
        c.classes.clear();
        ClassCollect cc{base, &c.classes};
        object_index::ForEachClass(&CollectClass, &cc);
        c.version = v;
        c.base = base;
    }
    std::vector<void*> found;
    for (void* cls : c.classes)
        object_index::ForEachInstance(
            cls, [](void* ctx, void* obj, int32_t) { static_cast<std::vector<void*>*>(ctx)->push_back(obj); }, &found);
    void* world = world_identity::CurrentWorld();
    for (void* obj : found) {
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (world && world_identity::WorldOf(obj) != world) continue;
        out.push_back(obj);
    }
    return out;
}

Census g_holes, g_mounds, g_plants;

R::InstanceOffset g_matOff{N::DirtHoleMatTypeProp};
R::InstanceOffset g_betterOff{N::DirtMoundBetterDataProp};
R::InstanceOffset g_plantOff{N::GrowingPlantNameProp};

}  // namespace

bool IsHoleClass(void* cls) { return Descends(cls, ClassOfName(N::DirtHoleClass)); }
bool IsMoundClass(void* cls) { return Descends(cls, ClassOfName(N::DirtMoundClass)); }
bool IsPlantClass(void* cls) { return Descends(cls, ClassOfName(N::GrowingPlantClass)); }
bool IsHole(void* obj) { return obj && R::IsLive(obj) && IsHoleClass(R::ClassOf(obj)); }
bool IsMound(void* obj) { return obj && R::IsLive(obj) && IsMoundClass(R::ClassOf(obj)); }
bool IsPlant(void* obj) { return obj && R::IsLive(obj) && IsPlantClass(R::ClassOf(obj)); }
bool IsDirtBall(void* obj) { return obj && R::IsLive(obj) && Descends(R::ClassOf(obj), ClassOfName(N::DirtBallClass)); }

std::vector<void*> AllHoles() { return Instances(g_holes, ClassOfName(N::DirtHoleClass)); }
std::vector<void*> AllMounds() { return Instances(g_mounds, ClassOfName(N::DirtMoundClass)); }
std::vector<void*> AllPlants() { return Instances(g_plants, ClassOfName(N::GrowingPlantClass)); }

bool ReadHole(void* hole, HoleState& out) {
    if (!IsHole(hole)) return false;
    if (!engine::TryGetActorLocation(hole, out.location) || !engine::TryGetActorRotation(hole, out.rotation)) return false;
    if (!Finite(out.location) || !Finite(out.rotation)) return false;
    out.mat = 0;
    const int32_t off = g_matOff.Of(hole);
    if (off >= 0) std::memcpy(&out.mat, reinterpret_cast<uint8_t*>(hole) + off, sizeof(out.mat));
    return true;
}

void* SpawnHole(void* cls, const HoleState& at) {
    if (!IsHoleClass(cls) || !Finite(at.location) || !Finite(at.rotation)) return nullptr;
    void* a = engine::BeginDeferredSpawn(cls, at.location, at.rotation);
    if (!a) return nullptr;
    const int32_t off = g_matOff.Of(a);
    if (off >= 0) std::memcpy(reinterpret_cast<uint8_t*>(a) + off, &at.mat, sizeof(at.mat));
    if (!engine::FinishDeferredSpawn(a, at.location, at.rotation) || !R::IsLive(a)) return nullptr;
    return a;
}

bool ReadBurial(void* mound, FVector& loc, FRotator& rot, std::vector<uint8_t>& blob) {
    if (!IsMound(mound) || !codec::BetterSaveReady()) return false;
    const int32_t off = g_betterOff.Of(mound);
    if (off < 0) return false;
    const auto* st = reinterpret_cast<const uint8_t*>(mound) + off;
    if (codec::BetterSaveClassPath(st).empty()) return false;   // not a burial: a loot mound
    if (!engine::TryGetActorLocation(mound, loc) || !engine::TryGetActorRotation(mound, rot)) return false;
    if (!Finite(loc) || !Finite(rot)) return false;
    blob.clear();
    return codec::EncodeBetterSave(st, blob);
}

void* SpawnMound(void* cls, const FVector& loc, const FRotator& rot, const std::vector<uint8_t>& blob) {
    if (!IsMoundClass(cls) || !Finite(loc) || !Finite(rot) || blob.empty() || !codec::BetterSaveReady()) return nullptr;
    void* a = engine::BeginDeferredSpawn(cls, loc, rot);
    if (!a) return nullptr;
    const int32_t off = g_betterOff.Of(a);
    size_t o = 0;
    if (off < 0 || !codec::DecodeBetterSave(blob, o, reinterpret_cast<uint8_t*>(a) + off) || o != blob.size()) {
        engine::DestroyActor(a);
        return nullptr;
    }
    if (!engine::FinishDeferredSpawn(a, loc, rot) || !R::IsLive(a)) return nullptr;
    return a;
}

bool ReadPlantName(void* plant, std::wstring& name) {
    if (!IsPlant(plant)) return false;
    const int32_t off = g_plantOff.Of(plant);
    if (off < 0) return false;
    R::FName fn{};
    std::memcpy(&fn, reinterpret_cast<uint8_t*>(plant) + off, sizeof(fn));
    name = R::ToString(fn);
    if (name == L"None") name.clear();
    return !name.empty();
}

void* SpawnPlant(void* cls, const FVector& loc, const FRotator& rot, const std::wstring& name) {
    if (!IsPlantClass(cls) || !Finite(loc) || !Finite(rot) || name.empty()) return nullptr;
    const R::FName fn = fname_utils::StringToFName(name);
    if (fn.ComparisonIndex == 0) return nullptr;
    void* a = engine::BeginDeferredSpawn(cls, loc, rot);
    if (!a) return nullptr;
    const int32_t off = g_plantOff.Of(a);
    if (off < 0) {
        engine::DestroyActor(a);
        return nullptr;
    }
    std::memcpy(reinterpret_cast<uint8_t*>(a) + off, &fn, sizeof(fn));
    if (!engine::FinishDeferredSpawn(a, loc, rot) || !R::IsLive(a)) return nullptr;
    // The Blueprint calls upd(false) on the plant it spawns: it reads the plant's row and builds its look.
    if (void* fnUpd = R::FindDispatchFunctionCached(R::ClassOf(a), N::GrowingPlantUpdFn)) {
        ParamFrame pf(fnUpd);
        const uint8_t skip = 0;
        pf.SetRaw(L"skipDataTable", &skip, 1);
        Call(a, pf);
    }
    return a;
}

bool ReadItemRecord(void* item, std::vector<uint8_t>& blob) {
    if (!item || !R::IsLive(item) || !codec::BetterSaveReady()) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(item), N::BetterGetDataFn);
    if (!fn) return false;
    ParamFrame pf(fn);
    if (!pf.valid() || !Call(item, pf)) return false;
    const int32_t off = pf.ParamOffset(N::BetterDataParam);
    if (off < 0 || !pf.data()) return false;
    blob.clear();
    return codec::EncodeBetterSave(static_cast<const uint8_t*>(pf.data()) + off, blob);
}

void* SpawnItem(void* cls, const FVector& loc, const std::vector<uint8_t>& blob) {
    if (!cls || !Finite(loc)) return nullptr;
    void* a = engine::BeginDeferredSpawn(cls, loc, FRotator{});
    if (!a) return nullptr;
    if (!engine::FinishDeferredSpawn(a, loc, FRotator{}) || !R::IsLive(a)) return nullptr;
    if (blob.empty()) return a;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(a), N::BetterLoadDataFn);
    if (!fn || !codec::BetterSaveReady()) return a;   // a record this build cannot load: the item as it was born
    ParamFrame pf(fn);
    const int32_t off = pf.valid() ? pf.ParamOffset(N::BetterDataParam) : -1;
    size_t o = 0;
    if (off >= 0 && pf.data() && codec::DecodeBetterSave(blob, o, static_cast<uint8_t*>(pf.data()) + off) && o == blob.size())
        Call(a, pf);
    return a;
}

bool FilterBlocks(void* hole, void* item) {
    if (!IsHole(hole) || !item) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(hole), N::FilterItemsFn);
    if (!fn) return false;
    ParamFrame pf(fn);
    if (!pf.valid()) return false;
    pf.SetRaw(N::FilterItemsActorParam, &item, static_cast<int32_t>(sizeof(item)));
    if (!Call(hole, pf)) return false;
    return pf.Get<uint8_t>(N::FilterItemsBlockedParam) != 0;
}

bool ProcessBuried(void* hole, void* item, bool& ok) {
    ok = false;
    if (!IsHole(hole) || !item) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(hole), N::ProcessBuriedItemFn);
    if (!fn) return false;
    ParamFrame pf(fn);
    if (!pf.valid()) return false;
    pf.SetRaw(N::ProcessBuriedItemActorParam, &item, static_cast<int32_t>(sizeof(item)));
    if (!Call(hole, pf)) return false;
    ok = pf.Get<uint8_t>(L"return") != 0;
    return true;
}

}  // namespace ue_wrap::dirt
