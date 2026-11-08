// coop/save/save_block.cpp -- see coop/save/save_block.h.

#include "coop/save/save_block.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/save/save_player_filter.h"
#include "ue_wrap/actors/inventory_component.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/save_to_slot_hook.h"
#include "ue_wrap/engine/world_folder.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

// The world-save verbs (sdk_profile_names.h says what each writes). Declared rows: the body each
// class owns, so a subclass override would need its own row.
UE_FACT_FN(kRowGamemodeSave, "save_block", L"mainGamemode_C", L"save", Declared);
UE_FACT_FN(kRowSlotSave, "save_block", L"saveSlot_C", L"save", Declared);
UE_FACT_FN(kRowSlotSaveToSlot, "save_block", L"saveSlot_C", L"saveToSlot", Declared);
UE_FACT_FN(kRowSlotSaveManual, "save_block", L"saveSlot_C", L"saveManual", Declared);
UE_FACT_FN(kRowSlotSavePlayerOnly, "save_block", L"saveSlot_C", L"savePlayerOnly", Declared);
UE_FACT_FN(kRowSlotSaveSelf, "save_block", L"saveSlot_C", L"saveSelf", Declared);
UE_FACT_FN(kRowMetaSave, "save_block", L"save_metadata_C", L"saveMetadata", Declared);
UE_FACT_FN(kRowMetaSaveSelf, "save_block", L"save_metadata_C", L"saveSelf", Declared);
UE_FACT_FN(kRowPersistSaveSelf, "save_block", L"save_persistent_C", L"saveSelf", Declared);
// The world file carries the host's key bindings (save_main_C keybinds, keybinds_keys, keybindsnames) and
// mainGamemode_C::receiveBeginPlay applies them to the engine's input settings through this function.
UE_FACT_FN(kRowMainSaveKeybinds, "save_block", L"save_main_C", L"saveKeybinds", Declared);

namespace coop::save_block {

namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace N = ue_wrap::profile::name;

namespace {

// The session whose role the gates read at the moment a save fires.
std::atomic<coop::net::Session*> g_session{nullptr};

// The native save-cycle hold: the gamemode disableSave is held on, by slot, serial and world.
// disableSave is a BP BoolProperty, written through its real byte and mask.
ue_wrap::CachedObjRef g_gm;
int32_t g_disableSaveOff = -1;
uint8_t g_disableSaveMask = 0;

bool g_watched = false;
WrittenFn g_written = nullptr;

constexpr int kTagGamemodeSave = 1;
constexpr int kTagSaveToSlot = 2;
constexpr int kTagOther = 3;
constexpr int kTagKeybinds = 4;

// The words the client sees when it tries to save.
constexpr const wchar_t* kNotHostLine = L"You're not the host, you can't save.";

// The world a client plays in is a mirror of the host's, and it can outlive the session that
// built it: the session is stopped BEFORE the trip to the menu, and a trip that fails leaves the
// mirror loaded. `g_gm` is that world's gamemode (the one Tick holds disableSave on).
bool MirrorWorldIsCurrent() {
    if (!g_gm.Get()) return false;
    void* const now = ue_wrap::world_identity::CurrentWorld();
    return now && g_gm.StampedWorld() == now;
}

// Decided when a save fires, never cached: a client of a running session, or a process whose
// loaded world is still that client's mirror. A host, a solo world, and a process that left a
// session and loaded a world of its own save.
bool MustNotSave() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    const bool client = s && s->running() && s->role() == coop::net::Role::Client;
    return client || MirrorWorldIsCurrent();
}

void LogBlocked(const char* what) {
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 3 && (n % 20) != 0) return;
    UE_LOGI("save_block: refused the client's %s (refusal #%llu) -- only the host saves; the client's "
            "files are left untouched", what, static_cast<unsigned long long>(n));
}

// A save the player asked for: the gamemode's save called from the player (the quicksave and
// backup-save keys) or from the pause menu, never as an autosave. Every other caller is the game
// saving on its own (a bed, a trigger, the autosave timer, the post-load save).
bool IsManualAttempt(const SG::Call& c) {
    if (!c.callerObject || !R::IsLive(c.callerObject)) return false;
    const int32_t autoOff = R::FindParamOffset(c.function, N::MainGamemodeSaveAutosaveParam);
    if (autoOff >= 0 && c.locals[autoOff] != 0) return false;
    for (void* cls = R::ClassOf(c.callerObject); cls; cls = R::SuperStructOf(cls)) {
        const std::wstring name = R::ToString(R::NameOf(cls));
        if (name == N::MainPlayerClass || name == N::UiMenuClass) return true;
    }
    return false;
}

// The one line per attempt, at most one in two seconds: a held quicksave key repeats.
void TellNotHost() {
    static ULONGLONG sLastMs = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (sLastMs && now - sLastMs < 2000) return;
    sLastMs = now;
    coop::chat_feed::Push(std::wstring(kNotHostLine), coop::chat_feed::Keep::Transient);
}

