// ue_wrap/actors/save_record.cpp -- see ue_wrap/actors/save_record.h.

#include "ue_wrap/actors/save_record.h"

#include "ue_wrap/actors/drive_bank.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <chrono>
#include <cstring>
#include <cwchar>
#include <unordered_map>

namespace ue_wrap::save_record {
namespace {

namespace R = ue_wrap::reflection;
namespace PN = ue_wrap::profile::name;

// ---- the record layout, MEASURED ---------------------------------------------------------------
//
// The record struct is a Blueprint UserDefinedStruct, and its shape moves between game builds:
// 0.9.0n's Fstruct_save had a flat `signals` array mid-struct, 0.10.0's struct_save1 has no
// `signals` member, so every group after it moved up and the span shrank. A
// fixed offset table written against one build therefore writes TArray headers past the end of
// the other build's struct -- into whatever the engine laid down next, a parameter frame's return
// slot or the next array element -- which is heap corruption, not a wrong value. So every offset,
// every size and the array stride are read off the running struct through reflection, each
// group's element type is checked down to its inner element size, and a struct that does not read
// as a record makes every read and write refuse (said once) rather than guess.

enum Field : int {
    F_class, F_transform, F_key, F_bools, F_floats, F_ints, F_strings, F_signals, F_classes,
    F_vectors, F_rotators, F_transforms, F_bytes, F_names, F_count
};

// The member's stable prefix (a BP struct member renders "<name>_<n>_<GUID>"), its own size, and
// for a value group the size of one X inside its struct_mX's TArray<X> (kFlat = the flat signals
// array, kScalar = not an array). The trailing underscore keeps "class_" off "classes_" and
// "transform_" off "transforms_".
constexpr int32_t kScalar = 0;
constexpr int32_t kFlat   = -1;
struct FieldSpec { const wchar_t* prefix; int32_t size; int32_t innerElem; bool required; const char* tag; };
constexpr FieldSpec kSpec[F_count] = {
    {L"class_",      8,    kScalar, true,  "class"},
    {L"transform_",  0x30, kScalar, true,  "transform"},
    {L"key_",        8,    kScalar, true,  "key"},
    {L"bools_",      0x10, 1,       false, "bools"},
    {L"floats_",     0x10, 4,       false, "floats"},
    {L"ints_",       0x10, 4,       false, "ints"},
    {L"strings_",    0x10, 16,      false, "strings"},
    {L"signals_",    0x10, kFlat,   false, "signals"},
    {L"classes_",    0x10, 8,       false, "classes"},
    {L"vectors_",    0x10, 12,      false, "vectors"},
    {L"rotators_",   0x10, 12,      false, "rotators"},
    {L"transforms_", 0x10, 48,      false, "transforms"},
    {L"bytes_",      0x10, 1,       false, "bytes"},
    {L"names_",      0x10, 8,       false, "names"},
};

struct Resolved {
    bool ok = false;
    RecordLayout pub{};
    bool exact = false;     // `stride` was handed in by the engine, not guessed
    int32_t off[F_count];   // -1 = the running struct has no such member (or it failed its type check)
};

// UScriptStruct* -> its measured layout (a failed one kept too, so it is measured once). A struct
// object is immortal once loaded; ResetCodecCache drops the table.
std::unordered_map<void*, Resolved> g_layouts;
void*    g_defaultScript = nullptr;
int32_t  g_defaultStride = 0;
uint64_t g_nextDefaultTryMs = 0;

uint64_t NowMsLayout() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = std::wcslen(prefix);
    return s.size() >= n && ::_wcsnicmp(s.c_str(), prefix, n) == 0;
}

// Measure `script` as a record struct. `stride` is the element size the caller's container walks
// it at (an array's inner ElementSize or a parameter's size), or <= 0 when unknown, in which case
// the span rounded up to the FTransform's 16-byte alignment is used.
const Resolved& Measure(void* script, int32_t stride) {
    auto it = g_layouts.find(script);
    if (it != g_layouts.end()) return it->second;
    Resolved r;
    for (int32_t& o : r.off) o = -1;
    const std::wstring sname = script ? R::ToString(R::NameOf(script)) : std::wstring();
    auto refuse = [&](const char* why) -> const Resolved& {
        UE_LOGE("save_record: struct '%ls' does not read as a save record (%s) -- every record "
                "read and write through it is REFUSED (this line prints once per struct)",
                sname.c_str(), why);
        return g_layouts[script] = r;
    };
    if (!script) return refuse("null struct");
    const int32_t span = R::StructSize(script);
    if (span <= 0 || span > 0x1000) return refuse("implausible PropertiesSize");
    const int32_t aligned = (span + 15) & ~15;
    r.exact = stride > 0;
    if (stride <= 0) stride = aligned;
    if (stride < span) return refuse("element stride smaller than the struct's span");

    for (const R::StructFieldInfo& f : R::EnumerateStructFields(script)) {
        for (int k = 0; k < F_count; ++k) {
            const FieldSpec& spec = kSpec[k];
            if (!StartsWithI(f.name, spec.prefix) || r.off[k] >= 0) continue;
            if (f.size != spec.size || f.offset < 0 || f.offset + f.size > span) {
                UE_LOGW("save_record: '%ls'.%ls is %d bytes at +0x%X (expected %d) -- the %s group is "
                        "not carried", sname.c_str(), f.name.c_str(), f.size, f.offset, spec.size, spec.tag);
                break;
            }
            if (spec.innerElem != kScalar) {
                R::ArrayInnerInfo outer;
                if (!R::ArrayPropertyInner(script, f.name.c_str(), outer)) {
                    UE_LOGW("save_record: '%ls'.%ls is not an array -- the %s group is not carried",
                            sname.c_str(), f.name.c_str(), spec.tag);
                    break;
                }
                bool typeOk = false;
                if (spec.innerElem == kFlat) {
                    // The signal row codec is the desk layer's; the array must be ITS row struct at the
                    // engine ElementSize that layer measured.
                    typeOk = signal_dynamic::ArrayHoldsRows(script, f.name.c_str());
                } else if (outer.innerStruct && outer.elementSize == kMxStride) {
                    // struct_mX: a single TArray<X> at +0.
                    const auto mx = R::EnumerateStructFields(outer.innerStruct);
                    R::ArrayInnerInfo inner;
                    typeOk = !mx.empty() && mx[0].offset == 0 &&
                             R::ArrayPropertyInner(outer.innerStruct, mx[0].name.c_str(), inner) &&
                             inner.elementSize == spec.innerElem;
                }
                if (!typeOk) {
                    UE_LOGW("save_record: '%ls'.%ls element type differs from the codec's (stride %d) -- "
                            "the %s group is not carried", sname.c_str(), f.name.c_str(),
                            outer.elementSize, spec.tag);
                    break;
                }
            }
            r.off[k] = f.offset;
            break;
        }
    }
    for (int k = 0; k < F_count; ++k)
        if (kSpec[k].required && r.off[k] < 0) return refuse(kSpec[k].tag);
    r.ok = true;
    r.pub.script = script;
    r.pub.span = span;
    r.pub.stride = stride;
    r.pub.exact = r.exact;
    int carried = 0;
    for (int k = 0; k < F_count; ++k) carried += r.off[k] >= 0;
    UE_LOGI("save_record: measured struct '%ls' span=0x%X stride=0x%X (%s), %d of %d members carried "
            "(signals %s)", sname.c_str(), span, stride, r.exact ? "engine" : "UNCONFIRMED guess",
            carried, static_cast<int>(F_count),
            r.off[F_signals] >= 0 ? "present" : "absent");
    return g_layouts[script] = r;
}

// The record struct the running game files a prop's save as: the `data` parameter of
// Aprop_C::getData, else the struct asset by name. Retried at most once a second while neither is
// loaded.
const Resolved* DefaultResolved() {
    if (g_defaultScript) {
        const Resolved& r = Measure(g_defaultScript, g_defaultStride);
        return r.ok ? &r : nullptr;
    }
    const uint64_t now = NowMsLayout();
    if (now < g_nextDefaultTryMs) return nullptr;
    g_nextDefaultTryMs = now + 1000;
    void* script = nullptr;
    int32_t stride = 0;
    if (void* propCls = R::FindClass(L"prop_C")) {
        if (void* fn = R::FindFunction(propCls, L"getData")) {
            script = R::PropertyInnerStruct(fn, L"data");
            if (script) stride = R::FindParamSize(fn, L"data");
        }
    }
    if (!script) script = R::FindObject(PN::SaveRecordStruct, L"UserDefinedStruct");
    if (!script) script = R::FindObject(PN::SaveRecordStructLegacy, L"UserDefinedStruct");
    if (!script) return nullptr;
    g_defaultScript = script;
    g_defaultStride = stride;
    const Resolved& r = Measure(script, stride);
    return r.ok ? &r : nullptr;
}

// Whether the running record struct has a `signals` member of its own (0.9.0n yes, 0.10.0 no). Where it
// does, the game's getData/loadData move the flat rows themselves and no sidecar is needed.
bool RecordCarriesSignals() {
    const Resolved* r = DefaultResolved();
    return r && r->off[F_signals] >= 0;
}

// The layout of the record a UFunction's `data` parameter holds, with the parameter's own extent:
// the frame bound a codec read or write is checked against is the PARAMETER's size, never the frame's
// PropertiesSize, since a frame has more parameters after `data` (a return bool at least).
const Resolved* ParamResolved(void* fn, int32_t& outOff) {
    outOff = -1;
    if (!fn) return nullptr;
    void* script = R::PropertyInnerStruct(fn, L"data");
    const int32_t size = R::FindParamSize(fn, L"data");
    const int32_t off = R::FindParamOffset(fn, L"data");
    if (!script || size <= 0 || off < 0) return nullptr;
    const Resolved& r = Measure(script, size);
    if (!r.ok || r.pub.span > size || off + size > R::FunctionFrameSize(fn)) return nullptr;
    outOff = off;
    return &r;
}

std::wstring ReadFStringAt(const void* base, int32_t off) {
    return ue_wrap::raw_read::ReadFString(reinterpret_cast<const uint8_t*>(base) + off);
}
std::wstring ReadClassNameAt(const void* base, int32_t off) {
    void* cls = nullptr;
    std::memcpy(&cls, reinterpret_cast<const uint8_t*>(base) + off, sizeof(void*));
    if (!PlausibleObjPtr(cls) || !R::IsLive(cls)) return {};
    return R::ToString(R::NameOf(cls));  // leaf name; re-resolved via FindClass on apply
}
std::array<float, 10> ReadXformAt(const void* base, int32_t off) {
    ue_wrap::FTransform t{};
    std::memcpy(&t, reinterpret_cast<const uint8_t*>(base) + off, sizeof(t));  // 48 B
    return {t.RotX, t.RotY, t.RotZ, t.RotW, t.TX, t.TY, t.TZ, t.SX, t.SY, t.SZ};
}

// Read a TArray<Fstruct_mX> at `off` into a vector<vector<X>>: each Fstruct_mX (0x10) wraps a
// TArray<X> at +0; `perElem(elemPtr, innerVec)` appends one X.
template <class V, class F>
void ReadGroups(const void* save, int32_t off, int32_t elemSize, std::vector<V>& out, F perElem) {
    const Arr outer = ReadArr(save, off);
    out.reserve(static_cast<size_t>(outer.num));
    for (int32_t i = 0; i < outer.num; ++i) {
        const uint8_t* mx = outer.data + static_cast<size_t>(i) * kMxStride;
        const Arr inner = ReadArr(mx, 0);
        V group;
        group.reserve(static_cast<size_t>(inner.num));
        for (int32_t j = 0; j < inner.num; ++j)
            perElem(inner.data + static_cast<size_t>(j) * elemSize, group);
        out.push_back(std::move(group));
    }
}

// Resolve `leaf` through `resolve` (null = FindClass) and write the UClass (or null) at `dst`.
// Returns false ONLY when a non-empty name did not resolve, so WriteSaveRecord can count the misses
// and say them once per record rather than once per leaf: a crafted record's `classes` group is
// thousands of unknown names, and a line each turned the write into its own flood.
bool WriteClassField(void* dst, const std::wstring& leaf, ClassResolver resolve) {
    void* cls = leaf.empty() ? nullptr : (resolve ? resolve(leaf.c_str()) : R::FindClass(leaf.c_str()));
    std::memcpy(dst, &cls, sizeof(void*));
    return cls || leaf.empty();
}

// Build the outer TArray<Fstruct_mX> for a value group (vector<vector<X>>): each Fstruct_mX
// (0x10) wraps a TArray<X> at +0; `writeOne(elemPtr, x)` writes one X into a zeroed slot.
template <class V, class F>
void WriteGroups(void* save, int32_t off, int32_t elemSize, const std::vector<V>& groups, F writeOne) {
    void* outer = AllocZeroed(groups.size(), kMxStride);
    if (!outer) { WriteArrHeader(save, off, nullptr, 0); return; }
    for (size_t i = 0; i < groups.size(); ++i) {
        const V& inner = groups[i];
        uint8_t* mx = reinterpret_cast<uint8_t*>(outer) + i * kMxStride;
        void* ib = AllocZeroed(inner.size(), static_cast<size_t>(elemSize));
        if (!ib) { WriteArrHeader(mx, 0, nullptr, 0); continue; }
        for (size_t j = 0; j < inner.size(); ++j)
            writeOne(reinterpret_cast<uint8_t*>(ib) + j * elemSize, inner[j]);
        WriteArrHeader(mx, 0, ib, static_cast<int32_t>(inner.size()));
    }
    WriteArrHeader(save, off, outer, static_cast<int32_t>(groups.size()));
}

}  // namespace

