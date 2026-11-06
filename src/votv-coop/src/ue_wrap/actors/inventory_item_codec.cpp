// ue_wrap/actors/inventory_item_codec.cpp -- see ue_wrap/actors/inventory_item_codec.h.

#include "ue_wrap/actors/inventory_item_codec.h"

#include "ue_wrap/actors/save_record.h"     // AllocZeroed, WriteArrHeader, WriteFNameField
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/raw_read.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <chrono>
#include <cstring>
#include <cwchar>

namespace ue_wrap::inventory_item_codec {
namespace {

namespace R = ue_wrap::reflection;
namespace N = ue_wrap::profile::name;
namespace O = ue_wrap::profile::off;
namespace SR = ue_wrap::save_record;

enum class Kind : uint8_t { Pod, Bool, Name, Str, Soft, Struct, Array };

struct Node {
    std::wstring name;
    int32_t off = 0;
    int32_t size = 0;
    Kind kind = Kind::Pod;
    uint8_t mask = 0;          // Bool: the bit
    std::vector<Node> fields;  // Struct: its members; Array: one element node at offset 0
};

constexpr int kMaxDepth = 6;
// An array's element count bound: a byte array (an item's own serialized state) may be long, any other short.
constexpr uint32_t kMaxPodElements = 64 * 1024;
constexpr uint32_t kMaxOtherElements = 1024;

struct Plan {
    bool ready = false;
    int32_t stride = 0;
    uint32_t signature = 0;
    Node row;                       // the struct_inventoryItem, kind Struct
    int32_t posOff = -1, rotOff = -1, amountOff = -1;     // top-level members by role
    int32_t nameOff = -1, classOff = -1;                  // inside data, absolute within the row
    size_t posField = SIZE_MAX, rotField = SIZE_MAX, amountField = SIZE_MAX;
};

Plan g_plan;
bool g_refusedSaid = false;
std::chrono::steady_clock::time_point g_nextTry{};

bool IsPodType(const std::wstring& t) {
    static const wchar_t* kPods[] = {L"ByteProperty",  L"Int8Property",   L"Int16Property",  L"UInt16Property",
                                     L"IntProperty",   L"UInt32Property", L"Int64Property",  L"UInt64Property",
                                     L"FloatProperty", L"DoubleProperty", L"EnumProperty"};
    for (const wchar_t* p : kPods)
        if (t == p) return true;
    return false;
}

void Refuse(const wchar_t* field, const std::wstring& type) {
    if (g_refusedSaid) return;
    g_refusedSaid = true;
    UE_LOGE("inventory_item_codec: struct_inventoryItem member '%ls' is a %ls, which the row codec does not carry -- "
            "world storages are not synced on this build", field, type.c_str());
}

bool BuildStruct(void* st, Node& out, int depth);

// One element of an array property, by its inner type.
bool BuildElement(const R::ArrayInnerInfo& info, Node& out, int depth) {
    out.off = 0;
    out.size = info.elementSize;
    if (IsPodType(info.innerType)) { out.kind = Kind::Pod; return true; }
    if (info.innerType == L"NameProperty" && info.elementSize == 8) { out.kind = Kind::Name; return true; }
    if (info.innerType == L"StrProperty" && info.elementSize == 16) { out.kind = Kind::Str; return true; }
    if (info.innerType == L"StructProperty" && info.innerStruct) {
        out.kind = Kind::Struct;
        return BuildStruct(info.innerStruct, out, depth + 1);
    }
    Refuse(L"(array element)", info.innerType);
    return false;
}

bool BuildStruct(void* st, Node& out, int depth) {
    if (!st || depth > kMaxDepth) return false;
    out.kind = Kind::Struct;
    for (const R::StructFieldInfo& f : R::EnumerateStructFields(st)) {
        Node n;
        n.name = f.name;
        n.off = f.offset;
        n.size = f.size;
        std::wstring type;
        int32_t off = -1;
        if (!R::PropertyOffsetAndClass(st, f.name.c_str(), off, type) || off != f.offset) return false;
        if (IsPodType(type)) {
            n.kind = Kind::Pod;
        } else if (type == L"BoolProperty") {
            int32_t byteOff = -1;
            if (!R::FindBoolProperty(st, f.name.c_str(), byteOff, n.mask) || n.mask == 0) return false;
            n.kind = Kind::Bool;
            n.off = byteOff;
        } else if (type == L"NameProperty" && f.size == 8) {
            n.kind = Kind::Name;
        } else if (type == L"StrProperty" && f.size == 16) {
            n.kind = Kind::Str;
        } else if ((type == L"SoftClassProperty" || type == L"SoftObjectProperty") &&
                   f.size == static_cast<int32_t>(O::FSoftObjectPtr_Size)) {
            n.kind = Kind::Soft;
        } else if (type == L"StructProperty") {
            if (!BuildStruct(R::PropertyInnerStruct(st, f.name.c_str()), n, depth + 1)) return false;
        } else if (type == L"ArrayProperty" && f.size == 16) {
            R::ArrayInnerInfo info;
            if (!R::ArrayPropertyInner(st, f.name.c_str(), info)) return false;
            n.kind = Kind::Array;
            n.fields.emplace_back();
            if (!BuildElement(info, n.fields.back(), depth)) return false;
        } else {
            Refuse(f.name.c_str(), type);
            return false;
        }
        out.fields.push_back(std::move(n));
    }
    return !out.fields.empty();
}

uint32_t Mix(uint32_t h, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}

uint32_t SignatureOf(const Node& n, uint32_t h) {
    h = Mix(h, n.name.data(), n.name.size() * sizeof(wchar_t));
    h = Mix(h, &n.kind, 1);
    h = Mix(h, &n.size, sizeof(n.size));
    for (const Node& c : n.fields) h = SignatureOf(c, h);
    return h;
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = std::wcslen(prefix);
    return s.size() >= n && ::_wcsnicmp(s.c_str(), prefix, n) == 0;
}

bool Measure() {
    if (g_plan.ready) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextTry) return false;
    g_nextTry = now + std::chrono::seconds(1);
    void* const cls = ue_wrap::object_index::ClassByName(N::InventoryComponentClass);
    if (!cls) return false;
    R::ArrayInnerInfo info;
    if (!R::ArrayPropertyInner(cls, N::InventoryCarriedArray, info) || !info.innerStruct || info.elementSize <= 0)
        return false;
    Plan p;
    p.stride = info.elementSize;
    p.row.size = info.elementSize;
    if (!BuildStruct(info.innerStruct, p.row, 0)) {
        g_nextTry = now + std::chrono::hours(24);   // a type it does not carry will not appear later in this build
        return false;
    }
    for (size_t i = 0; i < p.row.fields.size(); ++i) {
        const Node& f = p.row.fields[i];
        if (StartsWithI(f.name, N::ItemPositionPrefix) && f.kind == Kind::Struct && f.size == 8) { p.posOff = f.off; p.posField = i; }
        if (StartsWithI(f.name, N::ItemRotationPrefix) && f.kind == Kind::Pod && f.size == 4) { p.rotOff = f.off; p.rotField = i; }
        if (StartsWithI(f.name, N::ItemAmountPrefix) && f.kind == Kind::Pod && f.size == 1) { p.amountOff = f.off; p.amountField = i; }
        if (StartsWithI(f.name, N::ItemDataPrefix) && f.kind == Kind::Struct)
            for (const Node& d : f.fields) {
                if (StartsWithI(d.name, N::BetterSaveNamePrefix) && d.kind == Kind::Name) p.nameOff = f.off + d.off;
                if (StartsWithI(d.name, N::BetterSaveClassPrefix) && d.kind == Kind::Soft) p.classOff = f.off + d.off;
            }
    }
    if (p.posOff < 0 || p.rotOff < 0 || p.amountOff < 0 || p.nameOff < 0 || p.classOff < 0) {
        UE_LOGE("inventory_item_codec: struct_inventoryItem lacks a member by role (position %d rotation %d amount %d "
                "data.name %d data.class %d) -- world storages are not synced on this build",
                p.posOff, p.rotOff, p.amountOff, p.nameOff, p.classOff);
        g_nextTry = now + std::chrono::hours(24);
        return false;
    }
    p.signature = SignatureOf(p.row, 2166136261u);
    p.ready = true;
    g_plan = std::move(p);
    UE_LOGI("inventory_item_codec: struct_inventoryItem measured (%d B per row, %zu members, plan %08x)",
            g_plan.stride, g_plan.row.fields.size(), g_plan.signature);
    return true;
}

// ---- encode ----

void PutU8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void PutU16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void PutU32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }
void PutRaw(std::vector<uint8_t>& o, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    o.insert(o.end(), b, b + n);
}
bool PutString(std::vector<uint8_t>& o, const std::wstring& s) {
    if (s.size() > kMaxStringChars) return false;
    PutU16(o, static_cast<uint16_t>(s.size()));
    for (wchar_t c : s) PutU16(o, static_cast<uint16_t>(c));
    return true;
}

