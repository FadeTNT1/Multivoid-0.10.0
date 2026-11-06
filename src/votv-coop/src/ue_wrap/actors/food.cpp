// ue_wrap/actors/food.cpp -- see ue_wrap/actors/food.h.

#include "ue_wrap/actors/food.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cmath>
#include <cstring>
#include <string>

namespace ue_wrap::food {
namespace {

namespace R = reflection;
namespace P = profile;

// A member of one of the family's base classes, found on that class by name and checked by type: the base class
// declares it, so its offset is the same in every class below. Asked again when a new world re-creates the class.
struct Member {
    const wchar_t*        name;
    const wchar_t*        type;   // the FFieldClass name; "BoolProperty" takes the bool's own byte and mask
    CachedObjRef          cls;
    int32_t               off = -1;
    uint8_t               mask = 0;
    bool                  ok = false;
    bool                  said = false;
};

void* BaseClass(const wchar_t* name) { return object_index::ClassByName(name); }

bool Resolve(Member& m, const wchar_t* baseName) {
    void* base = BaseClass(baseName);
    if (!base) return false;
    if (m.cls.Is(base)) return m.ok;
    m.cls.Set(base);
    m.ok = false;
    m.off = -1;
    m.mask = 0;
    if (std::wcscmp(m.type, L"BoolProperty") == 0) {
        int32_t off = -1;
        uint8_t mask = 0;
        m.ok = R::FindBoolProperty(base, m.name, off, mask) && off >= 0 && mask != 0;
        if (m.ok) {
            m.off = off;
            m.mask = mask;
        }
    } else {
        int32_t off = -1;
        std::wstring found;
        m.ok = R::PropertyOffsetAndClass(base, m.name, off, found) && off >= 0 && found == m.type;
        if (m.ok) m.off = off;
    }
    if (!m.ok && !m.said) {
        m.said = true;
        UE_LOGW("food: %ls.%ls (%ls) did not resolve by name and type -- that part of the food state is not read",
                baseName, m.name, m.type);
    }
    return m.ok;
}

Member g_temperature{P::name::FoodTemperatureProp, L"FloatProperty", {}};
Member g_celsius{P::name::FoodCelsiusProp, L"FloatProperty", {}};
Member g_freshness{P::name::FoodFreshnessProp, L"FloatProperty", {}};
Member g_freshnessLeft{P::name::FoodFreshnessLeftProp, L"IntProperty", {}};
Member g_uses{P::name::FoodUsesProp, L"IntProperty", {}};
Member g_ignoreRotting{P::name::FoodIgnoreRottingProp, L"BoolProperty", {}};
Member g_spoiled{P::name::FoodSpoiledProp, L"BoolProperty", {}};
Member g_frozen{P::name::FoodFrozenProp, L"BoolProperty", {}};
Member g_steam{P::name::FoodSteamComp, L"ObjectProperty", {}};
Member g_cookProgress{P::name::FoodCookProgressProp, L"FloatProperty", {}};

bool IsChild(void* obj, const wchar_t* baseName) {
    if (!obj || !R::IsLive(obj)) return false;
    void* base = BaseClass(baseName);
    void* cls = R::ClassOf(obj);
    return base && cls && R::IsDescendantOfAny(cls, &base, 1);
}

uint8_t* At(void* obj, const Member& m) { return reinterpret_cast<uint8_t*>(obj) + m.off; }

template <class T>
T Load(void* obj, const Member& m) {
    T v{};
    std::memcpy(&v, At(obj, m), sizeof(T));
    return v;
}

template <class T>
void Store(void* obj, const Member& m, const T& v) {
    std::memcpy(At(obj, m), &v, sizeof(T));
}

bool LoadBool(void* obj, const Member& m) { return (*At(obj, m) & m.mask) != 0; }

void StoreBool(void* obj, const Member& m, bool v) {
    uint8_t& b = *At(obj, m);
    b = v ? static_cast<uint8_t>(b | m.mask) : static_cast<uint8_t>(b & ~m.mask);
}

bool ResolveFood() {
    const wchar_t* base = P::name::FoodClass;
    return Resolve(g_temperature, base) & Resolve(g_freshness, base) & Resolve(g_freshnessLeft, base) &
           Resolve(g_uses, base) & Resolve(g_ignoreRotting, base) & Resolve(g_spoiled, base);
}

// A one-parameter verb of the actor's own class, by name.
template <class T>
bool CallOne(void* obj, const wchar_t* fnName, const wchar_t* param, const T& value) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<T>(param, value) && Call(obj, f);
}

void* ComponentAt(void* obj, Member& m, const wchar_t* baseName) {
    if (!Resolve(m, baseName)) return nullptr;
    void* comp = Load<void*>(obj, m);
    return comp && R::IsLive(comp) ? comp : nullptr;
}

// prop_C::staticMesh, the prop's own mesh: found on the actor's class, which inherits it from prop_C.
R::InstanceOffset g_staticMesh{P::name::PropStaticMeshComp};

bool SetPrimitiveData(void* food, int index, float value) {
    const int32_t off = g_staticMesh.Of(food);
    if (off < 0) return false;
    void* mesh = nullptr;
    std::memcpy(&mesh, reinterpret_cast<const uint8_t*>(food) + off, sizeof(mesh));
    if (!mesh || !R::IsLive(mesh)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(mesh), P::name::SetCustomPrimitiveDataFloatFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<int32_t>(L"DataIndex", index) && f.Set<float>(L"Value", value) && Call(mesh, f);
}

}  // namespace

bool IsFood(void* obj) { return IsChild(obj, P::name::FoodClass); }
bool IsCookingFood(void* obj) { return IsChild(obj, P::name::CookingFoodClass); }

bool ReadState(void* food, State& out) {
    if (!IsFood(food) || !ResolveFood()) return false;
    State s;
    s.temperature = Load<float>(food, g_temperature);
    s.freshness = Load<float>(food, g_freshness);
    s.freshnessLeft = Load<int32_t>(food, g_freshnessLeft);
    s.uses = Load<int32_t>(food, g_uses);
    s.ignoreRotting = LoadBool(food, g_ignoreRotting);
    s.spoiled = LoadBool(food, g_spoiled);
    s.frozen = Resolve(g_frozen, P::name::FoodClass) && LoadBool(food, g_frozen);
    if (IsCookingFood(food) && Resolve(g_cookProgress, P::name::CookingFoodClass)) {
        s.hasCook = true;
        s.cookProgress = Load<float>(food, g_cookProgress);
    }
    if (!std::isfinite(s.temperature) || !std::isfinite(s.freshness) || !std::isfinite(s.cookProgress)) return false;
    out = s;
    return true;
}

bool WriteState(void* food, const State& s) {
    if (!IsFood(food) || !ResolveFood()) return false;
    Store<float>(food, g_temperature, s.temperature);
    Store<float>(food, g_freshness, s.freshness);
    Store<int32_t>(food, g_freshnessLeft, s.freshnessLeft);
    Store<int32_t>(food, g_uses, s.uses);
    StoreBool(food, g_ignoreRotting, s.ignoreRotting);
    if (s.hasCook && IsCookingFood(food) && Resolve(g_cookProgress, P::name::CookingFoodClass))
        Store<float>(food, g_cookProgress, s.cookProgress);
    return true;
}

bool RefreshDisplay(void* food, const State& now, const State& was) {
    if (!IsFood(food)) return false;
    bool drawn = false;
    if (Resolve(g_celsius, P::name::FoodClass)) {
        const float celsius = now.temperature - P::name::FoodKelvinOffset;
        Store<float>(food, g_celsius, celsius);
        if (void* steam = ComponentAt(food, g_steam, P::name::FoodClass))
            drawn = component_calls::SetActive(steam, celsius > P::name::FoodSteamCelsius, true);
    }
    if (now.spoiled != was.spoiled)
        CallOne<bool>(food, P::name::FoodSetSpoiledFn, P::name::FoodSetSpoiledParam, now.spoiled);
    if (now.frozen != was.frozen && Resolve(g_frozen, P::name::FoodClass)) {
        StoreBool(food, g_frozen, now.frozen);
        SetPrimitiveData(food, P::name::FoodFrozenDataIndex, now.frozen ? P::name::FoodFrozenDataValue : 0.f);
        CallOne<bool>(food, P::name::FoodFreezedFn, P::name::FoodFreezedParam, now.frozen);
    }
    return drawn;
}

}  // namespace ue_wrap::food
