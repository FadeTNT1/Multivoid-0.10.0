// coop/player/melee_fx_safe.cpp -- the safe seam of coop/player/melee_fx.h: the sounds the local player's dial turn makes
// on a combination safe's door (prop_safeDoor_C), made into kMeleeFxSafe effects.
//
// The dial is the safe door's own graph. Its mouse-wheel handlers (scrollUp, scrollDown), bound only on the machine
// of the player at the safe, play every sound through PlaySound2D: the click of a notch (tick1, mechanicalssmaller1),
// the tick of a right digit, the reset of a wrong code (mechanicalssmaller2), the unlock (mechanicalssmaller4), and,
// from the opening timeline's event, the door (rozdoor). PlaySound2D is heard by the player alone, so a bystander
// hears nothing. A scope over the three bodies captures what the door's own graph plays and sends each sound at the
// door's place; the observers are the swing seam's (melee_fx_swing.cpp). What the opening does to the safe's lock
// and loot is not this lane's.

#include "melee_fx_internal.h"

#include "coop/net/protocol.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstring>

namespace coop::melee_fx::detail {
namespace {

namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

UE_FACT_FN(kRowScrollUp, "melee_fx", L"prop_safeDoor_C", L"scrollUp", Declared);
UE_FACT_FN(kRowScrollDown, "melee_fx", L"prop_safeDoor_C", L"scrollDown", Declared);
UE_FACT_FN(kRowSoundOpen, "melee_fx", L"prop_safeDoor_C", L"OpenAnimationTL__SoundOpen__EventFunc", Declared);
constexpr int kTag = 0x4D465333;   // 'MFS3'

// A notch plays at most four sounds: the tick, the click, the right digit's tick, the outcome.
constexpr int kMaxCaptured = 4;

struct Captured {
    void* sound = nullptr;
    float volume = 1.f;
    float pitch = 1.f;
};

struct Scope {
    bool     open = false;
    void*    door = nullptr;
    int      count = 0;
    Captured sounds[kMaxCaptured];
};
Scope g_scope;

constexpr int kRows = 3;
const ue_wrap::facts::Function* const kWatched[kRows] = {&kRowScrollUp, &kRowScrollDown, &kRowSoundOpen};
bool g_registered = false;
bool g_settled = false;

int  g_turns = 0;
int  g_silent = 0;
bool g_noSoundSaid = false;

// ---- the scope -------------------------------------------------------------------------------------------------

sg::Verdict OnPre(const sg::Call& c) {
    if (!Enabled() || !LiveSession() || !c.object) return sg::Verdict::Run;
    g_scope = Scope{};
    g_scope.open = true;
    g_scope.door = c.object;
    SafeWantsObservers(true);
    return sg::Verdict::Run;
}

void OnPost(const sg::Call& c) {
    if (!g_scope.open || c.object != g_scope.door) return;
    g_scope.open = false;
    SafeWantsObservers(false);
    ++g_turns;
    if (g_scope.count == 0) {
        ++g_silent;
        if (!g_noSoundSaid) {
            g_noSoundSaid = true;
            UE_LOGI("[MELEE-FX] a safe's dial body played no sound through the door's own graph -- nothing to share (said "
                    "once)");
        }
        return;
    }
    FVector at{};
    if (!E::TryGetActorLocation(c.object, at)) return;   // no place to play it at
    for (int i = 0; i < g_scope.count; ++i) {
        Effect e;
        e.kind = coop::net::kMeleeFxSafe;
        e.loc = at;
        e.volume = g_scope.sounds[i].volume;
        e.pitch = g_scope.sounds[i].pitch;
        e.sound = E::SoundName(g_scope.sounds[i].sound);
        Publish(e);
    }
}

}  // namespace

void SafeCapture(void* source, const uint8_t* parms, int32_t sound, int32_t volume, int32_t pitch, int32_t) {
    // Only what the door's own graph plays is the dial's; another object's call is that object's.
    if (!g_scope.open || source != g_scope.door || g_scope.count >= kMaxCaptured) return;
    Captured c;
    std::memcpy(&c.sound, parms + sound, sizeof(c.sound));
    if (!c.sound) return;
    std::memcpy(&c.volume, parms + volume, sizeof(c.volume));
    std::memcpy(&c.pitch, parms + pitch, sizeof(c.pitch));
    g_scope.sounds[g_scope.count++] = c;
}

void InstallSafe() {
    if (g_registered) return;
    g_registered = true;
    for (const ue_wrap::facts::Function* row : kWatched)
        if (!sg::WatchName(*row, kTag, &OnPre, &OnPost)) g_registered = false;
}

void TickSafe() {
    // A body that faulted or was refused runs no post: its scope would hold the observers armed for ever. Between calls
    // no body is running, so an open scope here is stale.
    if (g_scope.open) {
        g_scope = Scope{};
        SafeWantsObservers(false);
    }
    if (!g_registered) InstallSafe();
    if (g_settled || !g_registered) return;
    sg::ResolvePendingNames();
    int live = 0, settled = 0;
    for (const ue_wrap::facts::Function* row : kWatched) {
        live += sg::NameWatchLive(*row, kTag) ? 1 : 0;
        settled += sg::NameWatchSettled(*row, kTag) ? 1 : 0;
    }
    if (settled < kRows) return;   // the safe's class may not be loaded yet; a later tick judges it
    g_settled = true;
    if (live == kRows)
        UE_LOGI("[MELEE-FX] the prop_safeDoor_C dial watches are live (%d of %d)", live, kRows);
    else
        UE_LOGE("[MELEE-FX] only %d of %d prop_safeDoor_C dial watches are live -- a safe's dial is not fully shared", live,
                kRows);
}

void ResetSafe() {
    if (g_turns != 0)
        UE_LOGI("[MELEE-FX] this session: %d safe dial bodies ran, %d with no sound of their own", g_turns, g_silent);
    g_scope = Scope{};
    SafeWantsObservers(false);
    g_turns = g_silent = 0;
    g_noSoundSaid = false;
}

}  // namespace coop::melee_fx::detail
