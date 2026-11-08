// coop/player/tipsy.cpp -- see coop/player/tipsy.h.

#include "coop/player/tipsy.h"

#include "coop/net/acting_slot.h"
#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <string>

namespace coop::tipsy {
namespace {

namespace R = ue_wrap::reflection;
namespace N = ue_wrap::profile::name;
namespace sg = ue_wrap::script_gate;

// prop_snack_C declares snackEaten and prop_snack_milk_C overrides it, so the row watches every class's body of the name
// and the callback picks the drinks by class.
UE_FACT_FN(kRowSnackEaten, "tipsy", nullptr, L"snackEaten", AnyClass);
constexpr int kTag = 0x54505359;   // 'TPSY'

constexpr uint64_t kPerDrinkMs = 5ull * 60 * 1000;
constexpr uint64_t kMaxMs = 15ull * 60 * 1000;

std::atomic<uint64_t> g_untilMs{0};   // steady-clock ms; the player is tipsy while it is ahead of now
bool g_registered = false;
bool g_live = false;
bool g_dead = false;
uint64_t g_nextLiveCheckMs = 0;
bool g_saidForeign = false;

void OnSnackEaten(const sg::Call& c) {
    const std::wstring cls = R::ClassNameOf(c.object);
    if (cls != N::SnackBeerClass && cls != N::SnackWineClass) return;
    const uint8_t acting = coop::acting_slot::Current();
    if (acting != coop::acting_slot::kNone && acting != coop::players::Registry::Get().LocalPeerId()) {
        if (!g_saidForeign) {
            g_saidForeign = true;
            UE_LOGI("tipsy: a %ls was drunk here for slot %u (the host ran its verb) -- not this player's, no credit",
                    cls.c_str(), static_cast<unsigned>(acting));
        }
        return;
    }
    const uint64_t now = coop::net::NowMs();
    const uint64_t from = std::max(g_untilMs.load(std::memory_order_relaxed), now);
    const uint64_t until = std::min(from + kPerDrinkMs, now + kMaxMs);
    g_untilMs.store(until, std::memory_order_relaxed);
    UE_LOGI("tipsy: drank a %ls -- tipsy for %llu s more", cls.c_str(), static_cast<unsigned long long>((until - now) / 1000));
}

}  // namespace

void Tick() {
    if (g_dead || !sg::IsInstalled()) return;
    if (!g_registered) {
        g_registered = sg::WatchName(kRowSnackEaten, kTag, nullptr, &OnSnackEaten);
        if (!g_registered) {
            g_dead = true;
            UE_LOGE("tipsy: the script-body gate refused the snackEaten watch -- nobody gets tipsy this run");
        }
        return;
    }
    if (g_live) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextLiveCheckMs) return;
    g_nextLiveCheckMs = now + 1000;
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRowSnackEaten, kTag)) {
        g_live = true;
        UE_LOGI("tipsy: the snackEaten watch is live -- a beer or a wine that goes down makes the player tipsy");
    } else if (sg::PendingNameCount() == 0) {
        g_dead = true;
        UE_LOGE("tipsy: the snackEaten watch is dead -- nobody gets tipsy this run");
    }
}

bool LocalTipsy() { return g_untilMs.load(std::memory_order_relaxed) > coop::net::NowMs(); }

void OnDisconnect() { g_untilMs.store(0, std::memory_order_relaxed); }

}  // namespace coop::tipsy
