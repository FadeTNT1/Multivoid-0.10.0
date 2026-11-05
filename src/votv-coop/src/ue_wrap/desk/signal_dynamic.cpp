// ue_wrap/desk/signal_dynamic.cpp -- see header.

#include "ue_wrap/desk/signal_dynamic.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/raw_read.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <chrono>
#include <cstring>
#include <cwchar>
#include <map>
#include <utility>

namespace ue_wrap::signal_dynamic {

namespace R = ue_wrap::reflection;

namespace {

// ---- the measured layout -----------------------------------------------------------------------

struct MemberSpec { const wchar_t* prefix; int32_t size; int32_t Layout::*slot; };
// The trailing underscore keeps "signal_" off "signalLocation_" and "object_" off "objectType_".
constexpr MemberSpec kMembers[] = {
    {L"name_",                16, &Layout::name},
    {L"level_",                4, &Layout::level},
    {L"id_",                  16, &Layout::id},
    {L"size_",                 4, &Layout::size},
    {L"decoded_",              4, &Layout::decoded},
    {L"date_",                 8, &Layout::date},
    {L"isCopy_",               1, &Layout::isCopy},
    {L"polarity_",             4, &Layout::polarity},
    {L"signalLocation_",       8, &Layout::loc},
    {L"object_",               8, &Layout::object},
    {L"signal_",               8, &Layout::signal},
    {L"image_",               16, &Layout::image},
    {L"frequency_",            1, &Layout::freq},
    {L"quality_",              1, &Layout::qual},
    {L"objectType_",           1, &Layout::objType},
    {L"downloadedAtQuality_",  4, &Layout::daq},
};

Layout   g_layout;
void*    g_script = nullptr;   // the UserDefinedStruct the layout was measured on
bool     g_measured = false;   // a struct was found and judged (ok or not)
bool     g_ok = false;
uint64_t g_nextTryMs = 0;

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = std::wcslen(prefix);
    return s.size() >= n && ::_wcsnicmp(s.c_str(), prefix, n) == 0;
}

void Measure() {
    if (g_measured) return;
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + 1000;
    void* script = R::FindObject(ue_wrap::profile::name::SignalDynamicStruct, L"UserDefinedStruct");
    if (!script) return;   // not loaded yet: retried
    g_measured = true;
    Layout L;
    L.span = R::StructSize(script);
    if (L.span <= 0 || L.span > 0x1000) {
        UE_LOGE("signal_dynamic: struct_signalDataDynamic reads an implausible size %d -- every signal "
                "row read and write is REFUSED", L.span);
        return;
    }
    L.stride = (L.span + 7) & ~7;
    for (const R::StructFieldInfo& f : R::EnumerateStructFields(script)) {
        for (const MemberSpec& m : kMembers) {
            if (!StartsWithI(f.name, m.prefix) || L.*(m.slot) >= 0) continue;
            if (f.size == m.size && f.offset >= 0 && f.offset + f.size <= L.span) L.*(m.slot) = f.offset;
            break;
        }
    }
    for (const MemberSpec& m : kMembers) {
        if (L.*(m.slot) < 0) {
            UE_LOGE("signal_dynamic: struct_signalDataDynamic has no '%ls*' member of %d bytes -- every "
                    "signal row read and write is REFUSED (the struct changed shape; re-derive the row)",
                    m.prefix, m.size);
            return;
        }
    }
    g_layout = L;
    g_script = script;
    g_ok = true;
    UE_LOGI("signal_dynamic: measured struct_signalDataDynamic span=0x%X stride=0x%X (name@0x%X "
            "signal@0x%X image@0x%X daq@0x%X)", L.span, L.stride, L.name, L.signal, L.image, L.daq);
}

// The same guards the record codec reads with (raw_read.h): a Data pointer that is not a pointer, a Num
// of INT32_MAX, a name index outside the pool all read as empty instead of faulting the tick.
std::wstring ReadFString(const uint8_t* p) { return ue_wrap::raw_read::ReadFString(p); }
std::wstring ReadFNameLeaf(const uint8_t* p) { return ue_wrap::raw_read::ReadFName(p); }

// A leaf as the FName the struct stores: NAME_None for an empty leaf, and false for a non-empty one
// the engine interns to NAME_None ("None" in any case), which no reader could tell from empty.
bool LeafToFName(const std::wstring& leaf, R::FName& out) {
    out = R::FName{};
    if (leaf.empty()) return true;
    out = ue_wrap::fname_utils::StringToFName(leaf);
    return !(out.ComparisonIndex == 0 && out.Number == 0);
}

// A photo past this is no photo the laptop took (the local saves hold at most 8.7 KB): the array is
// read as garbage and left out.
constexpr int32_t kImageSanity = 4 * 1024 * 1024;

// Fstruct_byteImage: its one member, the photo's TArray<uint8>.
struct ByteArray {
    uint8_t* data;
    int32_t  num;
    int32_t  max;
};

}  // namespace

