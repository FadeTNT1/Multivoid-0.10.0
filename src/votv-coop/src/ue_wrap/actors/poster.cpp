// ue_wrap/actors/poster.cpp -- see ue_wrap/actors/poster.h.

#include "ue_wrap/actors/poster.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
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
#include <string>
#include <vector>

namespace ue_wrap::poster {
namespace {

namespace R = reflection;
namespace P = profile;

void* RolledBase() { return object_index::ClassByName(P::name::PosterRolledClass); }
void* PlacedBase() { return object_index::ClassByName(P::name::PosterPlacedClass); }

// A member of one of the two base classes, found by name and type; asked again when a new world re-creates the class.
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

Member g_rType{&RolledBase, P::name::PosterTypeProp, L"IntProperty", {}};
Member g_rIndex{&RolledBase, P::name::PosterIndexProp, L"IntProperty", {}};
Member g_rFile{&RolledBase, P::name::PosterFileNameProp, L"NameProperty", {}};
Member g_rCustom{&RolledBase, P::name::PosterCustomProp, L"BoolProperty", {}};
Member g_rClass{&RolledBase, P::name::PosterClassProp, L"ClassProperty", {}};
Member g_rTurn{&RolledBase, P::name::PosterTurnComp, L"ObjectProperty", {}};
Member g_rHolder{&RolledBase, P::name::PosterHoldingPlayerProp, L"ObjectProperty", {}};
Member g_rGamemode{&RolledBase, P::name::PosterGamemodeProp, L"ObjectProperty", {}};
Member g_pType{&PlacedBase, P::name::PosterTypeProp, L"IntProperty", {}};
Member g_pIndex{&PlacedBase, P::name::PosterIndexProp, L"IntProperty", {}};
Member g_pFile{&PlacedBase, P::name::PosterFileNameProp, L"NameProperty", {}};
Member g_pCustom{&PlacedBase, P::name::PosterCustomProp, L"BoolProperty", {}};
Member g_pClass{&PlacedBase, P::name::PosterClassProp, L"ClassProperty", {}};
Member g_pGamemode{&PlacedBase, P::name::PosterGamemodeProp, L"ObjectProperty", {}};

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
        UE_LOGW("poster: %ls.%ls (%ls) did not resolve by name and type", R::ClassNameOf(cls).c_str(), m.name, m.type);
    }
    return m.ok;
}

uint8_t* At(void* obj, const Member& m) { return reinterpret_cast<uint8_t*>(obj) + m.off; }

template <class T>
bool ReadAt(void* obj, Member& m, T& out) {
    if (!Resolve(m)) return false;
    std::memcpy(&out, At(obj, m), sizeof(T));
    return true;
}

template <class T>
bool WriteAt(void* obj, Member& m, const T& v) {
    if (!Resolve(m)) return false;
    std::memcpy(At(obj, m), &v, sizeof(T));
    return true;
}

bool ReadBool(void* obj, Member& m, bool& out) {
    if (!Resolve(m)) return false;
    out = (*At(obj, m) & m.mask) != 0;
    return true;
}

bool WriteBool(void* obj, Member& m, bool v) {
    if (!Resolve(m)) return false;
    uint8_t& b = *At(obj, m);
    b = v ? static_cast<uint8_t>(b | m.mask) : static_cast<uint8_t>(b & ~m.mask);
    return true;
}

void* ReadLiveObj(void* obj, Member& m) {
    void* p = nullptr;
    if (!ReadAt(obj, m, p)) return nullptr;
    return p && R::IsLive(p) ? p : nullptr;
}

bool Descends(void* cls, void* base) { return cls && base && R::IsDescendantOfAny(cls, &base, 1); }

bool MakeName(const std::wstring& s, R::FName& out) {
    if (s.empty() || s == L"None") {
        out = R::FName{0, 0};
        return true;
    }
    out = fname_utils::StringToFName(s);
    return out.ComparisonIndex != 0;
}

bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }
bool Finite(const FRotator& r) { return std::isfinite(r.Pitch) && std::isfinite(r.Yaw) && std::isfinite(r.Roll); }

// The gamemode's custom poster files (TArray<FName>), its element type measured once per class.
struct FilesArray {
    CachedObjRef cls;
    int32_t      off = -1;
    bool         ok = false;
    bool         said = false;
};
FilesArray g_files;

struct RawArray {
    void*   data;
    int32_t num;
    int32_t max;
};