std::wstring NameAt(const uint8_t* p) {
    std::wstring s = ue_wrap::raw_read::ReadFName(p);
    if (s == L"None") s.clear();
    return s;
}

bool EncodeNode(const Node& n, const uint8_t* base, std::vector<uint8_t>& o, int depth);

bool EncodeValue(const Node& n, const uint8_t* p, std::vector<uint8_t>& o, int depth) {
    switch (n.kind) {
        case Kind::Pod: PutRaw(o, p, static_cast<size_t>(n.size)); return true;
        case Kind::Bool: PutU8(o, (*p & n.mask) ? 1 : 0); return true;
        case Kind::Name: return PutString(o, NameAt(p));
        case Kind::Str: return PutString(o, ue_wrap::raw_read::ReadFString(p));
        case Kind::Soft:
            return PutString(o, NameAt(p + O::FSoftObjectPtr_AssetPathName)) &&
                   PutString(o, ue_wrap::raw_read::ReadFString(p + O::FSoftObjectPtr_SubPathString));
        case Kind::Struct:
            for (const Node& f : n.fields)
                if (!EncodeNode(f, p, o, depth + 1)) return false;
            return true;
        case Kind::Array: {
            const SR::Arr a = SR::ReadArr(p, 0);
            const Node& e = n.fields[0];
            const uint32_t cap = e.kind == Kind::Pod ? kMaxPodElements : kMaxOtherElements;
            if (static_cast<uint32_t>(a.num) > cap) return false;
            PutU32(o, static_cast<uint32_t>(a.num));
            if (e.kind == Kind::Pod) {
                PutRaw(o, a.data, static_cast<size_t>(a.num) * static_cast<size_t>(e.size));
                return o.size() <= kMaxItemBytes;
            }
            for (int32_t i = 0; i < a.num; ++i)
                if (!EncodeValue(e, a.data + static_cast<size_t>(i) * static_cast<size_t>(e.size), o, depth + 1))
                    return false;
            return o.size() <= kMaxItemBytes;
        }
    }
    return false;
}

