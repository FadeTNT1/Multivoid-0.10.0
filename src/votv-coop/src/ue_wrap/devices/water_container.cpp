// ue_wrap/devices/water_container.cpp -- see ue_wrap/devices/water_container.h.

#include "ue_wrap/devices/water_container.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine_physics.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::water_container {
namespace {

namespace R = reflection;
namespace P = profile;

CachedObjRef g_cls;           // the class as this world holds it
int32_t      g_heightOff = -1;
int32_t      g_interpOff = -1;
int32_t      g_emptyOff = -1;
uint8_t      g_emptyMask = 0;
bool         g_resolved = false;
bool         g_said = false;

bool FloatMember(void* cls, const wchar_t* name, int32_t& off) {
    std::wstring found;
    return R::PropertyOffsetAndClass(cls, name, off, found) && off >= 0 && found == L"FloatProperty";
}

}  // namespace

bool EnsureResolved() {
    void* cls = object_index::ClassByName(P::name::BucketClass);
    if (!cls) return g_resolved;
    if (g_resolved && g_cls.Is(cls)) return true;
    int32_t h = -1, hi = -1, e = -1;
    uint8_t mask = 0;
    const bool ok = FloatMember(cls, P::name::BucketHeightProp, h) &&
                    FloatMember(cls, P::name::BucketHeightInterpProp, hi) &&
                    R::FindBoolProperty(cls, P::name::BucketEmptyProp, e, mask) && e >= 0 && mask != 0;
    g_cls.Set(cls);
    if (!ok) {
        g_resolved = false;
        if (!g_said) {
            g_said = true;
            UE_LOGW("water_container: %ls height / height_interp / empty did not resolve by name and type -- the "
                    "bucket fill sync is left out", P::name::BucketClass);
        }
        return false;
    }
    g_heightOff = h;
    g_interpOff = hi;
    g_emptyOff = e;
    g_emptyMask = mask;
    g_resolved = true;
    UE_LOGI("water_container: resolved %ls height@0x%04X height_interp@0x%04X empty@0x%04X", P::name::BucketClass, h, hi, e);
    return true;
}

bool IsBucket(void* obj) {
    if (!g_resolved || !obj) return false;
    void* base = g_cls.Get();
    void* cls = R::ClassOf(obj);
    return base && cls && (cls == base || R::IsDescendantOfAny(cls, &base, 1));
}

bool ReadBucket(void* bucket, Bucket& out) {
    if (!g_resolved || !bucket) return false;
    const char* b = reinterpret_cast<const char*>(bucket);
    Bucket v;
    std::memcpy(&v.height, b + g_heightOff, sizeof(float));
    std::memcpy(&v.interp, b + g_interpOff, sizeof(float));
    v.empty = (static_cast<uint8_t>(b[g_emptyOff]) & g_emptyMask) != 0;
    out = v;
    return true;
}

bool WriteBucket(void* bucket, float height, bool empty, bool snapInterp) {
    if (!g_resolved || !bucket) return false;
    char* b = reinterpret_cast<char*>(bucket);
    std::memcpy(b + g_heightOff, &height, sizeof(float));
    if (snapInterp) std::memcpy(b + g_interpOff, &height, sizeof(float));
    uint8_t& byte = reinterpret_cast<uint8_t&>(b[g_emptyOff]);
    byte = empty ? static_cast<uint8_t>(byte | g_emptyMask) : static_cast<uint8_t>(byte & ~g_emptyMask);
    return true;
}

bool RepaintBucket(void* bucket) { return bucket && component_calls::CallParamlessNamed(bucket, P::name::BucketUpdFn); }

bool BucketSimulating(void* bucket) {
    void* mesh = bucket ? prop::GetStaticMesh(bucket) : nullptr;
    return mesh && engine::IsComponentSimulatingPhysics(mesh);
}

}  // namespace ue_wrap::water_container
