// ue_wrap/engine/world_meta.cpp -- see ue_wrap/engine/world_meta.h.

#include "ue_wrap/engine/world_meta.h"

#include "miniz.h"

#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>

namespace ue_wrap::world_meta {
namespace {

constexpr uint32_t kChunkTag = 0x9E2A83C1u;

// A bounds-checked little-endian cursor over a buffer; every read fails closed.
struct Cursor {
    const uint8_t* p;
    size_t n;
    size_t at = 0;
    bool ok = true;

    bool Bytes(void* out, size_t len) {
        if (!ok || len > n - at) { ok = false; return false; }
        std::memcpy(out, p + at, len);
        at += len;
        return true;
    }
    bool Skip(int64_t len) {
        if (!ok || len < 0 || static_cast<uint64_t>(len) > n - at) { ok = false; return false; }
        at += static_cast<size_t>(len);
        return true;
    }
    template <class T>
    T Read() {
        T v{};
        Bytes(&v, sizeof(v));
        return v;
    }
    // UE FString: int32 length; positive is that many ANSI bytes, negative that many UTF-16 units,
    // both counting the terminator.
    std::wstring FStr() {
        const int32_t len = Read<int32_t>();
        if (!ok || len == 0) return {};
        if (len > 4096 || len < -4096) { ok = false; return {}; }
        std::wstring s;
        if (len > 0) {
            if (static_cast<size_t>(len) > n - at) { ok = false; return {}; }
            s.assign(p + at, p + at + len - 1);
            at += static_cast<size_t>(len);
        } else {
            const size_t units = static_cast<size_t>(-len);
            if (units * 2 > n - at) { ok = false; return {}; }
            for (size_t i = 0; i + 1 < units; ++i) {
                uint16_t c;
                std::memcpy(&c, p + at + i * 2, 2);
                s.push_back(static_cast<wchar_t>(c));
            }
            at += units * 2;
        }
        return s;
    }
};

// An enum value written as its name ("enum_gamemode::NewEnumerator3"): the trailing ordinal.
bool EnumOrdinal(const std::wstring& name, int& out) {
    size_t end = name.size();
    size_t begin = end;
    while (begin > 0 && std::iswdigit(name[begin - 1])) --begin;
    if (begin == end || end - begin > 4) return false;
    out = std::stoi(name.substr(begin));
    return true;
}

}  // namespace

bool IsCompressed(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    uint32_t tag;
    std::memcpy(&tag, data, 4);
    return tag == kChunkTag;
}

bool Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t maxBytes) {
    out.clear();
    Cursor c{data, size};
    while (c.ok && c.at < size) {
        // The group's header: {tag, pad, chunk size} then the summary {compressed, uncompressed}.
        const uint32_t tag = c.Read<uint32_t>();
        c.Read<uint32_t>();
        const int64_t chunkSize = c.Read<int64_t>();
        c.Read<int64_t>();
        const int64_t total = c.Read<int64_t>();
        if (!c.ok || tag != kChunkTag || chunkSize <= 0 || total < 0 ||
            static_cast<uint64_t>(total) > maxBytes - out.size()) {
            out.clear();
            return false;
        }
        const int64_t chunks = (total + chunkSize - 1) / chunkSize;
        if (chunks > 4096) { out.clear(); return false; }
        std::vector<std::pair<int64_t, int64_t>> sizes(static_cast<size_t>(chunks));
        int64_t sum = 0;
        for (auto& s : sizes) {
            s.first = c.Read<int64_t>();
            s.second = c.Read<int64_t>();
            if (s.first < 0 || s.second < 0 || s.second > chunkSize) c.ok = false;
            sum += s.second;
        }
        if (!c.ok || sum != total) { out.clear(); return false; }
        for (const auto& s : sizes) {
            if (static_cast<uint64_t>(s.first) > size - c.at) { out.clear(); return false; }
            const size_t base = out.size();
            out.resize(base + static_cast<size_t>(s.second));
            mz_ulong got = static_cast<mz_ulong>(s.second);
            const int rc = mz_uncompress(out.data() + base, &got, data + c.at,
                                         static_cast<mz_ulong>(s.first));
            if (rc != MZ_OK || got != static_cast<mz_ulong>(s.second)) { out.clear(); return false; }
            c.at += static_cast<size_t>(s.first);
        }
    }
    return c.ok;
}

