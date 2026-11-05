// The 0.10.0 world-metadata reader (src/ue_wrap/engine/world_meta.cpp) against files built here in
// the game's format: UE chunked compression over a save_metadata_C tagged stream.

#include "ue_wrap/engine/world_meta.h"

#include "miniz.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace WM = ue_wrap::world_meta;

int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

struct Writer {
    std::vector<uint8_t> b;
    template <class T>
    void Put(T v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
    }
    void Str(const std::string& s) {
        Put<int32_t>(static_cast<int32_t>(s.size() + 1));
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }
    void Bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};

// One tag head: name, type, size, array index, then the type's extras and the guid flag.
void Tag(Writer& w, const std::string& name, const std::string& type, int32_t size) {
    w.Str(name);
    w.Str(type);
    w.Put<int32_t>(size);
    w.Put<int32_t>(0);
}

std::vector<uint8_t> StrValue(const std::string& s) {
    Writer v;
    v.Str(s);
    return v.b;
}

void StrProp(Writer& w, const std::string& name, const std::string& type, const std::string& value) {
    const std::vector<uint8_t> v = StrValue(value);
    Tag(w, name, type, static_cast<int32_t>(v.size()));
    w.Put<uint8_t>(0);
    w.Bytes(v);
}

std::vector<uint8_t> MetaPayload(bool withMode) {
    Writer w;
    w.Str("/Game/main/save_metadata.save_metadata_C");
    StrProp(w, "Version", "StrProperty", "0.10.0");
    Tag(w, "lastSaved", "StructProperty", 8);
    w.Str("DateTime");
    w.Bytes(std::vector<uint8_t>(16, 0));
    w.Put<uint8_t>(0);
    w.Put<int64_t>(639294174722880000LL);
    // A rules struct whose inner stream must be skipped by its size.
    {
        Writer inner;
        Tag(inner, "funnySetting_29_3DBB", "BoolProperty", 0);
        inner.Put<uint8_t>(1);
        inner.Put<uint8_t>(0);
        inner.Str("None");
        Tag(w, "Rules", "StructProperty", static_cast<int32_t>(inner.b.size()));
        w.Str("struct_gameRules");
        w.Bytes(std::vector<uint8_t>(16, 7));
        w.Put<uint8_t>(0);
        w.Bytes(inner.b);
    }
    if (withMode) {
        const std::vector<uint8_t> v = StrValue("enum_gamemode::NewEnumerator4");
        Tag(w, "gamemode_type", "ByteProperty", static_cast<int32_t>(v.size()));
        w.Str("enum_gamemode");
        w.Put<uint8_t>(0);
        w.Bytes(v);
    }
    Tag(w, "savedtime", "StructProperty", 12);
    w.Str("IntVector");
    w.Bytes(std::vector<uint8_t>(16, 0));
    w.Put<uint8_t>(0);
    w.Put<int32_t>(5);
    w.Put<int32_t>(30);
    w.Put<int32_t>(6);
    StrProp(w, "saveSlot", "StrProperty", "My World");
    StrProp(w, "metadataName", "StrProperty", "main");
    StrProp(w, "CurrentLevel", "NameProperty", "Untitled_1");
    w.Str("None");
    w.Put<int32_t>(0);
    Writer framed;
    framed.Put<int32_t>(static_cast<int32_t>(w.b.size()));
    framed.Bytes(w.b);
    return framed.b;
}

// The plugin's file: one chunk group per `chunk` bytes, each {tag, pad, chunk size, summary,
// the one chunk's sizes, the zlib bytes}.
std::vector<uint8_t> Compress(const std::vector<uint8_t>& raw, size_t chunk) {
    Writer w;
    for (size_t at = 0; at < raw.size(); at += chunk) {
        const size_t n = std::min(chunk, raw.size() - at);
        mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(n));
        std::vector<uint8_t> z(cap);
        mz_compress(z.data(), &cap, raw.data() + at, static_cast<mz_ulong>(n));
        z.resize(cap);
        w.Put<uint32_t>(0x9E2A83C1u);
        w.Put<uint32_t>(0);
        w.Put<int64_t>(static_cast<int64_t>(chunk));
        w.Put<int64_t>(static_cast<int64_t>(z.size()));
        w.Put<int64_t>(static_cast<int64_t>(n));
        w.Put<int64_t>(static_cast<int64_t>(z.size()));
        w.Put<int64_t>(static_cast<int64_t>(n));
        w.Bytes(z);
    }
    return w.b;
}

std::wstring WriteFile(const std::filesystem::path& dir, const char* name, const std::vector<uint8_t>& bytes) {
    const std::filesystem::path p = dir / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                             static_cast<std::streamsize>(bytes.size()));
    return p.wstring();
}

}  // namespace

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "world_meta_test";
    std::filesystem::create_directories(dir);

    const std::vector<uint8_t> payload = MetaPayload(true);
    const std::vector<uint8_t> file = Compress(payload, 64);  // several groups

    std::vector<uint8_t> back;
    Check(WM::IsCompressed(file.data(), file.size()), "the chunk tag is recognised");
    Check(WM::Decompress(file.data(), file.size(), back) && back == payload, "every chunk group inflates");

    WM::WorldMeta m;
    Check(WM::ReadWorldMeta(WriteFile(dir, "main.meta", file), m), "a metadata file reads");
    Check(m.isMetadataClass, "the class is save_metadata_C");
    Check(m.hasVersion && m.version == L"0.10.0", "the version");
    Check(m.hasLastSaved && m.lastSavedTicks == 639294174722880000LL, "the last-saved date");
    Check(m.hasGameMode && m.gameMode == 4, "the mode from its enumerator name");
    Check(m.hasSavedTimeZ && m.savedTimeZ == 6, "the day past a skipped rules struct");
    Check(m.saveSlot == L"My World" && m.metadataName == L"main", "the slot identity");
    Check(m.currentLevel == L"Untitled_1", "a name property");

    WM::WorldMeta d;
    Check(WM::ReadWorldMeta(WriteFile(dir, "auto.meta", Compress(MetaPayload(false), 0x20000)), d) &&
              !d.hasGameMode, "an omitted mode stays omitted");

    uint64_t logical = 0;
    Check(WM::LogicalSize(WriteFile(dir, "big.sav", file), logical) && logical == payload.size(),
          "the logical size is the inflated size");
    const std::vector<uint8_t> gvas = {'G', 'V', 'A', 'S', 1, 2, 3, 4, 5};
    Check(!WM::IsCompressed(gvas.data(), gvas.size()), "a GVAS file is not compressed");
    Check(WM::LogicalSize(WriteFile(dir, "flat.sav", gvas), logical) && logical == gvas.size(),
          "a flat file's logical size is its size");

    std::vector<uint8_t> cut(file.begin(), file.end() - 5);
    Check(!WM::Decompress(cut.data(), cut.size(), back) && back.empty(), "a truncated file is refused");
    Check(!WM::LogicalSize(WriteFile(dir, "cut.sav", cut), logical), "a truncated file has no logical size");
    std::vector<uint8_t> bad = file;
    bad[24] ^= 0x40;  // the first group's stated size no longer matches its chunk
    Check(!WM::Decompress(bad.data(), bad.size(), back), "a size mismatch is refused");

    std::filesystem::remove_all(dir);
    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
