// ue_wrap/actors/save_rows.cpp -- see ue_wrap/actors/save_rows.h.

#include "ue_wrap/actors/save_rows.h"

#include "ue_wrap/actors/save_record.h"   // ReadArr, PlausibleObjPtr
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>

namespace ue_wrap::save_rows {
namespace {

namespace R = ue_wrap::reflection;
namespace O = ue_wrap::profile::off;

// The measured element of one array, keyed by (save class, array name).
struct ElemLayout {
    bool ok = false;
    int32_t arrOff = -1;    // the TArray header on the save object
    int32_t stride = 0;     // the inner ElementSize
    int32_t cls = -1;
    bool    softClass = false;
    int32_t transform = -1; // FTransform; its translation at +0x10
    int32_t key = -1;       // FName
};

struct Key {
    void* cls;
    std::wstring prop;
    bool operator==(const Key& o) const { return cls == o.cls && prop == o.prop; }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        return std::hash<void*>()(k.cls) ^ (std::hash<std::wstring>()(k.prop) << 1);
    }
};
std::unordered_map<Key, ElemLayout, KeyHash> g_layouts;

const ElemLayout& Measure(void* saveCls, const wchar_t* prop) {
    const Key k{saveCls, prop};
    auto it = g_layouts.find(k);
    if (it != g_layouts.end()) return it->second;
    ElemLayout L;
    auto refuse = [&](const char* why) -> const ElemLayout& {
        UE_LOGW("save_rows: %ls.%ls -- %s; its rows are NOT read (this line prints once per array)",
                R::ToString(R::NameOf(saveCls)).c_str(), prop, why);
        return g_layouts[k] = L;
    };
    L.arrOff = R::FindPropertyOffset(saveCls, prop);
    if (L.arrOff < 0) return g_layouts[k] = L;   // the build has no such array: quiet, the caller asks the other name
    R::ArrayInnerInfo inner;
    if (!R::ArrayPropertyInner(saveCls, prop, inner) || !inner.innerStruct)
        return refuse("not an array of structs");
    L.stride = inner.elementSize;
    const int32_t span = R::StructSize(inner.innerStruct);
    if (span <= 0 || span > L.stride) return refuse("element span does not fit its stride");
    for (const R::StructFieldInfo& f : R::EnumerateStructFields(inner.innerStruct)) {
        if (f.offset < 0 || f.offset + f.size > span) continue;
        auto starts = [&](const wchar_t* p) { return ::_wcsnicmp(f.name.c_str(), p, std::wcslen(p)) == 0; };
        if (L.cls < 0 && starts(L"class_")) {
            if (f.size == 8) { L.cls = f.offset; L.softClass = false; }
            else if (f.size == static_cast<int32_t>(O::FSoftObjectPtr_Size)) { L.cls = f.offset; L.softClass = true; }
        } else if (L.transform < 0 && starts(L"transform_") && f.size == 0x30) {
            L.transform = f.offset;
        } else if (L.key < 0 && starts(L"key_") && f.size == 8) {
            L.key = f.offset;
        }
    }
    if (L.cls < 0) return refuse("no class member of a known shape");
    if (L.transform < 0) return refuse("no 48-byte transform member");
    if (L.key < 0) return refuse("no FName key member");
    L.ok = true;
    UE_LOGI("save_rows: %ls.%ls measured -- stride=0x%X class@0x%X (%s) transform@0x%X key@0x%X",
            R::ToString(R::NameOf(saveCls)).c_str(), prop, L.stride, L.cls, L.softClass ? "soft" : "hard",
            L.transform, L.key);
    return g_layouts[k] = L;
}

}  // namespace

void* ResolveSoftClass(const uint8_t* softPtr) {
    if (!softPtr) return nullptr;
    int32_t idx = 0, serial = 0;
    std::memcpy(&idx, softPtr + O::FSoftObjectPtr_WeakIndex, sizeof(idx));
    std::memcpy(&serial, softPtr + O::FSoftObjectPtr_WeakSerial, sizeof(serial));
    if (serial != 0)
        if (void* obj = R::ResolveWeakObject(idx, serial)) return obj;
    R::FName path{};
    std::memcpy(&path, softPtr + O::FSoftObjectPtr_AssetPathName, sizeof(path));
    if (path.ComparisonIndex == 0 || path.ComparisonIndex > 0x0FFFFFFF) return nullptr;
    const std::wstring s = R::ToString(path);
    const size_t dot = s.rfind(L'.');
    const std::wstring leaf = dot == std::wstring::npos ? s : s.substr(dot + 1);
    return leaf.empty() ? nullptr : ue_wrap::object_index::ClassByName(leaf.c_str());
}

int32_t ForEachRow(void* saveSlot, const wchar_t* propName, RowFn fn, void* ctx) {
    if (!saveSlot || !propName || !fn || !R::IsLive(saveSlot)) return -1;
    const ElemLayout& L = Measure(R::ClassOf(saveSlot), propName);
    if (!L.ok) return -1;
    const save_record::Arr a = save_record::ReadArr(saveSlot, L.arrOff);
    for (int32_t i = 0; i < a.num; ++i) {
        const uint8_t* e = a.data + static_cast<size_t>(i) * static_cast<size_t>(L.stride);
        RowView row;
        if (L.softClass) {
            row.cls = ResolveSoftClass(e + L.cls);
        } else {
            void* c = nullptr;
            std::memcpy(&c, e + L.cls, sizeof(c));
            row.cls = (save_record::PlausibleObjPtr(c) && R::IsLive(c)) ? c : nullptr;
        }
        std::memcpy(&row.location, e + L.transform + 0x10, sizeof(row.location));
        std::memcpy(&row.key, e + L.key, sizeof(row.key));
        fn(ctx, i, row);
    }
    return a.num;
}

}  // namespace ue_wrap::save_rows
