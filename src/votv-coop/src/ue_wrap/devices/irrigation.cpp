// ue_wrap/devices/irrigation.cpp -- see ue_wrap/devices/irrigation.h.

#include "ue_wrap/devices/irrigation.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstring>

namespace ue_wrap::irrigation {
namespace {

namespace R  = reflection;
namespace P  = profile;
namespace SR = save_record;

void* FaucetClass() { return object_index::ClassByName(P::name::IrrigationFaucetClass); }
void* HoseClass() { return object_index::ClassByName(P::name::IrrigationHoseClass); }
void* SprinklerClass() { return object_index::ClassByName(P::name::IrrigationSprinklerClass); }

bool IsOf(void* actor, void* base) {
    if (!actor || !base) return false;
    void* cls = R::ClassOf(actor);
    return cls && (cls == base || R::IsDescendantOfAny(cls, &base, 1));
}

// One member of a class, by name and type, asked again when a new world re-creates the class.
struct Member {
    void* (*base)();
    const wchar_t* name;
    const wchar_t* type;   // FFieldClass name; "BoolProperty" takes the bit mask
    CachedObjRef   cls;
    int32_t        off = -1;
    uint8_t        mask = 0;
    bool           ok = false;
    bool           said = false;
};

Member g_faucetActive{&FaucetClass, P::name::IrrigationFaucetActiveProp, L"BoolProperty", {}};
Member g_faucetHose{&FaucetClass, P::name::IrrigationFaucetHoseProp, L"ObjectProperty", {}};
Member g_hoseFaucet{&HoseClass, P::name::IrrigationHoseFaucetProp, L"ObjectProperty", {}};
Member g_hoseSprinkler{&HoseClass, P::name::IrrigationHoseSprinklerProp, L"ObjectProperty", {}};
Member g_hosePoints{&HoseClass, P::name::IrrigationHosePointsProp, L"ArrayProperty", {}};
Member g_hoseDoCollect{&HoseClass, P::name::IrrigationHoseDoCollectProp, L"BoolProperty", {}};
Member g_sprinklerHose{&SprinklerClass, P::name::IrrigationSprinklerHoseProp, L"ObjectProperty", {}};

bool Resolve(Member& m) {
    void* cls = m.base();
    if (!cls) return false;
    if (m.cls.Is(cls)) return m.ok;
    m.cls.Set(cls);
    m.ok = false;
    if (std::wcscmp(m.type, L"BoolProperty") == 0) {
        int32_t off = -1;
        uint8_t mask = 0;
        m.ok = R::FindBoolProperty(cls, m.name, off, mask) && off >= 0 && mask != 0;
        m.off = off;
        m.mask = mask;
    } else {
        int32_t off = -1;
        std::wstring found;
        m.ok = R::PropertyOffsetAndClass(cls, m.name, off, found) && off >= 0 && found == m.type;
        m.off = off;
    }
    if (!m.ok && !m.said) {
        m.said = true;
        UE_LOGW("irrigation: %ls.%ls (%ls) did not resolve by name and type -- that read refuses",
                R::ClassNameOf(cls).c_str(), m.name, m.type);
    }
    return m.ok;
}

void* ReadObj(void* a, Member& m) {
    if (!a || !Resolve(m)) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(a) + m.off, sizeof(v));
    return (v && R::IsLive(v)) ? v : nullptr;
}

bool WriteObj(void* a, Member& m, void* v) {
    if (!a || !Resolve(m)) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(a) + m.off, &v, sizeof(v));
    return true;
}

bool ReadBool(void* a, Member& m, bool& out) {
    if (!a || !Resolve(m)) return false;
    out = (reinterpret_cast<const uint8_t*>(a)[m.off] & m.mask) != 0;
    return true;
}

void* Verb(void* a, const wchar_t* name) {
    void* cls = a ? R::ClassOf(a) : nullptr;
    return cls ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

bool Finite(float v) { return std::isfinite(v) != 0; }

}  // namespace

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Faucet: return "faucet";
        case Kind::Hose: return "hose";
        default: return "none";
    }
}

Kind KindOf(void* actor) {
    if (IsOf(actor, FaucetClass())) return Kind::Faucet;
    if (IsOf(actor, HoseClass())) return Kind::Hose;
    return Kind::None;
}

bool IsSprinkler(void* actor) { return IsOf(actor, SprinklerClass()); }
bool IsFaucetItem(void* actor) { return IsOf(actor, object_index::ClassByName(P::name::IrrigationFaucetItemClass)); }
bool IsHoseItem(void* actor) { return IsOf(actor, object_index::ClassByName(P::name::IrrigationHoseItemClass)); }

std::wstring KeyOf(void* a) {
    if (KindOf(a) == Kind::None) return {};
    R::FName k{0, 0};
    std::memcpy(&k, reinterpret_cast<const uint8_t*>(a) + P::off::Aactor_save_Key, sizeof(k));
    if (k.ComparisonIndex == 0 && k.Number == 0) return {};
    std::wstring s = R::ToString(k);
    return s == L"None" ? std::wstring{} : s;
}

bool SetKey(void* a, const std::wstring& key) {
    if (KindOf(a) == Kind::None || key.empty() || key == L"None") return false;
    const R::FName k = fname_utils::StringToFName(key);
    if (k.ComparisonIndex == 0) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(a) + P::off::Aactor_save_Key, &k, sizeof(k));
    return true;
}