Arr ReadArr(const void* base, int32_t off) {
    Arr a;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(base) + off;
    std::memcpy(&a.data, p, sizeof(void*));   // TArray::Data
    std::memcpy(&a.num, p + 8, 4);            // TArray::Num
    // Reject a garbage header: negative/huge Num, or a non-empty array whose Data pointer is
    // not a plausible heap pointer (walking it would deref wild memory + fault the GT tick).
    if (a.num < 0 || a.num > kMaxArr || (a.num > 0 && !PlausibleObjPtr(a.data))) {
        a.data = nullptr;
        a.num = 0;
    }
    return a;
}

void WriteArrHeader(void* base, int32_t off, void* data, int32_t num) {
    uint8_t* p = reinterpret_cast<uint8_t*>(base) + off;
    std::memcpy(p, &data, sizeof(void*));
    std::memcpy(p + 8, &num, 4);
    std::memcpy(p + 12, &num, 4);
}

void* AllocZeroed(size_t count, size_t elemSize) {
    if (!count) return nullptr;
    void* buf = R::EngineAlloc(count * elemSize);
    if (buf) std::memset(buf, 0, count * elemSize);
    return buf;
}

std::wstring ReadFNameAt(const void* base, int32_t off) {
    return ue_wrap::raw_read::ReadFName(reinterpret_cast<const uint8_t*>(base) + off);
}

