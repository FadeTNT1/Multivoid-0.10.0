// coop/player/player_profile_store.cpp -- see coop/player/player_profile_store.h.

#include "coop/player/player_profile_store.h"

#include "coop/atomic_file/atomic_file.h"
#include "coop/items/inventory_wire.h"  // a blob's item bytes, for the shrink check
#include "coop/items/player_profile.h"
#include "coop/mod_data/legacy_note.h"
#include "coop/net/blob_chunks.h"  // Fnv64, the file's integrity hash
#include "coop/save/save_transfer.h"  // HostSlot, the world the profiles belong to
#include "coop/session/player_handshake.h"  // IsValidGuid
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"
#include "ue_wrap/core/sdk_profile_names.h"  // the world entries' names
#include "ue_wrap/engine/world_folder.h"      // SplitKey, MakeKey

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace coop::player_profile_store {
namespace {

namespace fs = std::filesystem;

// The newest profile per GUID this host session has been handed, and the copy of it taken the last
// time the world was gathered, which is the one a save writes.
struct Held {
    std::vector<uint8_t> blob;
    std::string          nick;
    uint64_t             hash = 0;
    bool                 changed = false;   // since the last gather
    std::vector<uint8_t> atGather;          // valid while `uncut`
    std::string          nickAtGather;
    bool                 uncut = false;     // gathered, not yet on disk
    // The newest blob that held a real inventory (kPopulatedItemBytes or more of items). A profile
    // that later collapses is cut to disk as it is, and this copy goes to the rotating backups beside
    // it, so a wipe -- a death, a rescue gone wrong, a stream of an emptied world -- never takes the
    // last full inventory with it. Cut with the world save (CutToDisk), like the profile itself.
    std::vector<uint8_t> lastGood;
    std::string          goodNick;
    bool                 goodUncut = false;
};

// A profile "holds an inventory" from this many bytes of item data. A starting kit is about this, a
// played inventory is many times it (a 36 KB profile against a 1 KB empty one in the run that
// motivated this), and an empty inventory is a short header.
constexpr size_t kPopulatedItemBytes = 4096;
// A new profile this many times smaller than the populated one it replaces is a shrink.
constexpr size_t kShrinkFactor = 4;
// How many past full inventories are kept per player on disk: <guid>.json.good, .good.1, .good.2.
constexpr int kGoodGenerations = 3;
std::unordered_map<std::string, Held> g_held;
std::unordered_set<std::string> g_quarantined;  // stored profile unreadable: never written over

// The slot whose directory holds this world's profile set on disk: the slot the world was loaded
// from, then whichever slot a save of it was last written to. Empty while the world has no file.
std::wstring g_setSlot;

// What is held belongs to ONE loaded world. A process names the slot once per world it loads
// (save_transfer::SetHostWorld), so a new serial means another world took this one's place -- and
// that world is as old as its save, so the profiles to go with it are the files', not the ones
// held from the world before.
uint32_t g_heldForSerial = 0;
void DropHeldOfAnotherWorld() {
    const uint32_t serial = coop::save_transfer::HostSlotSerial();
    if (serial == g_heldForSerial) return;
    if (!g_held.empty())
        UE_LOGI("player_profile: another world was loaded to host -- %zu held profile(s) of the "
                "previous one dropped", g_held.size());
    g_held.clear();
    g_quarantined.clear();
    g_setSlot = coop::save_transfer::HostSlot();
    g_heldForSerial = serial;
}

// <mod data dir>/coop_players/<folder> for a world's main entry and
// <mod data dir>/coop_players/<folder>/autosave for its autosave, empty if a piece is missing. The
// key is a world key ("<folder>" or "<folder>/autosave", ue_wrap/engine/world_folder). Each entry
// holds the profiles that were cut beside ITS file: main.sav and autosave.sav are two moments of
// the world, so a player loading one must get the items of that moment, not of the other's. The
// folder becomes ONE path component, so a name that could be more than one is refused; the
// autosave's directory sits inside the main's, which only ever lists regular <guid>.json files.
fs::path SlotDir(const std::wstring& key) {
    std::wstring slot, entry;
    if (!ue_wrap::world_folder::SplitKey(key, slot, entry)) return {};
    if (slot == L"." || slot == L".." || slot.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return {};
    const std::wstring base = ue_wrap::paths::ModDataDir();
    if (base.empty()) return {};
    fs::path dir = fs::path(base) / L"coop_players" / slot;
    if (entry == ue_wrap::profile::name::WorldEntryAutosave) dir /= entry;
    return dir;
}

bool IsAutosaveKey(const std::wstring& key) {
    std::wstring folder, entry;
    return ue_wrap::world_folder::SplitKey(key, folder, entry) &&
           entry == ue_wrap::profile::name::WorldEntryAutosave;
}

// The directory a set is READ from. An autosave key whose own directory does not exist yet is a
// world saved by a build that kept one set per folder: its profiles are the shared directory's, read
// from there (never written to) until the first save of the autosave seeds its own (CutToDisk).
fs::path ReadDir(const std::wstring& key) {
    const fs::path dir = SlotDir(key);
    if (dir.empty() || !IsAutosaveKey(key)) return dir;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) return dir;
    std::wstring folder, entry;
    ue_wrap::world_folder::SplitKey(key, folder, entry);
    const fs::path shared = SlotDir(folder);
    if (shared.empty() || !fs::is_directory(shared, ec)) return dir;
    static std::unordered_set<std::wstring> sTold;
    if (sTold.insert(key).second)
        UE_LOGI("player_profile: '%ls' has no profile set of its own yet -- reading the folder's shared "
                "set at '%ls' (read-only) until its first save writes one", key.c_str(), shared.c_str());
    return shared;
}

// <mod data dir>/coop_players/<the set's slot>/<guid>.json, empty if any piece is missing.
fs::path PlayerFilePathIn(const fs::path& dir, const std::string& guid) {
    // Defence in depth (the wire boundary already validates): a GUID that is not exactly 32 hex
    // characters never becomes a path component; an empty path makes every write no-op, so a
    // non-hex GUID cannot traverse.
    if (!coop::player_handshake::IsValidGuid(guid)) return {};
    if (dir.empty()) return {};
    return dir / (std::wstring(guid.begin(), guid.end()) + L".json");
}

// Where the set is written, and where it is read.
fs::path PlayerFilePath(const std::string& guid) { return PlayerFilePathIn(SlotDir(g_setSlot), guid); }
fs::path PlayerReadPath(const std::string& guid) { return PlayerFilePathIn(ReadDir(g_setSlot), guid); }

// The name a rotating backup of <guid>.json carries: generation 0 is the newest.
std::wstring GoodSuffix(int generation) {
    return generation == 0 ? std::wstring(L".good") : L".good." + std::to_wstring(generation);
}

// Is this one of the set's files: <32 hex>.json, or the .bak or a .good backup of one?
bool IsProfileFile(const fs::path& file) {
    std::wstring name = file.filename().wstring();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, L".bak") == 0) name.resize(name.size() - 4);
    for (int g = 0; g < kGoodGenerations; ++g) {
        const std::wstring suffix = GoodSuffix(g);
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    if (name.size() != 32 + 5 || name.compare(32, 5, L".json") != 0) return false;
    std::string guid;
    for (size_t i = 0; i < 32; ++i) {
        if (name[i] > 0x7F) return false;
        guid.push_back(static_cast<char>(name[i]));
    }
    return coop::player_handshake::IsValidGuid(guid);
}

// The set's files in `dir`. False when the directory could not be read to its end: a listing cut
// short is not the set, and a caller that removes what is "not in the set" must not act on one.
bool ProfileFilesIn(const fs::path& dir, std::vector<fs::path>& out) {
    out.clear();
    bool everyEntryAnswered = true;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        // Its own error code: the step to the next entry writes `ec` again, and a failure left in
        // it would be gone by the time the loop is over.
        std::error_code entryEc;
        const bool regular = it->is_regular_file(entryEc);
        if (entryEc) { everyEntryAnswered = false; continue; }
        if (regular && IsProfileFile(it->path())) out.push_back(it->path());
    }
    return everyEntryAnswered && !ec;
}

