// coop/player/timeout_hold.cpp -- see coop/player/timeout_hold.h.

#include "coop/player/timeout_hold.h"

#include "coop/player/players_registry.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/character_hold.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::timeout_hold {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;
namespace CH = ue_wrap::character_hold;

// The use action's three bindings (the press that grabs and uses, the second press, the release that
// would reach the deny sound for a press that never ran), the drop key's press and the fire press.
// The ordinals are recook-fragile: the census names a renumbering as a row gone.
UE_FACT_FN(kRowUsePress, "timeout_hold", L"mainPlayer_C", L"InpActEvt_use_K2Node_InputActionEvent_37", Declared);
UE_FACT_FN(kRowUsePress2, "timeout_hold", L"mainPlayer_C", L"InpActEvt_use_K2Node_InputActionEvent_34", Declared);
UE_FACT_FN(kRowUseRelease, "timeout_hold", L"mainPlayer_C", L"InpActEvt_use_K2Node_InputActionEvent_38", Declared);
UE_FACT_FN(kRowDropPress, "timeout_hold", L"mainPlayer_C", L"InpActEvt_drop_K2Node_InputActionEvent_0", Declared);
UE_FACT_FN(kRowFirePress, "timeout_hold", L"mainPlayer_C", L"InpActEvt_fire_K2Node_InputActionEvent_58", Declared);

// The longest order honoured: a corrupted number cannot hold a player for days.
constexpr float kMaxSeconds = 3600.f;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The steady-clock millisecond the hold ends at; 0 when none stands. Written on the game thread.
std::atomic<int64_t> g_deadlineMs{0};
bool g_held = false;            // the pawn was put in MOVE_None by this module (a release is owed)
bool g_guardsInstalled = false;

bool Standing() {
    const int64_t d = g_deadlineMs.load(std::memory_order_acquire);
    return d != 0 && NowMs() < d;
}

// The press seams: while a hold stands the local player's press never reaches the game's handler.
// Registered once, they stay for the process (a cheap load when nothing is held).
bool OnBlockedInput(void* self, void* /*params*/) {
    if (!Standing()) return false;
    return self && self == coop::players::Registry::Get().Local();
}

void InstallGuards() {
    if (g_guardsInstalled) return;
    if (!R::FindClass(P::name::MainPlayerClass)) return;   // retried until mainPlayer_C is loaded
    int installed = 0;
    for (const ue_wrap::facts::Function* row :
         {&kRowUsePress, &kRowUsePress2, &kRowUseRelease, &kRowDropPress, &kRowFirePress}) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(*row);
        if (fn && GT::RegisterInterceptor(fn, &OnBlockedInput)) ++installed;
    }
    g_guardsInstalled = true;   // best effort: a seam that does not resolve leaves that key to the game
    UE_LOGI("timeout_hold: input guards on %d/5 press seams (use x3, drop, fire)", installed);
}

void Clear(const char* why) {
    g_deadlineMs.store(0, std::memory_order_release);
    if (g_held) {
        g_held = false;
        void* local = coop::players::Registry::Get().Local();
        if (local) CH::Release(local);
    }
    UE_LOGI("timeout_hold: hold ended (%s)", why);
}

}  // namespace

bool Apply(float seconds) {
    if (!std::isfinite(seconds)) return false;
    if (seconds <= 0.f) {
        if (g_deadlineMs.load(std::memory_order_acquire) != 0) Clear("released by the host");
        return true;
    }
    if (seconds > kMaxSeconds) seconds = kMaxSeconds;
    g_deadlineMs.store(NowMs() + static_cast<int64_t>(seconds * 1000.f), std::memory_order_release);
    InstallGuards();
    UE_LOGI("timeout_hold: held for %.0f s", seconds);
    return true;
}

bool Active() { return Standing(); }

int RemainingSecs() {
    const int64_t d = g_deadlineMs.load(std::memory_order_acquire);
    const int64_t left = d - NowMs();
    if (d == 0 || left <= 0) return 0;
    return static_cast<int>((left + 999) / 1000);
}

void Tick() {
    if (g_deadlineMs.load(std::memory_order_acquire) == 0) return;
    if (!Standing()) {
        Clear("time served");
        return;
    }
    InstallGuards();
    void* local = coop::players::Registry::Get().Local();
    if (!local) return;
    // Each tick: another system (a wisp's release, a ragdoll's end, a level reload) may have put the
    // mode back. A falling pawn is held when it lands.
    if (CH::HoldStill(local) == CH::Hold::Holding) g_held = true;
}

void OnDisconnect() {
    if (g_deadlineMs.load(std::memory_order_acquire) != 0) Clear("session ended");
}

}  // namespace coop::timeout_hold