bool EncodeNode(const Node& n, const uint8_t* base, std::vector<uint8_t>& o, int depth) {
    if (depth > kMaxDepth + 2) return false;
    return EncodeValue(n, base + n.off, o, depth);
}

// ---- decode ----

struct Reader {
    const std::vector<uint8_t>& b;
    size_t& o;
    bool Need(size_t n) const { return o <= b.size() && b.size() - o >= n; }
    bool U8(uint8_t& v) { if (!Need(1)) return false; v = b[o++]; return true; }
    bool U16(uint16_t& v) { if (!Need(2)) return false; v = uint16_t(b[o] | (b[o + 1] << 8)); o += 2; return true; }
    bool U32(uint32_t& v) {
        if (!Need(4)) return false;
        v = uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
        o += 4;
        return true;
    }
    bool Raw(void* dst, size_t n) { if (!Need(n)) return false; std::memcpy(dst, b.data() + o, n); o += n; return true; }
    bool String(std::wstring& s) {
        uint16_t n = 0;
        if (!U16(n) || n > kMaxStringChars || !Need(size_t(n) * 2)) return false;
        s.resize(n);
        for (uint16_t i = 0; i < n; ++i) {
            uint16_t c = 0;
            U16(c);
            if (c == 0) return false;   // an embedded terminator would cut the name the engine interns
            s[i] = static_cast<wchar_t>(c);
        }
        return true;
    }
};

