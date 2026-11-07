// ue_wrap/actors/save_actor.cpp -- see ue_wrap/actors/save_actor.h.

#include "ue_wrap/actors/save_actor.h"

#include "ue_wrap/actors/inventory_item_codec.h"  // the struct_betterSave codec
#include "ue_wrap/actors/prop.h"          // GetActorSaveKeyString: the class-gated key read
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace ue_wrap::save_actor {
namespace {

namespace R  = reflection;
namespace E  = engine;
namespace P  = profile;
namespace N  = profile::name;
namespace SR = save_record;
namespace codec = inventory_item_codec;

constexpr size_t kCount = sizeof(N::kPlayerDeployedSaveActorClasses) / sizeof(N::kPlayerDeployedSaveActorClasses[0]);

// One allowlisted class as Refresh last resolved it: the class object and the listing the object index gave it, so a
// pointer to a class unloaded since never answers for whatever now lives at its address.
struct Slot {
    void*    cls = nullptr;
    uint64_t listing = 0;
};
Slot g_slots[kCount];

// actor_save_C's skipSave, a bool UPROPERTY: a byte offset and a bit mask, resolved off the base class.
int32_t g_skipOff  = -1;
uint8_t g_skipMask = 0;
void*   g_skipFrom = nullptr;   // the base class object the offset was read off
bool    g_saidNoSkip = false;

void* ActorSaveBase() { return object_index::ClassByName(N::ActorSaveClass); }

bool IsActorSave(void* actor) {
    if (!actor) return false;
    void* base = ActorSaveBase();
    return base && R::IsDescendantOfAny(R::ClassOf(actor), &base, 1);
}

bool ResolveSkipSave() {
    void* base = ActorSaveBase();
    if (!base) return false;
    if (g_skipFrom == base && g_skipOff >= 0) return true;
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(base, N::ActorSaveSkipSaveProp, off, mask) || off < 0 || mask == 0) {
        if (!g_saidNoSkip) {
            g_saidNoSkip = true;
            UE_LOGE("save_actor: %ls::%ls did not resolve -- no mirror of a deployed actor can be kept out of the save, "
                    "so none is spawned", N::ActorSaveClass, N::ActorSaveSkipSaveProp);
        }
        return false;
    }
    g_skipOff = off;
    g_skipMask = mask;
    g_skipFrom = base;
    return true;
}

bool MakeName(const std::wstring& s, R::FName& out) {
    if (s.empty() || s == L"None") {
        out = R::FName{0, 0};
        return true;
    }
    out = fname_utils::StringToFName(s);
    return out.ComparisonIndex != 0;
}

void* DispatchFn(void* actor, const wchar_t* name) {
    return actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), name) : nullptr;
}

bool Finite(float v) { return std::isfinite(v); }

}  // namespace

size_t Count() { return kCount; }

const wchar_t* ClassNameAt(size_t idx) { return idx < kCount ? N::kPlayerDeployedSaveActorClasses[idx] : L""; }

void Refresh() {
    for (size_t i = 0; i < kCount; ++i) {
        void* cls = object_index::ClassByName(N::kPlayerDeployedSaveActorClasses[i]);
        g_slots[i].cls = cls;
        g_slots[i].listing = cls ? object_index::ClassListingId(cls) : 0;
    }
}

int IndexOf(void* obj) {
    if (!obj) return -1;
    void* cls = R::ClassOf(obj);
    if (!cls) return -1;
    for (size_t i = 0; i < kCount; ++i) {
        if (g_slots[i].cls != cls) continue;
        // The same pointer: is it still the class Refresh saw? A class a world unloaded and another took the address of
        // reads as a new listing.
        if (object_index::ClassListingId(cls) != g_slots[i].listing) return -1;
        return static_cast<int>(i);
    }
    return -1;
}

int IndexOfName(const std::wstring& className) {
    for (size_t i = 0; i < kCount; ++i)
        if (::_wcsicmp(className.c_str(), N::kPlayerDeployedSaveActorClasses[i]) == 0) return static_cast<int>(i);
    return -1;
}

void* ClassAt(size_t idx) {
    if (idx >= kCount) return nullptr;
    if (g_slots[idx].cls && object_index::ClassListingId(g_slots[idx].cls) == g_slots[idx].listing)
        return g_slots[idx].cls;
    void* cls = object_index::ClassByName(N::kPlayerDeployedSaveActorClasses[idx]);
    g_slots[idx].cls = cls;
    g_slots[idx].listing = cls ? object_index::ClassListingId(cls) : 0;
    return cls;
}

bool IsGamemode(void* obj) {
    if (!obj) return false;
    void* gm = object_index::ClassByName(N::GamemodeClass);
    return gm && R::IsDescendantOfAny(R::ClassOf(obj), &gm, 1);
}

std::wstring KeyOf(void* actor) {
    std::wstring k = prop::GetActorSaveKeyString(actor);
    return k == L"None" ? std::wstring{} : k;
}

bool WriteKey(void* actor, const std::wstring& key) {
    if (!IsActorSave(actor)) return false;
    R::FName n{0, 0};
    if (!MakeName(key, n)) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(actor) + P::off::Aactor_save_Key, &n, sizeof(n));
    return true;
}

