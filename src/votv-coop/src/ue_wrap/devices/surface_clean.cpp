// ue_wrap/devices/surface_clean.cpp -- see ue_wrap/devices/surface_clean.h.

#include "ue_wrap/devices/surface_clean.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstring>

namespace ue_wrap::surface_clean {
namespace {

namespace R = reflection;
namespace P = profile;

struct Desc {
    const wchar_t* className;
    CachedObjRef   cls;       // the class as this world holds it (a Blueprint class can be a new object in a new world)
    int32_t        keyOff = -1;
    int32_t        cleanOff = -1;
    bool           resolved = false;
    bool           unusable = false;
};
Desc g_descs[] = {{P::name::SinkClass, {}}, {P::name::ShowerClass, {}}, {P::name::ToiletClass, {}}};
constexpr int kNum = sizeof(g_descs) / sizeof(g_descs[0]);

void* ClassFor(Desc& d) {
    if (void* c = d.cls.Get()) return c;
    void* c = object_index::ClassByName(d.className);
    if (c) d.cls.Set(c);
    return c;
}

Desc* DescFor(void* obj) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    if (!cls) return nullptr;
    for (Desc& d : g_descs) {
        if (!d.resolved) continue;
        void* base = ClassFor(d);
        if (base && (cls == base || R::IsDescendantOfAny(cls, &base, 1))) return &d;
    }
    return nullptr;
}

}  // namespace

bool EnsureResolved() {
    int n = 0;
    for (Desc& d : g_descs) {
        if (d.resolved) { ++n; continue; }
        if (d.unusable) continue;
        void* cls = ClassFor(d);
        if (!cls) continue;
        std::wstring found;
        int32_t cleanOff = -1;
        const bool cleanOk = R::PropertyOffsetAndClass(cls, P::name::SurfaceCleanProp, cleanOff, found) &&
                             cleanOff >= 0 && found == L"FloatProperty";
        const int32_t keyOff = R::FindPropertyOffset(cls, L"Key");
        if (!cleanOk || keyOff < 0) {
            d.unusable = true;
            UE_LOGE("surface_clean: %ls.%ls did not resolve by name and type -- this surface is left out of the clean sync",
                    d.className, cleanOk ? L"Key" : P::name::SurfaceCleanProp);
            continue;
        }
        d.keyOff = keyOff;
        d.cleanOff = cleanOff;
        d.resolved = true;
        ++n;
        UE_LOGI("surface_clean: resolved %ls Key@0x%04X clean@0x%04X", d.className, keyOff, cleanOff);
    }
    return n > 0;
}

uint64_t ResolvedClassCount() {
    uint64_t n = 0;
    for (const Desc& d : g_descs) n += d.resolved ? 1u : 0u;
    return n;
}

bool IsSurface(void* obj) { return DescFor(obj) != nullptr; }

bool IsSink(void* obj) {
    Desc* d = DescFor(obj);
    return d && std::wcscmp(d->className, P::name::SinkClass) == 0;
}

std::wstring GetKeyString(void* surface) {
    const Desc* d = DescFor(surface);
    if (!d || d->keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(reinterpret_cast<const char*>(surface) + d->keyOff);
    return R::ToString(key);
}

bool ReadClean(void* surface, float& out) {
    const Desc* d = DescFor(surface);
    if (!d || d->cleanOff < 0) return false;
    std::memcpy(&out, reinterpret_cast<const char*>(surface) + d->cleanOff, sizeof(float));
    return true;
}

bool WriteCleanAndPaint(void* surface, float clean) {
    const Desc* d = DescFor(surface);
    if (!d || d->cleanOff < 0) return false;
    std::memcpy(reinterpret_cast<char*>(surface) + d->cleanOff, &clean, sizeof(float));
    return component_calls::CallParamlessNamed(surface, P::name::SurfaceUpdFn);
}

}  // namespace ue_wrap::surface_clean