void WriteFNameField(void* dst, const std::wstring& leaf) {
    R::FName n{0, 0};  // NAME_None for empty
    if (!leaf.empty()) n = ue_wrap::fname_utils::StringToFName(leaf);
    std::memcpy(dst, &n, sizeof(n));
}

namespace {

bool ReadWith(const Resolved& L, const void* base, SaveRecord& rec) {
    const int32_t* o = L.off;
    rec.className = ReadClassNameAt(base, o[F_class]);
    rec.xform     = ReadXformAt(base, o[F_transform]);
    rec.key       = ReadFNameAt(base, o[F_key]);
    if (o[F_bools] >= 0)
        ReadGroups(base, o[F_bools],   1, rec.bools,
                   [](const uint8_t* e, std::vector<uint8_t>& g) { g.push_back(*e ? 1 : 0); });
    if (o[F_floats] >= 0)
        ReadGroups(base, o[F_floats],  4, rec.floats,
                   [](const uint8_t* e, std::vector<float>& g) { float v; std::memcpy(&v, e, 4); g.push_back(v); });
    if (o[F_ints] >= 0)
        ReadGroups(base, o[F_ints],    4, rec.ints,
                   [](const uint8_t* e, std::vector<int32_t>& g) { int32_t v; std::memcpy(&v, e, 4); g.push_back(v); });
    if (o[F_strings] >= 0)
        ReadGroups(base, o[F_strings], 16, rec.strings,
                   [](const uint8_t* e, std::vector<std::wstring>& g) { g.push_back(ReadFStringAt(e, 0)); });
    if (o[F_classes] >= 0)
        ReadGroups(base, o[F_classes], 8, rec.classes,
                   [](const uint8_t* e, std::vector<std::wstring>& g) { g.push_back(ReadClassNameAt(e, 0)); });
    if (o[F_vectors] >= 0)
        ReadGroups(base, o[F_vectors], 12, rec.vectors,
                   [](const uint8_t* e, std::vector<std::array<float, 3>>& g) { std::array<float, 3> v; std::memcpy(v.data(), e, 12); g.push_back(v); });
    if (o[F_rotators] >= 0)
        ReadGroups(base, o[F_rotators], 12, rec.rotators,
                   [](const uint8_t* e, std::vector<std::array<float, 3>>& g) { std::array<float, 3> v; std::memcpy(v.data(), e, 12); g.push_back(v); });
    if (o[F_transforms] >= 0)
        ReadGroups(base, o[F_transforms], 48, rec.transforms,
                   [](const uint8_t* e, std::vector<std::array<float, 10>>& g) { g.push_back(ReadXformAt(e, 0)); });
    if (o[F_bytes] >= 0)
        ReadGroups(base, o[F_bytes],   1, rec.bytes,
                   [](const uint8_t* e, std::vector<uint8_t>& g) { g.push_back(*e); });
    if (o[F_names] >= 0)
        ReadGroups(base, o[F_names],   8, rec.names,
                   [](const uint8_t* e, std::vector<std::wstring>& g) { g.push_back(ReadFNameAt(e, 0)); });
    // signals: a flat TArray of the desk layer's signal row, at the stride that layer measured.
    if (o[F_signals] >= 0) {
        const int32_t sigStride = ue_wrap::signal_dynamic::Stride();
        const Arr sig = ReadArr(base, o[F_signals]);
        if (sigStride > 0) {
            rec.signals.reserve(static_cast<size_t>(sig.num));
            for (int32_t i = 0; i < sig.num; ++i) {
                ue_wrap::signal_dynamic::Row row;
                if (ue_wrap::signal_dynamic::ReadStruct(sig.data + static_cast<size_t>(i) * sigStride, row))
                    rec.signals.push_back(std::move(row));
            }
        }
    }
    return true;
}

bool WriteWith(const Resolved& L, uint8_t* base, const SaveRecord& r, ClassResolver resolve);

void SayGroupsDropped(const Resolved& L, const SaveRecord& r) {
    // A group the running struct does not have cannot be written; a record that carries one (a peer on
    // an older layout, or a crafted record) loses it here, said once.
    static bool s_said = false;
    if (s_said) return;
    const bool lost = (L.off[F_signals] < 0 && !r.signals.empty()) ||
                      (L.off[F_bools] < 0 && !r.bools.empty()) || (L.off[F_floats] < 0 && !r.floats.empty()) ||
                      (L.off[F_ints] < 0 && !r.ints.empty()) || (L.off[F_strings] < 0 && !r.strings.empty()) ||
                      (L.off[F_classes] < 0 && !r.classes.empty()) || (L.off[F_vectors] < 0 && !r.vectors.empty()) ||
                      (L.off[F_rotators] < 0 && !r.rotators.empty()) ||
                      (L.off[F_transforms] < 0 && !r.transforms.empty()) ||
                      (L.off[F_bytes] < 0 && !r.bytes.empty()) || (L.off[F_names] < 0 && !r.names.empty());
    if (!lost) return;
    s_said = true;
    UE_LOGW("save_record: a record carries a value group the running struct has no member for -- that "
            "group is dropped on write (this line prints once)");
}

void SayNoLayout(const char* what) {
    static bool s_said = false;
    if (s_said) return;
    s_said = true;
    UE_LOGW("save_record: %s REFUSED -- the record struct's layout is not measured (prop_C and the "
            "record struct not loaded yet, or it failed to read as a record); nothing was touched "
            "(this line prints once)", what);
}

}  // namespace

