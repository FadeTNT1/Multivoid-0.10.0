// coop/save/save_block.cpp -- see coop/save/save_block.h.

#include "coop/save/save_block.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/session.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/save_to_slot_hook.h"
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

void OnSaveToSlotPost(const SG::Call& c) {
    if (g_written && !MustNotSave()) g_written(c.object);
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

bool WatchAll() {
    bool ok = SG::WatchName(kRowGamemodeSave, kTagGamemodeSave, &OnSavePre, nullptr);
    ok = SG::WatchName(kRowSlotSaveToSlot, kTagSaveToSlot, &OnSavePre, &OnSaveToSlotPost) && ok;
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