SG::Verdict OnSavePre(const SG::Call& c) {
    if (!MustNotSave()) return SG::Verdict::Run;
    if (c.tag == kTagGamemodeSave) {
        const bool manual = IsManualAttempt(c);
        LogBlocked(manual ? "save request" : "automatic save");
        if (manual) TellNotHost();
    } else {
        LogBlocked("world write");
    }
    return SG::Verdict::Cancel;
}

// When the running saveToSlot began (FILETIME ticks), so its post can tell a file it rewrote from
// one it left alone. 0 outside a call. saveToSlot does not nest.
uint64_t g_saveToSlotStart = 0;

// Which entry the running saveToSlot writes: saveToSlot(asBackup, asAutosave) puts the autosave
// into autosave.sav of the same folder, from the same save object that owns main.sav. Unknown
// when the parameter did not resolve; the post then accepts whichever of the two files was written.
enum class WrittenEntry : uint8_t { Unknown, Main, Autosave };
WrittenEntry g_saveToSlotEntry = WrittenEntry::Unknown;

uint64_t NowFileTime() {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

SG::Verdict OnSaveToSlotPre(const SG::Call& c) {
    const SG::Verdict v = OnSavePre(c);
    g_saveToSlotStart = v == SG::Verdict::Run ? NowFileTime() : 0;
    g_saveToSlotEntry = WrittenEntry::Unknown;
    if (v == SG::Verdict::Run) {
        const int32_t autoOff = R::FindParamOffset(c.function, N::MainGamemodeSaveAutosaveParam);
        if (autoOff >= 0)
            g_saveToSlotEntry = c.locals[autoOff] != 0 ? WrittenEntry::Autosave : WrittenEntry::Main;
        else
            UE_LOGW("save_block: saveToSlot has no '%ls' parameter -- the entry it wrote is read off "
                    "the files", N::MainGamemodeSaveAutosaveParam);
    }
    return v;
}

struct EntryFile {
    std::wstring path;
    uint64_t     written = 0;  // FILETIME of its last write
    uint64_t     bytes = 0;
    bool         exists = false;
};

EntryFile StatEntry(void* saveObject, const wchar_t* entry) {
    EntryFile e;
    e.path = ue_wrap::world_folder::SaveObjectFile(saveObject, entry);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (e.path.empty() || !::GetFileAttributesExW(e.path.c_str(), GetFileExInfoStandard, &fa)) return e;
    e.exists = true;
    e.written = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    e.bytes = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    return e;
}

// Rewritten since the call began, and not empty. Two seconds of slack for the file system's
// timestamp resolution.
bool Rewritten(const EntryFile& e, uint64_t started) {
    constexpr uint64_t kSlack = 2ull * 10000000ull;
    return e.exists && e.bytes != 0 && !(started && e.written + kSlack < started);
}

// The body returning is not the file being written: saveToSlot's own return value is never assigned
// by its body, and SaveGameCompressed's result is only printed. The entry file it wrote is read
// instead: main.sav, or autosave.sav when the call was an autosave -- rewritten since the call
// began, and not empty.
void OnSaveToSlotPost(const SG::Call& c) {
    const uint64_t started = g_saveToSlotStart;
    const WrittenEntry want = g_saveToSlotEntry;
    g_saveToSlotStart = 0;
    g_saveToSlotEntry = WrittenEntry::Unknown;
    if (MustNotSave()) return;
    const EntryFile mainFile = StatEntry(c.object, N::WorldEntryMain);
    const EntryFile autoFile = StatEntry(c.object, N::WorldEntryAutosave);
    bool autosave = want == WrittenEntry::Autosave;
    if (want == WrittenEntry::Unknown) autosave = !Rewritten(mainFile, started) && Rewritten(autoFile, started);
    const EntryFile& e = autosave ? autoFile : mainFile;
    if (e.path.empty() || !e.exists) {
        UE_LOGE("save_block: saveToSlot on %p returned but its %ls file '%ls' %s -- not counted as a world save",
                c.object, autosave ? N::WorldEntryAutosave : N::WorldEntryMain,
                e.path.empty() ? L"<unresolved>" : e.path.c_str(), e.path.empty() ? "is not named" : "does not exist");
        return;
    }
    if (!Rewritten(e, started)) {
        UE_LOGE("save_block: saveToSlot on %p returned but '%ls' was not rewritten (%llu bytes, last written "
                "%llu ms before the call) -- not counted as a world save", c.object, e.path.c_str(),
                static_cast<unsigned long long>(e.bytes),
                static_cast<unsigned long long>(started > e.written ? (started - e.written) / 10000ull : 0ull));
        return;
    }
    UE_LOGI("save_block: world save written -- %ls entry '%ls' (%llu bytes)",
            autosave ? N::WorldEntryAutosave : N::WorldEntryMain, e.path.c_str(),
            static_cast<unsigned long long>(e.bytes));
    // What the host carries now, beside the file: an empty inventory in a save that was written is the
    // line to find when a reload comes back empty.
    if (void* const local = coop::players::Registry::Get().Local()) {
        const ue_wrap::inventory_component::Counts n = ue_wrap::inventory_component::CountItems(local);
        UE_LOGI("save_block: the local player %p holds carried=%d hands=%d equipment=%d slotC=%d slotD=%d as the "
                "save finished", local, n.carried, n.hands, n.equipment, n.slotC, n.slotD);
    }
    if (g_written) g_written(c.object, autosave);
}

// The gate on the engine's SaveGameToSlot (ue_wrap/engine/save_to_slot_hook): a flat-slot write of
// the world-save container, which a few blueprints still make. Shut on the same terms as the verbs.
bool WorldSaveGate(void* /*saveObject*/, const wchar_t* slotName) {
    if (!MustNotSave()) return true;
    static std::atomic<uint64_t> sCount{0};
    if (sCount.fetch_add(1, std::memory_order_relaxed) < 3)
        UE_LOGI("save_block: refused a client SaveGameToSlot of the world to '%ls'", slotName);
    return false;
}

// Key bindings are the player's own and never cross: a client that loaded the host's world keeps the bindings
// its own game loaded from its own keybinds.sav, instead of taking the ones stored in the host's world file.
SG::Verdict OnKeybindsPre(const SG::Call&) {
    if (!MustNotSave()) return SG::Verdict::Run;
    static std::atomic<uint64_t> sCount{0};
    if (sCount.fetch_add(1, std::memory_order_relaxed) < 3)
        UE_LOGI("player_settings: client kept its own key bindings -- save_main_C::saveKeybinds of the host's "
                "world was not applied");
    return SG::Verdict::Cancel;
}

bool WatchAll() {
    bool ok = SG::WatchName(kRowGamemodeSave, kTagGamemodeSave, &OnSavePre, nullptr);
    ok = SG::WatchName(kRowMainSaveKeybinds, kTagKeybinds, &OnKeybindsPre, nullptr) && ok;
    ok = SG::WatchName(kRowSlotSaveToSlot, kTagSaveToSlot, &OnSaveToSlotPre, &OnSaveToSlotPost) && ok;
    for (const ue_wrap::facts::Function* row : {&kRowSlotSave, &kRowSlotSaveManual, &kRowSlotSavePlayerOnly,
                                                &kRowSlotSaveSelf, &kRowMetaSave, &kRowMetaSaveSelf,
                                                &kRowPersistSaveSelf})
        ok = SG::WatchName(*row, kTagOther, &OnSavePre, nullptr) && ok;
    return ok;
}

}  // namespace