const RecordLayout* Layout() {
    const Resolved* r = DefaultResolved();
    return r ? &r->pub : nullptr;
}

int32_t RecordStride() {
    const Resolved* r = DefaultResolved();
    return r && r->exact ? r->pub.stride : 0;
}

int32_t RecordSpan() {
    const Resolved* r = DefaultResolved();
    return r ? r->pub.span : 0;
}

bool IsRecordStruct(void* scriptStruct) {
    const Resolved* r = DefaultResolved();
    return r && scriptStruct && r->pub.script == scriptStruct;
}

// The engine's own element size for an array of records is the authority on the stride. A layout
// measured before prop_C loaded only has the span-rounded guess; the first array that proves the
// real size replaces it. A layout whose stride the engine already gave must agree exactly.
bool ConfirmRecordStride(void* script, int32_t engine) {
    auto it = g_layouts.find(script);
    if (it == g_layouts.end() || !it->second.ok) return false;
    Resolved& r = it->second;
    if (r.exact) return r.pub.stride == engine;
    if (engine < r.pub.span || engine > 0x1000) return false;
    UE_LOGI("save_record: record stride confirmed by the engine's array ElementSize: 0x%X (was the "
            "0x%X span-rounded guess)", engine, r.pub.stride);
    r.pub.stride = engine;
    r.exact = true;
    return true;
}