const Layout* GetLayout() {
    Measure();
    return g_ok ? &g_layout : nullptr;
}

int32_t Stride() {
    const Layout* L = GetLayout();
    return L ? L->stride : 0;
}

namespace {

// (owner, member) -> whether it was proven to be a row array / row member; a verdict is final, so a failure
// is said once and not re-measured.
std::map<std::pair<void*, std::wstring>, bool> g_verdicts;

bool Verdict(void* owner, const wchar_t* member, bool array) {
    const Layout* L = GetLayout();
    if (!L || !owner || !member) return false;      // unmeasured: not a verdict, asked again
    const auto key = std::make_pair(owner, std::wstring(array ? L"[]" : L"") + member);
    const auto it = g_verdicts.find(key);
    if (it != g_verdicts.end()) return it->second;
    bool ok = false;
    const char* why = nullptr;
    if (array) {
        R::ArrayInnerInfo inner;
        if (!R::ArrayPropertyInner(owner, member, inner) || !inner.innerStruct) why = "is not an array of structs";
        else if (inner.innerStruct != g_script) why = "holds a different struct";
        else if (inner.elementSize != L->stride) why = "has an engine ElementSize that is not the measured row stride";
        else ok = true;
        if (why)
            UE_LOGE("signal_dynamic: %ls.%ls %s (engine ElementSize %d, measured stride %d) -- every row read "
                    "and write through it is REFUSED (this line prints once)",
                    R::ToString(R::NameOf(owner)).c_str(), member, why, inner.elementSize, L->stride);
    } else {
        if (R::PropertyInnerStruct(owner, member) == g_script) ok = true;
        else
            UE_LOGE("signal_dynamic: %ls.%ls is not a struct_signalDataDynamic member -- every row read and "
                    "write through it is REFUSED (this line prints once)",
                    R::ToString(R::NameOf(owner)).c_str(), member);
    }
    g_verdicts.emplace(key, ok);
    return ok;
}

}  // namespace

bool ArrayHoldsRows(void* owner, const wchar_t* member) { return Verdict(owner, member, true); }
bool MemberIsRow(void* owner, const wchar_t* member) { return Verdict(owner, member, false); }

bool ReadStruct(const void* base, Row& out, bool withImage) {
    const Layout* L = GetLayout();
    if (!base || !L) return false;
    const Layout& K = *L;
    const uint8_t* p = static_cast<const uint8_t*>(base);
    out.name   = ReadFString(p + K.name);
    out.id     = ReadFString(p + K.id);
    out.object = ReadFNameLeaf(p + K.object);
    out.signal = ReadFNameLeaf(p + K.signal);
    std::memcpy(&out.level, p + K.level, sizeof(out.level));
    std::memcpy(&out.polarity, p + K.polarity, sizeof(out.polarity));
    std::memcpy(&out.size, p + K.size, sizeof(out.size));
    std::memcpy(&out.decoded, p + K.decoded, sizeof(out.decoded));
    std::memcpy(&out.date, p + K.date, sizeof(out.date));
    out.isCopy = *(p + K.isCopy) != 0;
    std::memcpy(&out.locX, p + K.loc + 0, sizeof(float));
    std::memcpy(&out.locY, p + K.loc + 4, sizeof(float));
    out.frequency  = *(p + K.freq);
    out.quality    = *(p + K.qual);
    out.objectType = *(p + K.objType);
    std::memcpy(&out.downloadedAtQuality, p + K.daq, sizeof(float));
    out.hasData = out.size > 0.0f;
    out.image.clear();
    if (withImage) {
        ByteArray img{};
        std::memcpy(&img, p + K.image, sizeof(img));
        // A header that reads as a photo: a plausible buffer, a count inside the capacity and the sanity cap.
        if (ue_wrap::raw_read::PlausiblePtr(img.data) && img.num > 0 && img.num <= kImageSanity && img.max >= img.num)
            out.image.assign(img.data, img.data + img.num);
    }
    return true;
}