// Whether `fileName` is among this peer's custom poster files. `known` false when the list could not be read, which
// the caller treats as present: the game's own draw then runs as written.
bool HasCustomFile(void* gamemode, const R::FName& fileName, bool& known) {
    known = false;
    if (!gamemode || !R::IsLive(gamemode)) return false;
    void* cls = R::ClassOf(gamemode);
    if (!cls) return false;
    if (!g_files.cls.Is(cls)) {
        g_files.cls.Set(cls);
        g_files.ok = false;
        int32_t off = -1;
        std::wstring type;
        R::ArrayInnerInfo inner;
        if (R::PropertyOffsetAndClass(cls, P::name::PosterCustomFilesProp, off, type) && off >= 0 &&
            type == L"ArrayProperty" && R::ArrayPropertyInner(cls, P::name::PosterCustomFilesProp, inner) &&
            inner.innerType == L"NameProperty" && inner.elementSize == static_cast<int32_t>(sizeof(R::FName))) {
            g_files.off = off;
            g_files.ok = true;
        } else if (!g_files.said) {
            g_files.said = true;
            UE_LOGW("poster: %ls.%ls is not an array of names here -- a missing custom poster image is not caught "
                    "(the decal goes flat)", R::ClassNameOf(cls).c_str(), P::name::PosterCustomFilesProp);
        }
    }
    if (!g_files.ok) return false;
    RawArray arr{};
    std::memcpy(&arr, reinterpret_cast<uint8_t*>(gamemode) + g_files.off, sizeof(arr));
    if (arr.num < 0 || arr.num > 100000 || (arr.num > 0 && !arr.data)) return false;
    known = true;
    const R::FName* names = static_cast<const R::FName*>(arr.data);
    for (int32_t i = 0; i < arr.num; ++i)
        if (names[i].ComparisonIndex == fileName.ComparisonIndex && names[i].Number == fileName.Number) return true;
    return false;
}

bool CallNoArgs(void* obj, const wchar_t* fnName) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(obj, f);
}

bool ReadComponentScale(void* comp, FVector& out) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), P::name::ComponentScaleFn);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(comp, f)) return false;
    out = f.Get<FVector>(L"ReturnValue");
    return Finite(out);
}

}  // namespace

bool operator==(const Look& a, const Look& b) {
    return a.type == b.type && a.index == b.index && a.custom == b.custom && a.fileName == b.fileName;
}

bool IsRolledClass(void* cls) { return Descends(cls, RolledBase()); }
bool IsPlacedClass(void* cls) { return Descends(cls, PlacedBase()); }
bool IsRolled(void* obj) { return obj && R::IsLive(obj) && IsRolledClass(R::ClassOf(obj)); }
bool IsPlaced(void* obj) { return obj && R::IsLive(obj) && IsPlacedClass(R::ClassOf(obj)); }

bool ReadLook(void* poster, Look& out) {
    Look l;
    R::FName file{0, 0};
    if (IsRolled(poster)) {
        if (!ReadAt(poster, g_rType, l.type) || !ReadAt(poster, g_rIndex, l.index) || !ReadAt(poster, g_rFile, file) ||
            !ReadBool(poster, g_rCustom, l.custom))
            return false;
    } else if (IsPlaced(poster)) {
        if (!ReadAt(poster, g_pType, l.type) || !ReadAt(poster, g_pIndex, l.index) || !ReadAt(poster, g_pFile, file) ||
            !ReadBool(poster, g_pCustom, l.custom))
            return false;
    } else {
        return false;
    }
    l.fileName = file.ComparisonIndex == 0 && file.Number == 0 ? std::wstring(L"None") : R::ToString(file);
    out = std::move(l);
    return true;
}

bool WriteFileName(void* poster, const std::wstring& fileName) {
    R::FName n{0, 0};
    if (!MakeName(fileName, n)) return false;
    if (IsRolled(poster)) return WriteAt(poster, g_rFile, n);
    if (IsPlaced(poster)) return WriteAt(poster, g_pFile, n);
    return false;
}

bool WriteLook(void* placed, const Look& look) {
    if (!IsPlaced(placed)) return false;
    R::FName n{0, 0};
    if (!MakeName(look.fileName, n)) return false;
    return WriteAt(placed, g_pType, look.type) && WriteAt(placed, g_pIndex, look.index) &&
           WriteAt(placed, g_pFile, n) && WriteBool(placed, g_pCustom, look.custom);
}

bool Redraw(void* poster, bool* fellBack) {
    if (fellBack) *fellBack = false;
    const bool rolled = IsRolled(poster);
    if (!rolled && !IsPlaced(poster)) return false;
    Member& custom = rolled ? g_rCustom : g_pCustom;
    Member& file = rolled ? g_rFile : g_pFile;
    Member& gm = rolled ? g_rGamemode : g_pGamemode;
    bool isCustom = false;
    R::FName name{0, 0};
    bool clear = false;
    if (ReadBool(poster, custom, isCustom) && isCustom && ReadAt(poster, file, name)) {
        bool known = false;
        const bool present = HasCustomFile(ReadLiveObj(poster, gm), name, known);
        clear = known && !present;
    }
    if (clear) WriteBool(poster, custom, false);
    const bool ran = CallNoArgs(poster, rolled ? P::name::PosterRolledRedrawFn : P::name::PosterPlacedRedrawFn);
    if (clear) WriteBool(poster, custom, true);
    if (fellBack) *fellBack = clear;
    return ran;
}