bool ReadSaveRecord(const void* base, SaveRecord& rec) {
    const Resolved* L = DefaultResolved();
    if (!L) { SayNoLayout("a record read"); return false; }
    if (!base) return false;
    return ReadWith(*L, base, rec);
}

bool WriteSaveRecord(uint8_t* base, const SaveRecord& r, ClassResolver resolve) {
    const Resolved* L = DefaultResolved();
    if (!L) { SayNoLayout("a record write"); return false; }
    if (!base) return false;
    return WriteWith(*L, base, r, resolve);
}

namespace {

bool WriteWith(const Resolved& L, uint8_t* base, const SaveRecord& r, ClassResolver resolve) {
    const int32_t* o = L.off;
    SayGroupsDropped(L, r);
    int classMiss = 0;          // non-empty class leaves that did not resolve, said once below
    std::wstring firstMiss;     // the first one's name, so an honest single miss is still debuggable
    auto noteMiss = [&](const std::wstring& leaf) { if (firstMiss.empty()) firstMiss = leaf; ++classMiss; };
    if (!WriteClassField(base + o[F_class], r.className, resolve)) noteMiss(r.className);
    ue_wrap::FTransform t;  // identity defaults; we set all 10 packed floats
    t.RotX = r.xform[0]; t.RotY = r.xform[1]; t.RotZ = r.xform[2]; t.RotW = r.xform[3];
    t.TX = r.xform[4]; t.TY = r.xform[5]; t.TZ = r.xform[6];
    t.SX = r.xform[7]; t.SY = r.xform[8]; t.SZ = r.xform[9];
    std::memcpy(base + o[F_transform], &t, sizeof(t));
    WriteFNameField(base + o[F_key], r.key);
    if (o[F_bools] >= 0) WriteGroups(base, o[F_bools], 1, r.bools,
                [](uint8_t* e, uint8_t v) { *e = v ? 1 : 0; });
    if (o[F_floats] >= 0) WriteGroups(base, o[F_floats], 4, r.floats,
                [](uint8_t* e, float v) { std::memcpy(e, &v, 4); });
    if (o[F_ints] >= 0) WriteGroups(base, o[F_ints], 4, r.ints,
                [](uint8_t* e, int32_t v) { std::memcpy(e, &v, 4); });
    if (o[F_strings] >= 0) WriteGroups(base, o[F_strings], 16, r.strings,
                [](uint8_t* e, const std::wstring& v) { ue_wrap::fstring_utils::MintFString(v, e); });
    if (o[F_classes] >= 0) WriteGroups(base, o[F_classes], 8, r.classes,
                [&](uint8_t* e, const std::wstring& v) { if (!WriteClassField(e, v, resolve)) noteMiss(v); });
    // One line per record, not per leaf. A record with a null class is an item the game cannot
    // materialise, said out loud rather than lost -- but a crafted `classes` group is thousands of
    // unknown names, and the old per-leaf line was its own flood on exactly that input. The first
    // missing name is kept so an honest single miss can still be chased.
    if (classMiss > 0)
        // %.64ls, never %ls: the name is attacker-chosen and the wire admits up to 262144 chars, so
        // an unbounded print would be a 256 KB log line per crafted record.
        UE_LOGE("save_record: %d class name(s) in this record are not loaded (first: '%.64ls') -- "
                "written WITHOUT their class", classMiss, firstMiss.c_str());
    if (o[F_vectors] >= 0) WriteGroups(base, o[F_vectors], 12, r.vectors,
                [](uint8_t* e, const std::array<float, 3>& v) { std::memcpy(e, v.data(), 12); });
    if (o[F_rotators] >= 0) WriteGroups(base, o[F_rotators], 12, r.rotators,
                [](uint8_t* e, const std::array<float, 3>& v) { std::memcpy(e, v.data(), 12); });
    if (o[F_transforms] >= 0) WriteGroups(base, o[F_transforms], 48, r.transforms,
                [](uint8_t* e, const std::array<float, 10>& v) {
                    ue_wrap::FTransform tt;
                    tt.RotX = v[0]; tt.RotY = v[1]; tt.RotZ = v[2]; tt.RotW = v[3];
                    tt.TX = v[4]; tt.TY = v[5]; tt.TZ = v[6];
                    tt.SX = v[7]; tt.SY = v[8]; tt.SZ = v[9];
                    std::memcpy(e, &tt, sizeof(tt));
                });
    if (o[F_bytes] >= 0) WriteGroups(base, o[F_bytes], 1, r.bytes,
                [](uint8_t* e, uint8_t v) { *e = v; });
    if (o[F_names] >= 0) WriteGroups(base, o[F_names], 8, r.names,
                [](uint8_t* e, const std::wstring& v) { WriteFNameField(e, v); });
    // signals: a FLAT TArray<Fstruct_signalDataDynamic> (0x70). Reuse the proven live writer
    // (engine-mints the row FStrings, interns the FNames, zeroes image).
    if (o[F_signals] < 0) return true;
    const int32_t sigStride = ue_wrap::signal_dynamic::Stride();
    void* sb = sigStride > 0 ? AllocZeroed(r.signals.size(), static_cast<size_t>(sigStride)) : nullptr;
    if (!sb) { WriteArrHeader(base, o[F_signals], nullptr, 0); return true; }
    for (size_t i = 0; i < r.signals.size(); ++i)
        // Ignore the bool: the slot is AllocZeroed, so a partial mint (KismetStringLibrary
        // unresolved) leaves a valid empty row (null FStrings / NAME_None / empty image) -- a
        // safe blank fallback, never garbage. We never crash a whole inventory over one signal.
        ue_wrap::signal_dynamic::WriteStructLive(
            reinterpret_cast<uint8_t*>(sb) + i * static_cast<size_t>(sigStride), r.signals[i]);
    WriteArrHeader(base, o[F_signals], sb, static_cast<int32_t>(r.signals.size()));
    return true;
}

}  // namespace