std::string Hex(const std::vector<uint8_t>& b) {
    static const char k[] = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) { s.push_back(k[c >> 4]); s.push_back(k[c & 0xF]); }
    return s;
}

// Decode a hex string of either case to bytes. False on an odd length or a non-hex digit.
bool UnHex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() & 1) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Extract the value of a string field from our flat JSON (no nesting or escapes are used).
bool JsonStr(const std::string& doc, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\":\"";
    const size_t k = doc.find(needle);
    if (k == std::string::npos) return false;
    const size_t v = k + needle.size();
    const size_t e = doc.find('"', v);
    if (e == std::string::npos) return false;
    out = doc.substr(v, e - v);
    return true;
}

// Parse one stored file into `outBlob`. Defensive against a hand edit: requires the magic, a
// parseable hex blob and a matching FNV. False on any failure (the caller tries the .bak); never
// throws, never returns unverified bytes.
bool ParseBlobFile(const fs::path& file, std::vector<uint8_t>& outBlob) {
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)) return false;
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::string doc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string magic;
    if (!JsonStr(doc, "magic", magic) || magic != "VCPI") {
        UE_LOGW("player_profile: '%ls' missing/bad magic -- treating as corrupt", file.c_str());
        return false;
    }
    std::string blobHex, fnvHex;
    if (!JsonStr(doc, "blob", blobHex) || !JsonStr(doc, "fnv", fnvHex)) return false;
    std::vector<uint8_t> blob;
    if (!UnHex(blobHex, blob)) {
        UE_LOGW("player_profile: '%ls' blob hex unparseable -- corrupt", file.c_str());
        return false;
    }
    char want[17] = {};
    std::snprintf(want, sizeof(want), "%016llx",
                  static_cast<unsigned long long>(coop::blob_chunks::Fnv64(blob)));
    if (fnvHex != want) {
        UE_LOGW("player_profile: '%ls' FNV mismatch (have %s, stored %s) -- corrupt/tampered",
                file.c_str(), want, fnvHex.c_str());
        return false;
    }
    outBlob = std::move(blob);
    return true;
}