bool WriteStructLive(void* base, const Row& in) {
    const Layout* L = GetLayout();
    if (!base || !L) return false;
    const Layout& K = *L;
    uint8_t* p = static_cast<uint8_t*>(base);
    // Every step that can fail runs first, into locals, and the struct is written only once all of
    // them have succeeded. Written in place, a failure stopped half way: a drive kept a new name and
    // id over its old signal, and no caller could tell which row it now held.
    R::FName object{}, signal{};
    if (!LeafToFName(in.object, object) || !LeafToFName(in.signal, signal)) return false;
    uint8_t nameHeader[sizeof(R::FString)] = {};
    uint8_t idHeader[sizeof(R::FString)] = {};
    // A name minted before a failed id is left unreferenced, as every replaced string is (header).
    if (!ue_wrap::fstring_utils::MintFString(in.name, nameHeader)) return false;
    if (!ue_wrap::fstring_utils::MintFString(in.id, idHeader)) return false;
    std::memcpy(p + K.name, nameHeader, sizeof(nameHeader));
    std::memcpy(p + K.id, idHeader, sizeof(idHeader));
    std::memcpy(p + K.object, &object, sizeof(object));
    std::memcpy(p + K.signal, &signal, sizeof(signal));
    std::memcpy(p + K.level, &in.level, sizeof(in.level));
    std::memcpy(p + K.polarity, &in.polarity, sizeof(in.polarity));
    std::memcpy(p + K.size, &in.size, sizeof(in.size));
    std::memcpy(p + K.decoded, &in.decoded, sizeof(in.decoded));
    std::memcpy(p + K.date, &in.date, sizeof(in.date));
    *(p + K.isCopy) = in.isCopy ? 1 : 0;
    std::memcpy(p + K.loc + 0, &in.locX, sizeof(float));
    std::memcpy(p + K.loc + 4, &in.locY, sizeof(float));
    *(p + K.freq) = in.frequency;
    *(p + K.qual) = in.quality;
    *(p + K.objType) = in.objectType;
    std::memcpy(p + K.daq, &in.downloadedAtQuality, sizeof(float));
    // Empty the image (count only; the buffer stays engine-owned) so a stale
    // photo never rides with a different signal's data.
    int32_t zero = 0;
    std::memcpy(p + K.image + 8, &zero, sizeof(zero));  // TArray.Num
    return true;
}

bool BuildParamBytes(const Row& in, std::vector<uint8_t>& outVec) {
    outVec.clear();
    const Layout* L = GetLayout();
    if (!L) return false;
    const Layout& K = *L;
    outVec.assign(static_cast<size_t>(K.span), 0);
    uint8_t* out = outVec.data();
    // FStrings point at the caller-held Row's buffers; the callee (saveSignal's
    // Array_Add) deep-copies with the engine allocator during the call.
    auto setStr = [&](int32_t off, const std::wstring& s) {
        if (s.empty()) return;  // null FString = valid empty
        R::FString v{};
        v.Data = const_cast<wchar_t*>(s.c_str());
        v.Num  = static_cast<int32_t>(s.size() + 1);
        v.Max  = v.Num;
        std::memcpy(out + off, &v, sizeof(v));
    };
    setStr(K.name, in.name);
    setStr(K.id, in.id);
    R::FName object{}, signal{};
    if (!LeafToFName(in.object, object) || !LeafToFName(in.signal, signal)) { outVec.clear(); return false; }
    std::memcpy(out + K.object, &object, sizeof(object));
    std::memcpy(out + K.signal, &signal, sizeof(signal));
    std::memcpy(out + K.level, &in.level, sizeof(in.level));
    std::memcpy(out + K.polarity, &in.polarity, sizeof(in.polarity));
    std::memcpy(out + K.size, &in.size, sizeof(in.size));
    std::memcpy(out + K.decoded, &in.decoded, sizeof(in.decoded));
    std::memcpy(out + K.date, &in.date, sizeof(in.date));
    out[K.isCopy] = in.isCopy ? 1 : 0;
    std::memcpy(out + K.loc + 0, &in.locX, sizeof(float));
    std::memcpy(out + K.loc + 4, &in.locY, sizeof(float));
    out[K.freq] = in.frequency;
    out[K.qual] = in.quality;
    out[K.objType] = in.objectType;
    std::memcpy(out + K.daq, &in.downloadedAtQuality, sizeof(float));
    if (!in.image.empty()) {
        ByteArray img{};
        img.data = const_cast<uint8_t*>(in.image.data());
        img.num = static_cast<int32_t>(in.image.size());
        img.max = img.num;
        std::memcpy(out + K.image, &img, sizeof(img));
    }
    return true;
}

}  // namespace ue_wrap::signal_dynamic