std::wstring EnsureKey(void* actor) {
    if (!IsActorSave(actor)) return {};
    void* fn = DispatchFn(actor, N::ActorSaveGetKeyFn);
    if (!fn) return {};
    ParamFrame f(fn);
    if (!f.valid() || !Call(actor, f)) return {};
    R::FName n{0, 0};
    if (!f.GetRaw(N::ActorSaveGetKeyParam, &n, static_cast<int32_t>(sizeof(n)))) return {};
    if (n.ComparisonIndex == 0 && n.Number == 0) return {};
    const std::wstring s = R::ToString(n);
    return s == L"None" ? std::wstring{} : s;
}

bool WriteSkipSave(void* actor, bool on) {
    if (!IsActorSave(actor) || !ResolveSkipSave()) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(actor) + g_skipOff;
    *p = on ? static_cast<uint8_t>(*p | g_skipMask) : static_cast<uint8_t>(*p & ~g_skipMask);
    return true;
}

bool CaptureRecord(void* actor, SR::SaveRecord& out) {
    if (!IsActorSave(actor)) return false;
    void* fn = DispatchFn(actor, N::ActorSaveGetDataFn);
    return fn && SR::CaptureRecordVia(actor, fn, out);
}

bool CaptureBetter(void* actor, std::vector<uint8_t>& blob) {
    blob.clear();
    if (!IsActorSave(actor) || !codec::BetterSaveReady()) return false;
    void* fn = DispatchFn(actor, N::BetterGetDataFn);
    if (!fn) return false;
    ParamFrame pf(fn);
    if (!pf.valid() || !Call(actor, pf)) return false;
    const int32_t off = pf.ParamOffset(N::BetterDataParam);
    if (off < 0 || !pf.data()) return false;
    return codec::EncodeBetterSave(static_cast<const uint8_t*>(pf.data()) + off, blob);
}

bool ApplyRecord(void* actor, const SR::SaveRecord& r, SR::ClassResolver resolve) {
    if (!IsActorSave(actor)) return false;
    void* load = DispatchFn(actor, N::ActorSaveLoadDataFn);
    return load && SR::ApplyRecordVia(actor, load, r, resolve);
}

bool ApplyBetter(void* actor, const std::vector<uint8_t>& blob) {
    if (blob.empty() || !IsActorSave(actor) || !codec::BetterSaveReady()) return false;
    void* fn = DispatchFn(actor, N::BetterLoadDataFn);
    if (!fn) return false;
    ParamFrame pf(fn);
    const int32_t off = pf.valid() ? pf.ParamOffset(N::BetterDataParam) : -1;
    if (off < 0 || !pf.data()) return false;
    size_t o = 0;
    // The frame is zeroed, as the decode asks: nothing in it is freed, and every buffer the decode makes is the
    // engine's, handed to the call.
    if (!codec::DecodeBetterSave(blob, o, static_cast<uint8_t*>(pf.data()) + off) || o != blob.size()) return false;
    return Call(actor, pf);
}

bool ProcessKeys(void* actor) {
    if (!IsActorSave(actor)) return false;
    void* pk = DispatchFn(actor, N::ActorSaveProcessKeysFn);
    if (!pk) return false;
    ParamFrame f(pk);
    return f.valid() && Call(actor, f);
}

void* Spawn(void* cls, const SR::SaveRecord& r, const std::wstring& key, bool skipSave) {
    if (!cls) return nullptr;
    for (float v : r.xform)
        if (!Finite(v)) return nullptr;
    const FVector  loc{r.xform[4], r.xform[5], r.xform[6]};
    const FRotator rot = E::QuatToRotator(r.xform[0], r.xform[1], r.xform[2], r.xform[3]);
    const FVector  scale{r.xform[7], r.xform[8], r.xform[9]};
    if (skipSave && !ResolveSkipSave()) return nullptr;

    void* a = E::BeginDeferredSpawn(cls, loc, rot);
    if (!a) return nullptr;
    // IN THE WINDOW: the key (so BeginPlay and any getKey there see the canonical one, and assignKey files it rather
    // than minting another) and the save flag (so no autosave sees a mirror unmarked).
    if (!key.empty() && !WriteKey(a, key)) {
        UE_LOGW("save_actor: the key '%ls' could not be written on a %ls -- the spawn is abandoned", key.c_str(),
                R::ClassNameOf(a).c_str());
        E::DestroyActor(a);
        return nullptr;
    }
    if (skipSave && !WriteSkipSave(a, true)) {
        E::DestroyActor(a);
        return nullptr;
    }
    if (!E::FinishDeferredSpawn(a, loc, rot)) {
        E::DestroyActor(a);
        return nullptr;
    }
    const bool unit = std::fabs(scale.X - 1.f) < 1e-4f && std::fabs(scale.Y - 1.f) < 1e-4f &&
                      std::fabs(scale.Z - 1.f) < 1e-4f;
    if (!unit && scale.X > 0.f && scale.Y > 0.f && scale.Z > 0.f) E::SetActorScale3D(a, scale);
    return a;
}

void ResetCache() {
    for (Slot& s : g_slots) s = Slot{};
    g_skipOff = -1;
    g_skipMask = 0;
    g_skipFrom = nullptr;
}

}  // namespace ue_wrap::save_actor