bool ReadFile(const std::string& guid, std::vector<uint8_t>& outBlob) {
    const fs::path file = PlayerReadPath(guid);
    if (file.empty()) return false;
    if (ParseBlobFile(file, outBlob)) return true;
    const fs::path bak = fs::path(file).concat(L".bak");
    if (ParseBlobFile(bak, outBlob)) {
        UE_LOGW("player_profile: recovered guid=%s inventory from .bak (primary corrupt)",
                guid.c_str());
        return true;
    }
    return false;
}

// One profile file at `file`, atomically; no backup of what it replaces (the callers decide).
bool WriteJson(const fs::path& file, const std::vector<uint8_t>& blob, const std::string& nickJson) {
    const uint64_t fnv = coop::blob_chunks::Fnv64(blob);
    const long long epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char fnvhex[17] = {};
    std::snprintf(fnvhex, sizeof(fnvhex), "%016llx", static_cast<unsigned long long>(fnv));
    std::string json = "{\"magic\":\"VCPI\",\"ver\":1,\"fnv\":\"";
    json += fnvhex;
    json += "\",\"nick\":\"";
    json += nickJson;
    json += "\",\"lastSeen\":";
    json += std::to_string(epoch);
    json += ",\"blob\":\"";
    json += Hex(blob);
    json += "\"}\n";
    const atomic_file::Result r =
        atomic_file::Write(file, json, atomic_file::Mode::Replace, atomic_file::Sync::ToDisk);
    if (!r.ok()) {
        UE_LOGE("player_profile: write of '%ls' failed (%s)", file.c_str(),
                atomic_file::Describe(r).c_str());
        return false;
    }
    return true;
}