// ---- The GAME's own codec, on a LIVE actor --------------------------------------------------

namespace {

void*    g_propCls      = nullptr;  // Aprop_C -- the root the override test measures against
void*    g_baseGetData  = nullptr;  // Aprop_C::getData, for the base half of a record
uint64_t g_nextPropTryMs = 0;

// Class -> "declares a getData of its own below Aprop_C", and the resolved verbs. A UClass is
// immortal once loaded, so these are keyed by the pointer and only ResetCodecCache drops them.
// The verbs are cached because a lookup reads the class's function list, rendering each name it
// passes: uncached, every capture and every apply paid that, at join-drain rates.
struct ClassVerbs { void* getData = nullptr; void* loadData = nullptr; bool overrides = false;
                    bool conventionChecked = false; };
std::unordered_map<void*, ClassVerbs> g_verbs;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Blueprint classes load on demand, so a first call before prop_C exists must not disable the lane
// for the session: retried at most once a second, the idiom the device wrappers use.
bool EnsurePropClass() {
    if (g_propCls) return true;
    const uint64_t now = NowMs();
    if (now < g_nextPropTryMs) return false;
    g_nextPropTryMs = now + 1000;
    g_propCls = R::FindClass(L"prop_C");
    if (!g_propCls) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("save_record: prop_C not loaded -- no prop carries its own save record yet; "
                    "retrying at 1 Hz (this line prints once)");
        }
        return false;
    }
    g_baseGetData = R::FindFunction(g_propCls, L"getData");
    if (!g_baseGetData)
        UE_LOGW("save_record: prop_C has no getData -- the base half of a record cannot be read, "
                "so no record will be applied");
    return true;
}

// The per-class row, resolved once.
const ClassVerbs* VerbsFor(void* cls) {
    if (!cls || !EnsurePropClass()) return nullptr;
    const auto it = g_verbs.find(cls);
    if (it != g_verbs.end()) return &it->second;
    ClassVerbs v;
    // The lineage FIRST: a class off the Aprop_C chain resolves nothing. FindDispatchFunction
    // reads the function list of every level of the chain, and the key index this lane is driven
    // from also holds trash piles, clumps and chip piles, none descended from Aprop_C.
    if (R::IsDescendantOfAny(cls, &g_propCls, 1)) {
        v.getData  = R::FindDispatchFunction(cls, L"getData", nullptr);
        v.loadData = R::FindDispatchFunction(cls, L"loadData", nullptr);
        // Stop AT Aprop_C: its own getData is the base record, and every field of it already
        // rides the prop spawn row (class, key, name, the saved bools, the transform).
        for (void* c = cls; c && c != g_propCls; c = R::SuperStructOf(c))
            if (R::FindFunction(c, L"getData")) { v.overrides = true; break; }
    }
    return &(g_verbs[cls] = v);
}

// Run `fn` on `actor` and read the Fstruct_save it leaves in the frame's `data` parameter.
// Published as CaptureRecordVia at the bottom of the file: a class off the Aprop_C chain needs
// exactly this dispatch and none of the lineage policy around it.
bool CallForRecord(void* actor, void* fn, SaveRecord& out) {
    if (!fn) return false;
    int32_t off = -1;
    const Resolved* L = ParamResolved(fn, off);
    if (!L) { SayNoLayout("a getData capture"); return false; }
    ParamFrame f(fn);
    if (!f.valid() || f.ParamOffset(L"data") != off || off + L->pub.span > f.FrameSize()) return false;
    if (!Call(actor, f)) return false;
    // The record the call left in the frame owns engine-allocated nested arrays; ReadWith copies
    // every one out, and the frame's own buffers are the deliberate leak this layer's out-param
    // doctrine already carries (fstring_utils.h).
    return ReadWith(*L, static_cast<const uint8_t*>(f.data()) + off, out);
}