bool LogicalSize(const std::wstring& path, uint64_t& out) {
    out = 0;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f.is_open()) return false;
    f.seekg(0, std::ios::end);
    const int64_t fileSize = static_cast<int64_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    uint8_t head[32] = {};
    if (fileSize < 4 || !f.read(reinterpret_cast<char*>(head), 4)) return false;
    if (!IsCompressed(head, 4)) {
        out = static_cast<uint64_t>(fileSize);
        return true;
    }
    int64_t at = 0;
    while (at < fileSize) {
        f.seekg(at, std::ios::beg);
        if (!f.read(reinterpret_cast<char*>(head), sizeof(head)) || !IsCompressed(head, 4)) return false;
        int64_t chunkSize, compressed, total;
        std::memcpy(&chunkSize, head + 8, 8);
        std::memcpy(&compressed, head + 16, 8);
        std::memcpy(&total, head + 24, 8);
        if (chunkSize <= 0 || compressed < 0 || total < 0) return false;
        const int64_t chunks = (total + chunkSize - 1) / chunkSize;
        out += static_cast<uint64_t>(total);
        at += 32 + chunks * 16 + compressed;
    }
    return at == fileSize;
}

bool ParseMetaPayload(const uint8_t* data, size_t size, WorldMeta& out) {
    out = WorldMeta{};
    Cursor c{data, size};
    c.Read<int32_t>();  // the payload's own length
    const std::wstring className = c.FStr();
    if (!c.ok || className.empty()) return false;
    out.parsed = true;
    out.isMetadataClass = className.find(L"save_metadata_C") != std::wstring::npos;
    if (!out.isMetadataClass) return true;

    while (c.ok) {
        const std::wstring name = c.FStr();
        if (!c.ok || name.empty() || name == L"None") break;
        const std::wstring type = c.FStr();
        const int32_t sz = c.Read<int32_t>();
        c.Read<int32_t>();  // ArrayIndex
        if (!c.ok || sz < 0) break;
        std::wstring structName, enumName;
        if (type == L"StructProperty") {
            structName = c.FStr();
            c.Skip(16);
        } else if (type == L"BoolProperty") {
            c.Read<uint8_t>();
        } else if (type == L"ByteProperty" || type == L"EnumProperty") {
            enumName = c.FStr();
        } else if (type == L"ArrayProperty" || type == L"SetProperty") {
            c.FStr();
        } else if (type == L"MapProperty") {
            c.FStr();
            c.FStr();
        }
        if (c.Read<uint8_t>()) c.Skip(16);
        if (!c.ok) break;
        const size_t valueAt = c.at;

        const bool str = type == L"StrProperty" || type == L"NameProperty";
        if (name == L"Version" && str) {
            out.version = c.FStr();
            out.hasVersion = c.ok;
        } else if (name == L"lastSaved" && structName == L"DateTime" && sz == 8) {
            out.lastSavedTicks = c.Read<int64_t>();
            out.hasLastSaved = c.ok;
        } else if (name == L"savedtime" && structName == L"IntVector" && sz == 12) {
            c.Read<int32_t>();
            c.Read<int32_t>();
            out.savedTimeZ = c.Read<int32_t>();
            out.hasSavedTimeZ = c.ok;
        } else if (name == L"gamemode_type" && (type == L"ByteProperty" || type == L"EnumProperty")) {
            if (type == L"ByteProperty" && (enumName.empty() || enumName == L"None") && sz == 1) {
                out.gameMode = c.Read<uint8_t>();
                out.hasGameMode = c.ok;
            } else {
                out.hasGameMode = EnumOrdinal(c.FStr(), out.gameMode) && c.ok;
            }
        } else if (str && name == L"saveSlot") {
            out.saveSlot = c.FStr();
        } else if (str && name == L"metadataName") {
            out.metadataName = c.FStr();
        } else if (str && name == L"area") {
            out.area = c.FStr();
        } else if (str && name == L"CurrentLevel") {
            out.currentLevel = c.FStr();
        } else if (str && name == L"starterMap") {
            out.starterMap = c.FStr();
        }
        // Every value is left by its tag's size, whatever was read of it.
        if (!c.ok) break;
        c.at = valueAt;
        if (!c.Skip(sz)) break;
    }
    return true;
}

bool ReadWorldMeta(const std::wstring& metaPath, WorldMeta& out) {
    out = WorldMeta{};
    std::ifstream f(std::filesystem::path(metaPath), std::ios::binary);
    if (!f.is_open()) return false;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() > (size_t{1} << 20)) return false;  // a metadata file is a few hundred bytes
    std::vector<uint8_t> payload;
    if (!IsCompressed(file.data(), file.size()) ||
        !Decompress(file.data(), file.size(), payload, size_t{4} << 20))
        return false;
    return ParseMetaPayload(payload.data(), payload.size(), out);
}

}  // namespace ue_wrap::world_meta