bool WriteFile(const std::string& guid, const std::vector<uint8_t>& blob, const std::string& nickJson) {
    const fs::path file = PlayerFilePath(guid);
    if (file.empty()) return false;
    std::error_code ec;
    // Keep the last good file as .bak before overwriting.
    if (fs::exists(file, ec))
        fs::copy_file(file, fs::path(file).concat(L".bak"), fs::copy_options::overwrite_existing, ec);
    return WriteJson(file, blob, nickJson);
}

// Put `blob` at the head of the player's rotating backups: what stood at .good moves to .good.1, that
// to .good.2, and the last one goes. A blob the newest backup already is changes nothing, so a cut
// that finds the same full inventory again does not push the older ones out. The directory is the
// profile set's (the caller has made it). False leaves the backups as they were.
bool RotateGood(const std::string& guid, const std::vector<uint8_t>& blob, const std::string& nickJson) {
    const fs::path base = PlayerFilePath(guid);
    if (base.empty()) return false;
    const auto at = [&](int g) { return fs::path(base).concat(GoodSuffix(g)); };
    std::vector<uint8_t> newest;
    if (ParseBlobFile(at(0), newest) && newest == blob) return true;
    std::error_code ec;
    for (int g = kGoodGenerations - 1; g >= 1; --g) {
        if (fs::exists(at(g - 1), ec))
            fs::copy_file(at(g - 1), at(g), fs::copy_options::overwrite_existing, ec);
    }
    return WriteJson(at(0), blob, nickJson);
}

// The bytes of item data in a profile blob; 0 when it does not parse or holds none.
size_t ItemBytesOf(const std::vector<uint8_t>& blob) {
    coop::player_profile::Profile p;
    return coop::inventory_wire::Deserialize(blob, p) && p.hasItems ? p.items.size() : 0;
}

// A save of this world was written to `written`, which is not where the set stands: a quicksave
// names a new <main>_SUB_<n> file, and a plain save made after loading a subsave names the main
// slot (saveSlot_C::saveToSlot). That file now holds THIS world, so the directory beside it has to
// hold this world's set and nothing else: what stood there belonged to the world the file held
// before, and a player of that world would come back carrying items this one still has on the
// floor. The set is copied byte for byte -- a file that does not read stays as it is there too --
// and stands under `written` from then on. False leaves everything as it was for the next save.
//
// Nothing is removed before every copy has landed, and nothing at all on an answer that is not an
// answer: a directory that cannot be asked about is not an absent one, and a listing cut short is
// not the set. With no slot known for this world (a process never told which file it came from)
// the lineage is unknown too, so the directory beside the written file is taken as it stands; a
// save the host picker creates is cleared where it is created (ForgetSlot).
bool FollowWrittenSlot(const std::wstring& written) {
    if (written == g_setSlot) {
        // An autosave that was loaded from a folder's shared set has no directory of its own yet:
        // its first cut seeds one from the shared set, so the profiles it was not handed again are
        // not left behind in a directory that now holds only the changed ones.
        const fs::path own = SlotDir(written);
        if (own.empty() || ReadDir(written) == own) return true;
    }
    const fs::path to = SlotDir(written);
    std::error_code ec;
    if (!to.empty()) fs::create_directories(to, ec);
    if (to.empty() || ec) {
        UE_LOGW("player_profile: the profile directory of the slot '%ls' could not be made (%s) -- "
                "nothing cut, everything stays for the next save", written.c_str(),
                to.empty() ? "not a name for one directory, or no data folder" : ec.message().c_str());
        return false;
    }
    if (g_setSlot.empty()) {
        UE_LOGI("player_profile: the world was saved to '%ls' and no slot was known for it -- its "
                "profiles stand there from now on, the directory taken as it is", written.c_str());
        g_setSlot = written;
        return true;
    }
    const fs::path from = ReadDir(g_setSlot);
    const fs::file_type fromIs = from.empty() ? fs::file_type::not_found : fs::status(from, ec).type();
    if (fromIs != fs::file_type::directory && fromIs != fs::file_type::not_found) {
        UE_LOGE("player_profile: '%ls' could not be asked about (%s) -- nothing cut, everything "
                "stays for the next save", from.c_str(), ec.message().c_str());
        return false;
    }
    const bool haveFrom = fromIs == fs::file_type::directory;
    if (haveFrom && fs::equivalent(from, to, ec)) {  // one directory under two spellings
        g_setSlot = written;
        return true;
    }
    std::vector<fs::path> set, there;
    if ((haveFrom && !ProfileFilesIn(from, set)) || !ProfileFilesIn(to, there)) {
        UE_LOGE("player_profile: the profiles under '%ls' or '%ls' could not be listed -- nothing "
                "cut, everything stays for the next save", g_setSlot.c_str(), written.c_str());
        return false;
    }
    // Names compared the way the file system compares them: one file under two spellings of its
    // hex would otherwise be copied and then removed as a stranger.
    auto folded = [](const fs::path& file) {
        std::wstring name = file.filename().wstring();
        for (wchar_t& c : name)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        return name;
    };
    std::unordered_set<std::wstring> names;
    for (const fs::path& file : set) {
        // Cached: the old slot keeps the intact set, so a power cut just after the copy can cost
        // the new slot's copy and never the old one.
        // A stream iterator stops at the first read error without setting badbit, so the bytes are
        // checked against the size the file system gave before the read.
        std::string bytes;
        std::string why = "could not read";
        {
            std::error_code sizeEc;
            const std::uintmax_t size = fs::file_size(file, sizeEc);
            std::ifstream in(file, std::ios::binary);
            if (in && !sizeEc) {
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (bytes.size() == size) why.clear();
            }
        }
        if (why.empty()) {
            const atomic_file::Result r = atomic_file::Write(
                to / file.filename(), bytes, atomic_file::Mode::Replace, atomic_file::Sync::Cached);
            if (!r.ok()) why = atomic_file::Describe(r);
        }
        if (!why.empty()) {
            UE_LOGE("player_profile: copying '%ls' beside the slot '%ls' failed (%s) -- nothing "
                    "removed there, nothing cut, everything stays for the next save", file.c_str(),
                    written.c_str(), why.c_str());
            return false;
        }
        names.insert(folded(file));
    }
    size_t removed = 0;
    for (const fs::path& stale : there)
        if (!names.count(folded(stale)) && fs::remove(stale, ec)) ++removed;
    UE_LOGI("player_profile: the world was saved to '%ls', its profiles stood under '%ls' -- %zu "
            "file(s) of the set copied beside the new file, %zu left by the world that slot held "
            "before removed", written.c_str(), g_setSlot.c_str(), set.size(), removed);
    g_setSlot = written;
    return true;
}