// The splice below gives the receiver's own value to every ELEMENT the base class wrote and lets
// the leaf keep everything past that extent. A leaf free to store what it likes out there is still
// not free to store something of its own AT one of the base's elements: the splice would hand it
// the base's value instead -- a lifespan arriving as a timer. So the convention is MEASURED per
// class, once, against the game, and a class that fails it leaves the lane with its state
// travelling no worse than before this codec existed.
//
// The groups are not hand-listed: a group the base never wrote is empty in the base record, so the
// comparison over it has no elements to run. And the comparison covers only the OVERLAP, so a leaf
// group SHORTER than the base's is admitted where a whole-group compare rejected it on length
// alone -- harmless, since the splice grows it back and fills the new indices from the base.
//
// Both dispatches are paid once per class and cached with the verbs.
bool EnsureConventionChecked(void* actor, void* cls) {
    auto it = g_verbs.find(cls);
    if (it == g_verbs.end()) return false;
    if (it->second.conventionChecked) return it->second.overrides;
    it->second.conventionChecked = true;
    if (!it->second.overrides) return false;
    SaveRecord base, leaf;
    if (!CallForRecord(actor, g_baseGetData, base)) return true;    // unresolvable: re-checked never; the apply's own guard catches it
    if (!CallForRecord(actor, it->second.getData, leaf)) return true;
    // Both records come off the SAME actor in the same frame, and the leaf's getData calls the
    // base's first, so every element the base wrote is in the leaf's record unchanged -- unless
    // the leaf overwrote it, which is exactly what this asks.
    auto clash = [](const auto& leafG, const auto& baseG) {
        for (size_t g = 0; g < leafG.size() && g < baseG.size(); ++g)
            for (size_t e = 0; e < leafG[g].size() && e < baseG[g].size(); ++e)
                if (!(leafG[g][e] == baseG[g][e])) return true;
        return false;
    };
    auto clashFlat = [](const auto& leafA, const auto& baseA) {
        for (size_t i = 0; i < leafA.size() && i < baseA.size(); ++i)
            if (!(leafA[i] == baseA[i])) return true;
        return false;
    };
    const wchar_t* which = nullptr;
    if      (clash(leaf.bools,      base.bools))      which = L"bools";
    else if (clash(leaf.floats,     base.floats))     which = L"floats";
    else if (clash(leaf.ints,       base.ints))       which = L"ints";
    else if (clash(leaf.strings,    base.strings))    which = L"strings";
    else if (clash(leaf.classes,    base.classes))    which = L"classes";
    else if (clash(leaf.vectors,    base.vectors))    which = L"vectors";
    else if (clash(leaf.rotators,   base.rotators))   which = L"rotators";
    else if (clash(leaf.transforms, base.transforms)) which = L"transforms";
    else if (clash(leaf.bytes,      base.bytes))      which = L"bytes";
    else if (clash(leaf.names,      base.names))      which = L"names";
    else if (clashFlat(leaf.signals, base.signals))   which = L"signals";
    if (!which) return true;
    it->second.overrides = false;   // and so Covers() is false for this class from here on
    UE_LOGW("save_record: '%ls' stores its own value in `%ls` at an element Aprop_C writes too -- "
            "the base splice would hand it the base's value instead, so this class LEAVES the "
            "save-record lane. Its state needs a carrier of its own.",
            R::ToString(R::NameOf(cls)).c_str(), which);
    return false;
}

// The base's elements over the base's OWN extent; past it, whatever `dst` already held stands.
template <class T>
void SpliceElements(std::vector<T>& dst, const std::vector<T>& src) {
    // A base array longer than the leaf's grows the leaf's rather than truncating to it: every
    // element the base wrote has to land, and out there the leaf has nothing of its own to lose.
    if (dst.size() < src.size()) dst.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) dst[i] = src[i];
}

// The same rule one level up, per group: the receiver's own value for every element the BASE class
// wrote, the sender's for the rest. A whole-group replace instead of this dropped the leaf's TAIL
// -- Aprop_food_C turns the base's one float into four and keeps three of its own out there -- and
// every food class had to leave the lane to avoid losing it.
template <class V>
void SpliceGroups(std::vector<V>& dst, const std::vector<V>& src) {
    if (dst.size() < src.size()) dst.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) SpliceElements(dst[i], src[i]);
}

}  // namespace

bool OverridesGetData(void* cls) {
    const ClassVerbs* v = VerbsFor(cls);
    return v && v->overrides;
}

bool CaptureRecord(void* actor, SaveRecord& out) {
    if (!actor) return false;
    void* cls = R::ClassOf(actor);
    const ClassVerbs* v = VerbsFor(cls);
    if (!v || !v->getData) return false;
    // Measured before the first record of this class ever leaves: a class that fails the splice
    // convention must not be published, or every receiver would drop what it sent.
    if (!EnsureConventionChecked(actor, cls)) return false;
    if (!CallForRecord(actor, v->getData, out)) return false;
    // A drive box's rows ride the record's flat sidecar when the running struct has no member of its own
    // for them (0.10.0): its getData files the occupancy and leaves the rows behind.
    if (!RecordCarriesSignals() && out.signals.empty()) ue_wrap::drive_bank::Capture(actor, out);
    return true;
}