void SetWritten(WrittenFn fn) { g_written = fn; }

void Pump() {
    if (!g_watched || SG::PendingNameCount() == 0) return;
    SG::ResolvePendingNames();
    coop::save_player_filter::Pump();
}

void Install(coop::net::Session* session) {
    if (!session) return;  // session not ready yet -- retry next pump tick
    g_session.store(session, std::memory_order_release);
    if (!g_watched) {
        g_watched = true;
        if (!WatchAll())
            UE_LOGE("save_block: the script gate refused a save-verb watch -- a client save may reach disk");
        else
            UE_LOGI("save_block: the world-save verbs are gated (a client's save is refused at the verb)");
    }
    coop::save_player_filter::Install();  // a puppet or the preview mannequin is never written into the world
    ue_wrap::save_to_slot_hook::SetGate(&WorldSaveGate);
    ue_wrap::save_to_slot_hook::Install();
}

void Tick(coop::net::Session* session) {
    if (!session || !session->running() || session->role() != coop::net::Role::Client) return;

    // Steady state: cached live gamemode -> one masked-bit read, write only on the false->true edge
    // (the bit is ours to hold: no bytecode ever writes it).
    if (!g_gm.Get()) {
        void* gm = ue_wrap::world_singleton::Gamemode();
        if (!gm) return;
        if (g_disableSaveOff < 0) {
            static bool sNoField = false;
            if (sNoField) return;
            if (!R::FindBoolProperty(R::ClassOf(gm), L"disableSave", g_disableSaveOff, g_disableSaveMask)) {
                sNoField = true;
                UE_LOGE("save_block: mainGamemode_C.disableSave did not resolve -- the native save cycle "
                        "is not held off (the verb gates still refuse)");
                return;
            }
        }
        g_gm.Set(gm);
    }

    auto* p = reinterpret_cast<uint8_t*>(g_gm.Raw()) + g_disableSaveOff;
    if ((*p & g_disableSaveMask) == 0) {
        *p |= g_disableSaveMask;
        UE_LOGI("save_block: client native save cycle OFF -- disableSave=true on gamemode %p "
                "(+0x%X mask 0x%02X; saveSlot_C::save returns on it before the gather)",
                g_gm.Raw(), static_cast<unsigned>(g_disableSaveOff), g_disableSaveMask);
    }
}

}  // namespace coop::save_block