// A new profile arrives for `h`. When it holds an inventory it becomes the player's last full one; when it
// is a collapse of one the player held (a death, a rescue gone wrong, a stream of an emptied world), that
// is said loudly and the last full inventory is made sure to reach the backups. Nothing is refused: a
// player who really dropped everything is as entitled to an empty profile as to any other, so the new
// profile stands, and the old one is kept beside it.
void NoteShrinkAndGood(const std::string& guid, Held& h, const std::vector<uint8_t>& incoming,
                       const std::string& nickJson) {
    const size_t newItems = ItemBytesOf(incoming);
    std::vector<uint8_t> fromDisk;
    const std::vector<uint8_t>* prev = h.blob.empty() ? nullptr : &h.blob;
    if (!prev && ReadFile(guid, fromDisk)) prev = &fromDisk;
    const size_t prevItems = prev ? ItemBytesOf(*prev) : 0;
    if (newItems >= kPopulatedItemBytes) {
        h.lastGood = incoming;
        h.goodNick = nickJson;
        h.goodUncut = true;
        return;
    }
    if (prevItems >= kPopulatedItemBytes && newItems * kShrinkFactor < prevItems) {
        if (h.lastGood.empty()) {  // the populated one came from disk, not from a Put of this session
            h.lastGood = *prev;
            h.goodNick = nickJson;
            h.goodUncut = true;
        }
        UE_LOGW("player_profile: PROFILE SHRANK guid=%s (%s) -- %zu bytes of items down to %zu (profile %zu "
                "-> %zu bytes). The new profile is kept as the player's current one, and their last full "
                "inventory is kept beside it as '%s.json%ls' in the world's profile set; to put it back, "
                "stop the host and copy that file over '%s.json'", guid.c_str(), nickJson.c_str(), prevItems,
                newItems, prev->size(), incoming.size(), guid.c_str(), GoodSuffix(0).c_str(), guid.c_str());
    }
}

}  // namespace