bool CaptureOwnRecord(void* actor, SaveRecord& out) {
    if (!actor) return false;
    const ClassVerbs* v = VerbsFor(R::ClassOf(actor));
    return v && v->getData && CallForRecord(actor, v->getData, out);
}

bool IsPropClass(void* cls) {
    return cls && EnsurePropClass() && R::IsDescendantOfAny(cls, &g_propCls, 1);
}

bool ApplyRecord(void* actor, const SaveRecord& r, ClassResolver resolve) {
    if (!actor || !EnsurePropClass() || !g_baseGetData) return false;
    const ClassVerbs* v = VerbsFor(R::ClassOf(actor));
    // `overrides` and not merely "a loadData exists somewhere": this function dispatches
    // Aprop_C::getData on `actor` below, and Aprop_C::getData reads key, name, nametag and four
    // bools at Aprop_C's own property offsets. On an actor off that chain those offsets address
    // unrelated memory, and the caller's actor comes from a KEY the wire chose -- the key index
    // also holds trash piles, clumps and chip piles, none of which descend from Aprop_C. So the
    // lineage is a precondition of the codec, checked here rather than trusted from a call site.
    if (!v || !v->overrides || !v->loadData) return false;
    if (!EnsureConventionChecked(actor, R::ClassOf(actor))) return false;

    // THE BASE HALF IS NEVER THE SENDER'S. Aprop_C::loadData restores the save Key, the transform
    // scale, the four saved bools, the lifespan and the two names from whatever record it is
    // handed -- so a record taken at face value is a primitive for rewriting any prop's identity
    // and for destroying it through SetLifeSpan. Every one of those fields already rides the spawn
    // row, which is the authority for them. So the receiver's OWN base record is read here and
    // spliced over the incoming one: what survives from the sender is exactly the elements the base
    // class does not write, which is the leaf's own save state and the only thing this codec
    // exists to move.
    SaveRecord base;
    if (!CallForRecord(actor, g_baseGetData, base)) return false;
    SaveRecord merged = r;
    merged.className = base.className;
    merged.xform     = base.xform;
    merged.key       = base.key;
    SpliceGroups(merged.bools,      base.bools);
    SpliceGroups(merged.floats,     base.floats);
    SpliceGroups(merged.ints,       base.ints);
    SpliceGroups(merged.strings,    base.strings);
    SpliceGroups(merged.classes,    base.classes);
    SpliceGroups(merged.vectors,    base.vectors);
    SpliceGroups(merged.rotators,   base.rotators);
    SpliceGroups(merged.transforms, base.transforms);
    SpliceGroups(merged.bytes,      base.bytes);
    SpliceGroups(merged.names,      base.names);
    SpliceElements(merged.signals,  base.signals);   // a flat array, so the rule applies one level up

    int32_t off = -1;
    const Resolved* L = ParamResolved(v->loadData, off);
    if (!L) { SayNoLayout("a loadData apply"); return false; }
    ParamFrame f(v->loadData);
    if (!f.valid() || f.ParamOffset(L"data") != off || off + L->pub.span > f.FrameSize()) return false;
    // The frame arrives zeroed, which is WriteWith's stated precondition.
    if (!WriteWith(*L, static_cast<uint8_t*>(f.data()) + off, merged, resolve)) return false;
    if (!Call(actor, f)) return false;
    // The rows after the record's own loadData, which sets drives_in from the record: they go into the
    // slots it flagged, and upd() runs last so the lights and colours read them.
    if (!RecordCarriesSignals() && !r.signals.empty()) ue_wrap::drive_bank::Apply(actor, r);
    return true;
}

bool CaptureRecordVia(void* actor, void* recordFn, SaveRecord& out) {
    if (!actor) return false;
    return CallForRecord(actor, recordFn, out);
}

bool ApplyRecordVia(void* actor, void* loadFn, const SaveRecord& r, ClassResolver resolve) {
    if (!actor || !loadFn) return false;
    int32_t off = -1;
    const Resolved* L = ParamResolved(loadFn, off);
    if (!L) { SayNoLayout("a loadData apply"); return false; }
    ParamFrame f(loadFn);
    if (!f.valid() || f.ParamOffset(L"data") != off || off + L->pub.span > f.FrameSize()) return false;
    // The slot must be zeroed before the write: it constructs engine-owned TArrays in place and
    // reads nothing, so a dirty slot would leave the engine dereferencing whatever was there. The
    // span zeroed is the parameter's own struct, never a constant.
    std::memset(static_cast<uint8_t*>(f.data()) + off, 0, static_cast<size_t>(L->pub.span));
    if (!WriteWith(*L, static_cast<uint8_t*>(f.data()) + off, r, resolve)) return false;
    return Call(actor, f);
}

void ResetCodecCache() {
    // The measured layouts are NOT dropped: a UScriptStruct outlives every level (it is an asset),
    // and a pointer into a freed struct is refused by the IsLive the next measure would need anyway.
    g_propCls = nullptr;
    g_baseGetData = nullptr;
    g_nextPropTryMs = 0;
    g_verbs.clear();
    ue_wrap::drive_bank::Reset();
    ResetGObjStackCache();
}

}  // namespace ue_wrap::save_record