void* PosterClassOf(void* poster) {
    void* cls = nullptr;
    if (IsRolled(poster)) ReadAt(poster, g_rClass, cls);
    else if (IsPlaced(poster)) ReadAt(poster, g_pClass, cls);
    return cls && R::IsLive(cls) ? cls : nullptr;
}

void* HoldingPlayer(void* rolled) { return IsRolled(rolled) ? ReadLiveObj(rolled, g_rHolder) : nullptr; }

bool ReadPlacement(void* rolled, Placement& out) {
    void* turn = IsRolled(rolled) ? ReadLiveObj(rolled, g_rTurn) : nullptr;
    if (!turn) return false;
    Placement p;
    if (!engine::TryGetComponentLocation(turn, p.location) || !engine::TryGetComponentWorldRotation(turn, p.rotation) ||
        !ReadComponentScale(turn, p.scale))
        return false;
    if (!Finite(p.location) || !Finite(p.rotation)) return false;
    out = p;
    return true;
}

bool ReadTransform(void* placed, Placement& out) {
    if (!IsPlaced(placed)) return false;
    Placement p;
    if (!engine::TryGetActorLocation(placed, p.location) || !engine::TryGetActorRotation(placed, p.rotation))
        return false;
    p.scale = engine::GetActorScale3D(placed);
    if (!Finite(p.location) || !Finite(p.rotation) || !Finite(p.scale)) return false;
    out = p;
    return true;
}

std::wstring KeyOf(void* placed) {
    if (!IsPlaced(placed)) return {};
    R::FName k{0, 0};
    std::memcpy(&k, reinterpret_cast<uint8_t*>(placed) + P::off::Aactor_save_Key, sizeof(k));
    if (k.ComparisonIndex == 0 && k.Number == 0) return {};
    std::wstring s = R::ToString(k);
    return s == L"None" ? std::wstring{} : s;
}

bool SetKey(void* placed, const std::wstring& key) {
    if (!IsPlaced(placed)) return false;
    R::FName k{0, 0};
    if (key.empty() || !MakeName(key, k)) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(placed) + P::off::Aactor_save_Key, &k, sizeof(k));
    return true;
}

void* SpawnPlaced(void* cls, const Placement& at, const Look& look, bool writeIndex, const std::wstring& key) {
    if (!IsPlacedClass(cls) || !Finite(at.location) || !Finite(at.rotation) || !Finite(at.scale)) return nullptr;
    R::FName file{0, 0};
    if (!MakeName(look.fileName, file)) return nullptr;
    void* a = engine::BeginDeferredSpawn(cls, at.location, at.rotation);
    if (!a) return nullptr;
    WriteAt(a, g_pType, look.type);
    if (writeIndex) WriteAt(a, g_pIndex, look.index);
    WriteAt(a, g_pFile, file);
    WriteBool(a, g_pCustom, look.custom);
    if (!key.empty()) SetKey(a, key);
    if (!engine::FinishDeferredSpawn(a, at.location, at.rotation) || !R::IsLive(a)) return nullptr;
    engine::SetActorScale3D(a, at.scale);
    Redraw(a);
    return a;
}

void* SpawnRolled(void* cls, const FVector& location, const FRotator& rotation, const Look& look) {
    if (!IsRolledClass(cls) || !Finite(location) || !Finite(rotation)) return nullptr;
    R::FName file{0, 0};
    if (!MakeName(look.fileName, file)) return nullptr;
    void* a = engine::BeginDeferredSpawn(cls, location, rotation);
    if (!a) return nullptr;
    WriteAt(a, g_rType, look.type);
    WriteAt(a, g_rFile, file);
    WriteAt(a, g_rIndex, look.index);   // the rolled form's own `index`, which its right-button picker reads
    if (!engine::FinishDeferredSpawn(a, location, rotation) || !R::IsLive(a)) return nullptr;
    return a;
}

std::vector<void*> AllPlaced() {
    std::vector<void*> classes;
    object_index::ForEachClass(
        [](void* ctx, void* cls, void*) {
            if (IsPlacedClass(cls)) static_cast<std::vector<void*>*>(ctx)->push_back(cls);
        },
        &classes);
    std::vector<void*> found;
    for (void* cls : classes)
        object_index::ForEachInstance(
            cls, [](void* ctx, void* obj, int32_t) { static_cast<std::vector<void*>*>(ctx)->push_back(obj); }, &found);
    // Placed posters of the running world only: never a class default object, never an actor of a world going away.
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

}  // namespace ue_wrap::poster