// Encoding is the codec's job; escaping is this site's, because the container is JSON. Raw UTF-8
// is valid inside a JSON string, only the two structural metacharacters have to go, and they
// cannot appear inside a multi-byte sequence (continuation bytes are all above 0x7F), so dropping
// them cannot corrupt one.
std::string NickForJson(const std::wstring& nick) {
    std::string s = coop::text::CapUtf8Bytes(coop::text::ToUtf8(nick), coop::text::kNickMaxBytes);
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](char c) { return c == '"' || c == '\\'; }),
            s.end());
    return s;
}

bool Put(const std::string& guid, std::vector<uint8_t> blob, const std::string& nickJson) {
    if (!coop::player_handshake::IsValidGuid(guid)) return false;
    DropHeldOfAnotherWorld();
    if (g_quarantined.count(guid)) return false;
    const uint64_t hash = coop::blob_chunks::Fnv64(blob);
    Held& h = g_held[guid];
    if (h.hash == hash && h.blob == blob) return false;
    NoteShrinkAndGood(guid, h, blob, nickJson);
    h.blob = std::move(blob);
    h.nick = nickJson;
    h.hash = hash;
    h.changed = true;
    return true;
}

Found Get(const std::string& guid, std::vector<uint8_t>& outBlob) {
    DropHeldOfAnotherWorld();
    const auto it = g_held.find(guid);
    if (it != g_held.end()) { outBlob = it->second.blob; return Found::Yes; }
    if (ReadFile(guid, outBlob)) return Found::Yes;
    const fs::path file = PlayerReadPath(guid);
    std::error_code ec;
    const bool onDisk = !file.empty() &&
                        (fs::exists(file, ec) || fs::exists(fs::path(file).concat(L".bak"), ec));
    return onDisk ? Found::Unreadable : Found::Absent;
}

void Quarantine(const std::string& guid) {
    DropHeldOfAnotherWorld();
    g_held.erase(guid);
    if (g_quarantined.insert(guid).second)
        UE_LOGE("player_profile: the stored profile of guid=%s did NOT read -- nothing of this player "
                "is held or written for the rest of this hosted world; the files are left as they "
                "are for recovery", guid.c_str());
}

void NoteLegacyFolder() {
    coop::legacy_note::Once(coop::legacy_note::LegacyFile::CoopPlayers, L"coop_players");
}

size_t ForgetSlot(const std::wstring& slot) {
    // A world's two entries each hold a set (SlotDir), and a deleted world leaves both.
    std::wstring folder, entry;
    if (!ue_wrap::world_folder::SplitKey(slot, folder, entry)) return 0;
    size_t removed = 0;
    std::error_code ec;
    for (const wchar_t* e : {ue_wrap::profile::name::WorldEntryMain, ue_wrap::profile::name::WorldEntryAutosave}) {
        const fs::path dir = SlotDir(ue_wrap::world_folder::MakeKey(folder, e));
        // A listing cut short still names files that are there, and every one of them is a stranger
        // to a save that was just created: what it did list goes.
        std::vector<fs::path> files;
        if (!dir.empty() && fs::is_directory(dir, ec)) ProfileFilesIn(dir, files);
        for (const fs::path& file : files)
            if (fs::remove(file, ec)) ++removed;
    }
    if (removed)
        UE_LOGI("player_profile: the save '%ls' is new -- %zu profile file(s) a deleted save of that "
                "name had left beside it removed", slot.c_str(), removed);
    return removed;
}

bool AnythingPending() {
    DropHeldOfAnotherWorld();
    for (const auto& [guid, h] : g_held)
        if (h.changed || h.uncut || h.goodUncut) return true;
    return false;
}