bool DecodeValue(const Node& n, uint8_t* p, Reader& r, int depth) {
    if (depth > kMaxDepth + 2) return false;
    switch (n.kind) {
        case Kind::Pod: return r.Raw(p, static_cast<size_t>(n.size));
        case Kind::Bool: {
            uint8_t v = 0;
            if (!r.U8(v) || v > 1) return false;
            if (v) *p |= n.mask; else *p &= static_cast<uint8_t>(~n.mask);
            return true;
        }
        case Kind::Name: {
            std::wstring s;
            if (!r.String(s)) return false;
            SR::WriteFNameField(p, s);
            return true;
        }
        case Kind::Str: {
            std::wstring s;
            return r.String(s) && (s.empty() || ue_wrap::fstring_utils::MintFString(s, p));
        }
        case Kind::Soft: {
            // The weak pointer and its tag stay zero: the engine resolves the path on the first read.
            std::wstring path, sub;
            if (!r.String(path) || !r.String(sub)) return false;
            SR::WriteFNameField(p + O::FSoftObjectPtr_AssetPathName, path);
            return sub.empty() || ue_wrap::fstring_utils::MintFString(sub, p + O::FSoftObjectPtr_SubPathString);
        }
        case Kind::Struct:
            for (const Node& f : n.fields)
                if (!DecodeValue(f, p + f.off, r, depth + 1)) return false;
            return true;
        case Kind::Array: {
            const Node& e = n.fields[0];
            uint32_t count = 0;
            if (!r.U32(count)) return false;
            const uint32_t cap = e.kind == Kind::Pod ? kMaxPodElements : kMaxOtherElements;
            if (count > cap) return false;
            if (count == 0) return true;
            // The bytes left must be able to feed the count before anything is allocated.
            if (e.kind == Kind::Pod && !r.Need(size_t(count) * size_t(e.size))) return false;
            if (e.kind != Kind::Pod && !r.Need(count)) return false;
            void* buf = SR::AllocZeroed(count, static_cast<size_t>(e.size));
            if (!buf) return false;
            SR::WriteArrHeader(p, 0, buf, static_cast<int32_t>(count));
            auto* data = static_cast<uint8_t*>(buf);
            if (e.kind == Kind::Pod) return r.Raw(data, size_t(count) * size_t(e.size));
            for (uint32_t i = 0; i < count; ++i)
                if (!DecodeValue(e, data + size_t(i) * size_t(e.size), r, depth + 1)) return false;
            return true;
        }
    }
    return false;
}

}  // namespace

bool Ready() { return Measure(); }
int32_t RowStride() { return Measure() ? g_plan.stride : 0; }
uint32_t PlanSignature() { return Measure() ? g_plan.signature : 0; }

bool Encode(const uint8_t* row, std::vector<uint8_t>& out, Mode mode, int amountOverride) {
    if (!row || !Measure()) return false;
    const size_t start = out.size();
    for (size_t i = 0; i < g_plan.row.fields.size(); ++i) {
        const Node& f = g_plan.row.fields[i];
        if (mode == Mode::Identity && (i == g_plan.posField || i == g_plan.rotField || i == g_plan.amountField)) continue;
        if (i == g_plan.amountField && amountOverride >= 0) {
            PutU8(out, static_cast<uint8_t>(amountOverride));
            continue;
        }
        if (!EncodeNode(f, row, out, 0) || out.size() - start > kMaxItemBytes) {
            out.resize(start);
            return false;
        }
    }
    return true;
}

uint64_t Fingerprint(const uint8_t* row) {
    std::vector<uint8_t> b;
    if (!Encode(row, b, Mode::Identity)) return 0;
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h ? h : 1;
}

bool Decode(const std::vector<uint8_t>& b, size_t& o, uint8_t* row) {
    if (!row || !Measure()) return false;
    const size_t start = o;
    Reader r{b, o};
    for (const Node& f : g_plan.row.fields)
        if (!DecodeValue(f, row + f.off, r, 0)) return false;
    return o - start <= kMaxItemBytes;
}

bool IsEmpty(const uint8_t* row) {
    if (!row || !Measure()) return true;
    return NameAt(row + g_plan.nameOff).empty() && NameAt(row + g_plan.classOff + O::FSoftObjectPtr_AssetPathName).empty();
}

uint8_t Amount(const uint8_t* row) {
    return (row && Measure()) ? row[g_plan.amountOff] : 0;
}

void SetAmount(uint8_t* row, uint8_t amount) {
    if (row && Measure()) row[g_plan.amountOff] = amount;
}

std::wstring Name(const uint8_t* row) {
    return (row && Measure()) ? NameAt(row + g_plan.nameOff) : std::wstring{};
}

bool Placement(const uint8_t* row, float& x, float& y, float& rotation) {
    if (!row || !Measure()) return false;
    std::memcpy(&x, row + g_plan.posOff, 4);
    std::memcpy(&y, row + g_plan.posOff + 4, 4);
    std::memcpy(&rotation, row + g_plan.rotOff, 4);
    return true;
}

bool SetPlacement(uint8_t* row, float x, float y, float rotation) {
    if (!row || !Measure()) return false;
    std::memcpy(row + g_plan.posOff, &x, 4);
    std::memcpy(row + g_plan.posOff + 4, &y, 4);
    std::memcpy(row + g_plan.rotOff, &rotation, 4);
    return true;
}

}  // namespace ue_wrap::inventory_item_codec