std::wstring RegisterKey(void* a) {
    if (KindOf(a) == Kind::None) return {};
    void* fn = Verb(a, P::name::IrrigationGetKeyFn);
    if (!fn) return {};
    ParamFrame f(fn);
    if (!f.valid() || !Call(a, f)) return {};
    return KeyOf(a);
}

void* FaucetHose(void* faucet) { return KindOf(faucet) == Kind::Faucet ? ReadObj(faucet, g_faucetHose) : nullptr; }
void* HoseFaucet(void* hose) { return KindOf(hose) == Kind::Hose ? ReadObj(hose, g_hoseFaucet) : nullptr; }
void* HoseSprinkler(void* hose) { return KindOf(hose) == Kind::Hose ? ReadObj(hose, g_hoseSprinkler) : nullptr; }
void* SprinklerHose(void* s) { return IsSprinkler(s) ? ReadObj(s, g_sprinklerHose) : nullptr; }

bool SetFaucetHose(void* faucet, void* hose) {
    return KindOf(faucet) == Kind::Faucet && WriteObj(faucet, g_faucetHose, hose);
}
bool SetHoseFaucet(void* hose, void* faucet) { return KindOf(hose) == Kind::Hose && WriteObj(hose, g_hoseFaucet, faucet); }
bool SetHoseSprinkler(void* hose, void* s) { return KindOf(hose) == Kind::Hose && WriteObj(hose, g_hoseSprinkler, s); }
bool SetSprinklerHose(void* s, void* hose) { return IsSprinkler(s) && WriteObj(s, g_sprinklerHose, hose); }

bool ReadFaucetActive(void* faucet, bool& out) {
    return KindOf(faucet) == Kind::Faucet && ReadBool(faucet, g_faucetActive, out);
}

bool ReadHoseDoCollect(void* hose, bool& out) {
    return KindOf(hose) == Kind::Hose && ReadBool(hose, g_hoseDoCollect, out);
}

bool HoseSettled(void* hose) {
    if (KindOf(hose) != Kind::Hose) return false;
    void* fn = Verb(hose, L"IsActorTickEnabled");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(hose, f)) return false;
    return !f.Get<bool>(L"ReturnValue");
}

bool ReadHosePoints(void* hose, std::vector<FVector>& out) {
    out.clear();
    if (KindOf(hose) != Kind::Hose || !Resolve(g_hosePoints)) return false;
    const SR::Arr arr = SR::ReadArr(hose, g_hosePoints.off);
    if (arr.num < 0 || arr.num > 100000 || (arr.num > 0 && !arr.data)) return false;
    out.resize(static_cast<size_t>(arr.num));
    if (arr.num > 0) std::memcpy(out.data(), arr.data, static_cast<size_t>(arr.num) * sizeof(FVector));
    return true;
}

bool CaptureRecord(void* a, SR::SaveRecord& out) {
    if (KindOf(a) == Kind::None) return false;
    void* fn = Verb(a, P::name::IrrigationGetDataFn);
    return fn && SR::CaptureRecordVia(a, fn, out);
}

bool ApplyRecord(void* a, const SR::SaveRecord& r, SR::ClassResolver resolve) {
    if (KindOf(a) == Kind::None) return false;
    void* fn = Verb(a, P::name::IrrigationLoadDataFn);
    return fn && SR::ApplyRecordVia(a, fn, r, resolve);
}

bool CallFinish(void* hose) {
    return KindOf(hose) == Kind::Hose && component_calls::CallParamlessNamed(hose, P::name::IrrigationHoseFinishFn);
}

bool CallUpd(void* a) {
    if (KindOf(a) != Kind::Faucet && !IsSprinkler(a)) return false;
    return component_calls::CallParamlessNamed(a, P::name::IrrigationUpdFn);
}

bool CallUpdEffect(void* hose) {
    return KindOf(hose) == Kind::Hose && component_calls::CallParamlessNamed(hose, P::name::IrrigationHoseUpdEffectFn);
}

bool CallDetachSprinkler(void* hose) {
    return KindOf(hose) == Kind::Hose && component_calls::CallParamlessNamed(hose, P::name::IrrigationHoseDetachFn);
}

void* Spawn(Kind k, const SR::SaveRecord& at, const std::wstring& key) {
    void* cls = k == Kind::Faucet ? FaucetClass() : k == Kind::Hose ? HoseClass() : nullptr;
    if (!cls) return nullptr;
    for (float v : at.xform)
        if (!Finite(v)) return nullptr;
    const FVector loc{at.xform[4], at.xform[5], at.xform[6]};
    const FRotator rot = engine::QuatToRotator(at.xform[0], at.xform[1], at.xform[2], at.xform[3]);
    void* a = engine::BeginDeferredSpawn(cls, loc, rot);
    if (!a) return nullptr;
    if (!key.empty()) SetKey(a, key);
    if (!engine::FinishDeferredSpawn(a, loc, rot) || !R::IsLive(a)) return nullptr;
    return a;
}

std::vector<void*> AllOf(Kind k) {
    std::vector<void*> found;
    void* cls = k == Kind::Faucet ? FaucetClass() : k == Kind::Hose ? HoseClass() : nullptr;
    if (!cls) return found;
    object_index::ForEachInstance(
        cls, [](void* ctx, void* obj, int32_t) { static_cast<std::vector<void*>*>(ctx)->push_back(obj); }, &found);
    void* world = world_identity::CurrentWorld();
    std::vector<void*> out;
    out.reserve(found.size());
    for (void* obj : found) {
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (world && world_identity::WorldOf(obj) != world) continue;
        out.push_back(obj);
    }
    return out;
}

}  // namespace ue_wrap::irrigation