void MarkWorldGathered() {
    DropHeldOfAnotherWorld();
    for (auto& [guid, h] : g_held) {
        if (!h.changed) continue;
        h.atGather = h.blob;
        h.nickAtGather = h.nick;
        h.uncut = true;
        h.changed = false;
    }
}

size_t CutToDisk(const std::wstring& writtenSlot) {
    DropHeldOfAnotherWorld();
    if (!FollowWrittenSlot(writtenSlot)) return 0;
    size_t written = 0;
    bool dirReady = false;
    for (auto it = g_held.begin(); it != g_held.end();) {
        Held& h = it->second;
        if (h.uncut) {
            if (!dirReady) {  // one directory for every profile of this world
                const fs::path dir = SlotDir(g_setSlot);
                std::error_code ec;
                if (!dir.empty()) fs::create_directories(dir, ec);
                if (dir.empty() || ec) {
                    UE_LOGW("player_profile: the profile directory could not be made (%s) -- "
                            "nothing cut, everything stays for the next save",
                            dir.empty() ? "no data folder" : ec.message().c_str());
                    return 0;
                }
                dirReady = true;
            }
            if (WriteFile(it->first, h.atGather, h.nickAtGather)) {
                h.uncut = false;
                h.atGather = {};
                ++written;
            }  // else: stays for the next cut
        }
        if (h.goodUncut && !h.lastGood.empty()) {  // the last full inventory, beside the profile just cut
            const fs::path dir = SlotDir(g_setSlot);
            std::error_code ec;
            if (!dir.empty()) fs::create_directories(dir, ec);
            if (!dir.empty() && !ec && RotateGood(it->first, h.lastGood, h.goodNick)) {
                h.goodUncut = false;
                h.lastGood = {};
            }  // else: stays for the next cut
        }
        // What is on disk and unchanged since is the file's to give back (Get reads it), so it
        // need not be held: a host meets new identities for as long as its world lives.
        if (!h.uncut && !h.changed && !h.goodUncut) it = g_held.erase(it);
        else ++it;
    }
    return written;
}

namespace {

// A sidecar name: one plain file name, never a path, never a profile's own name.
bool SafeSidecarName(const std::wstring& name) {
    if (name.empty() || name.size() > 64 || name == L"." || name == L"..") return false;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    return !IsProfileFile(fs::path(name));
}

}  // namespace

bool WriteWorldSidecar(const std::wstring& fileName, const std::string& bytes) {
    DropHeldOfAnotherWorld();
    if (!SafeSidecarName(fileName)) return false;
    const fs::path dir = SlotDir(g_setSlot);
    if (dir.empty()) return false;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    const atomic_file::Result r =
        atomic_file::Write(dir / fileName, bytes, atomic_file::Mode::Replace, atomic_file::Sync::ToDisk);
    if (!r.ok()) {
        UE_LOGW("player_profile: the sidecar '%ls' beside the profiles could not be written (%s)", fileName.c_str(),
                atomic_file::Describe(r).c_str());
        return false;
    }
    return true;
}

