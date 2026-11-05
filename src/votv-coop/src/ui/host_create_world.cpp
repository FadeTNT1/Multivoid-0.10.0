// ui/host_create_world.cpp -- see ui/host_create_world.h.

#include "ui/host_create_world.h"

#include "ui/native_screen.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/umg_build.h"
#include "ue_wrap/engine/world_folder.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/world_singleton.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace ui::host_create_world {
namespace {

namespace R  = ue_wrap::reflection;
namespace U  = ue_wrap::umg;
namespace NS = ui::native_screen;
namespace N  = ue_wrap::profile::name;
namespace fs = std::filesystem;

bool  g_running = false;
void* g_saveSlots = nullptr;
int32_t g_saveSlotsIdx = -1;
int32_t g_innerOff = -1;
bool  g_sawPane = false;   // the create pane has been seen open
int   g_ticks = 0;
std::set<std::wstring> g_before;

// The world folders on disk now: those holding a main.meta, as the picker lists them.
std::set<std::wstring> WorldFolders() {
    std::set<std::wstring> out;
    const std::wstring dir = ue_wrap::world_folder::SaveGamesDir();
    if (dir.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it{fs::path(dir), ec}, end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2) &&
            fs::is_regular_file(it->path() / (std::wstring(N::WorldEntryMain) + N::WorldMetaExt), e2))
            out.insert(it->path().filename().wstring());
    }
    return out;
}

int32_t InnerIndex() {
    void* inner = NS::ReadPtr(g_saveSlots, g_innerOff);
    return inner ? U::SwitcherIndex(inner) : -1;
}

State Finish(State s) {
    g_running = false;
    g_saveSlots = nullptr;
    g_saveSlotsIdx = -1;
    g_sawPane = false;
    g_ticks = 0;
    return s;
}

}  // namespace

bool Begin(void* menuSwitcher) {
    if (g_running || !menuSwitcher) return false;
    void* saveSlots = NS::SwitcherChild(menuSwitcher, N::SaveSlotsWidgetClass);
    void* cls = saveSlots ? R::ClassOf(saveSlots) : nullptr;
    void* openFn = cls ? R::FindFunction(cls, N::SaveSlotsOpenCreateFn) : nullptr;
    const int32_t innerOff = cls ? R::FindPropertyOffset(cls, N::SaveSlotsInnerSwitcher) : -1;
    const int32_t idx = saveSlots ? U::IndexOfChild(menuSwitcher, saveSlots) : -1;
    if (!openFn || innerOff < 0 || idx < 0) {
        UE_LOGW("host_create_world: the game's save menu is not usable here (widget=%p open=%p inner=%d "
                "index=%d) -- Create world refused", saveSlots, openFn, innerOff, idx);
        return false;
    }
    // The game copies the game instance's mode into the new world's metadata, as its menu does once
    // a mode was picked; the hosting window creates story worlds, as its New game does.
    if (void* gi = ue_wrap::world_singleton::GameInstance())
        ue_wrap::game_mode::WriteTo(gi, ue_wrap::game_mode::kStory);
    g_before = WorldFolders();
    g_saveSlots = saveSlots;
    g_saveSlotsIdx = idx;
    g_innerOff = innerOff;
    g_sawPane = false;
    g_ticks = 0;
    U::SwitcherSetIndex(menuSwitcher, idx);
    const int32_t frameSize = R::FunctionFrameSize(openFn);
    std::vector<uint8_t> frame(frameSize > 0 ? static_cast<size_t>(frameSize) : 0u, 0u);
    if (!R::CallFunction(saveSlots, openFn, frame.empty() ? nullptr : frame.data())) {
        UE_LOGW("host_create_world: the save menu's create handler did not dispatch");
        Finish(State::Cancelled);
        return false;
    }
    g_running = true;
    UE_LOGI("host_create_world: the game's create pane is open (save menu at index %d, %zu world(s) on disk)",
            idx, g_before.size());
    return true;
}

State Poll(void* menuSwitcher, std::wstring& folder) {
    folder.clear();
    if (!g_running) return State::Idle;
    ++g_ticks;
    if (!R::IsLive(g_saveSlots)) return Finish(State::Cancelled);
    const int32_t inner = InnerIndex();
    if (inner == 1) g_sawPane = true;
    const bool paneClosed = g_sawPane && inner == 0;
    const bool leftMenu = U::SwitcherIndex(menuSwitcher) != g_saveSlotsIdx;
    // A pane that never opened is a handler that did nothing; a few seconds of menu ticks is ample.
    const bool neverOpened = !g_sawPane && g_ticks > 600;
    if (!paneClosed && !leftMenu && !neverOpened) return State::Running;

    for (const std::wstring& f : WorldFolders()) {
        if (g_before.count(f)) continue;
        folder = f;
        UE_LOGI("host_create_world: the game created the world '%ls'", f.c_str());
        return Finish(State::Created);
    }
    UE_LOGI("host_create_world: no world was created (%s) -- back to the hosting window",
            paneClosed ? "the pane closed" : leftMenu ? "the save menu was left" : "the pane never opened");
    return Finish(State::Cancelled);
}

bool Running() { return g_running; }

void Reset() {
    if (g_running) UE_LOGI("host_create_world: a creation in flight was dropped with its menu");
    Finish(State::Idle);
}

}  // namespace ui::host_create_world