bool ReadWorldSidecar(const std::wstring& fileName, std::string& out) {
    out.clear();
    DropHeldOfAnotherWorld();
    if (!SafeSidecarName(fileName)) return false;
    const fs::path dir = ReadDir(g_setSlot);
    if (dir.empty()) return false;
    std::ifstream in(dir / fileName, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

namespace {

// A relative path a host's admin typed to name a backup: no drive, no root, no `.` or `..` component, no characters a
// path cannot hold, and a .json file. It is joined under the world's own profile folder and never trusted beyond it.
bool SafeRelativeSpec(const std::string& spec) {
    if (spec.empty() || spec.size() > 200) return false;
    for (unsigned char c : spec)
        if (c < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    std::wstring wide;
    if (!coop::text::FromUtf8Strict(spec.data(), spec.size(), &wide)) return false;
    const fs::path p(wide);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const fs::path& part : p)
        if (part.empty() || part == "." || part == "..") return false;
    std::wstring ext = p.extension().wstring();
    for (wchar_t& ch : ext)
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    return ext == L".json";
}

// Is `full` under `base`, component for component, once both are normalised?
bool UnderDir(const fs::path& base, const fs::path& full) {
    const fs::path b = base.lexically_normal(), f = full.lexically_normal();
    auto bi = b.begin(), fi = f.begin();
    for (; bi != b.end(); ++bi, ++fi) {
        if (bi->empty()) continue;
        if (fi == f.end() || *bi != *fi) return false;
    }
    return true;
}

}  // namespace

bool ReadBackup(const std::string& guid, const std::string& specIn, std::vector<uint8_t>& blob, std::string& label,
                std::string& why) {
    DropHeldOfAnotherWorld();
    std::string spec = specIn;
    for (char& c : spec)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    // The directories a backup of this world may stand in: the one the set is cut to, and the world folder's own (the
    // main entry's), where a hand-made recovery folder sits.
    std::vector<fs::path> dirs;
    if (!g_setSlot.empty()) {
        const fs::path setDir = SlotDir(g_setSlot);
        if (!setDir.empty()) dirs.push_back(setDir);
        std::wstring folder, entry;
        if (ue_wrap::world_folder::SplitKey(g_setSlot, folder, entry)) {
            const fs::path folderDir = SlotDir(folder);
            if (!folderDir.empty() && (dirs.empty() || folderDir != dirs.front())) dirs.push_back(folderDir);
        }
    }
    if (dirs.empty()) {
        why = "the hosted world has no profile folder yet (it has not been saved)";
        return false;
    }
    int generation = -1;
    if (spec.empty() || spec == "good") generation = 0;
    else if (spec.rfind("good.", 0) == 0 && spec.size() == 6 && spec[5] >= '1' && spec[5] < '0' + kGoodGenerations)
        generation = spec[5] - '0';
    if (generation >= 0) {
        if (!coop::player_handshake::IsValidGuid(guid)) {
            why = "that player has no proved id, so no backup of theirs is on file";
            return false;
        }
        if (generation == 0) {   // the newest full inventory not cut to disk yet is newer than any file
            const auto it = g_held.find(guid);
            if (it != g_held.end() && !it->second.lastGood.empty()) {
                blob = it->second.lastGood;
                label = "the last full profile held in memory";
                return true;
            }
        }
        for (const fs::path& dir : dirs) {
            const fs::path file = fs::path(PlayerFilePathIn(dir, guid)).concat(GoodSuffix(generation));
            if (ParseBlobFile(file, blob)) {
                label = coop::text::ToUtf8(file.wstring());
                return true;
            }
        }
        why = "no readable backup '" + (spec.empty() ? std::string("good") : spec) + "' is on file for that player";
        return false;
    }
    if (!SafeRelativeSpec(specIn)) {
        why = "name a backup as good, good.1, good.2 or a relative path to a .json file in the world's profile folder";
        return false;
    }
    for (const fs::path& dir : dirs) {
        std::wstring wideSpec;
        coop::text::FromUtf8Strict(specIn.data(), specIn.size(), &wideSpec);   // SafeRelativeSpec has judged it
        const fs::path file = (dir / fs::path(wideSpec)).lexically_normal();
        std::error_code ec;
        if (!UnderDir(dir, file) || !fs::is_regular_file(file, ec)) continue;
        if (ParseBlobFile(file, blob)) {
            label = coop::text::ToUtf8(file.wstring());
            return true;
        }
        why = "'" + specIn + "' is not a readable profile (the magic or the checksum does not match)";
        return false;
    }
    why = "no file '" + specIn + "' is in the world's profile folder";
    return false;
}

std::string WriteRollback(const std::string& nickJson, const std::vector<uint8_t>& blob) {
    const std::wstring base = ue_wrap::paths::ModDataDir();
    if (base.empty()) return std::string();
    const fs::path dir = fs::path(base) / L"inventory_rollback";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return std::string();
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    wchar_t stamp[48] = {};
    std::wcsftime(stamp, 48, L"rollback-%Y%m%d-%H%M%S.json", &utc);
    const fs::path file = dir / stamp;
    return WriteJson(file, blob, nickJson) ? coop::text::ToUtf8(file.wstring()) : std::string();
}

}  // namespace coop::player_profile_store
