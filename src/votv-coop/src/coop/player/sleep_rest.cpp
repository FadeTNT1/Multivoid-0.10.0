// coop/player/sleep_rest.cpp -- see coop/player/sleep_rest.h.

#include "coop/player/sleep_rest.h"

#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/world/event_count_upkeep.h"

#include "ue_wrap/actors/effects.h"
#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/active_events.h"
#include "ue_wrap/world/sanity_ticker.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace coop::sleep_rest {
namespace {

namespace V  = ue_wrap::vitals;
namespace SLP = ue_wrap::sleep;
namespace FX = ue_wrap::effects;
namespace EN = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace AE = ue_wrap::active_events;
namespace R  = ue_wrap::reflection;

// The factor the game's own sleep sets (mainGamemode.cpp:5428, SetGlobalTimeDilation(20)). The row below is
// the census's entry for the `sleep` body that holds that literal (include/ue_wrap/facts/
// fingerprints_0.9.0n.inc), so a game update that changes the body is named at boot; the watch compares
// the factor it finds in the same call.
constexpr float kNativeSleepDilation = 20.0f;
constexpr int kTagSleepRest = 0x534C5253;  // 'SLRS'
UE_FACT_FN(kRowSleep, "sleep_rest", L"mainGamemode_C", L"sleep", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_watchRegistered = false;
bool g_watchSettled = false;
bool g_saidWatchFail = false;
uint64_t g_nextWatchMs = 0;  // the watch is asked about at most once a second
bool g_saidFactor = false;  // a factor other than the pinned one is said once, whatever the cause

// The rest in progress. The gamemode and the body are held by identity: a world change or a new body ends it.
bool g_resting = false;
ue_wrap::CachedObjRef g_gm;
ue_wrap::CachedObjRef g_body;
// Each effect actor the rest dilated, so the end puts back those and not the list as it stands then:
// effect_poo leaves the list with its actor alive. The game keeps one effect per kind.
constexpr int kMaxDilatedEffects = 16;
ue_wrap::CachedObjRef g_dilated[kMaxDilatedEffects];
int  g_dilatedCount = 0;
bool g_saidDilatedFull = false;
// Food and coffee as the last tick read them (after our own write). Unset on the first tick of a rest, which
// takes the baselines and corrects nothing.
bool  g_haveFood = false;
bool  g_haveCoffee = false;
float g_foodBase = 0.f;
float g_coffeeBase = 0.f;

bool ConnectedClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Client;
}

// The local player's body: the registry's (warm while the controller holds the sleeping pawn), else the
// pawn `sleep` took from the controller.
void* CurrentBody() {
    if (void* b = coop::players::Registry::Get().Local()) return b;
    return SLP::OrigPawn();
}

// An effect that ended mid-rest (lsd and foodPoison destroy themselves) leaves a dead entry; its slot is freed.
void ReclaimDeadEffects() {
    for (int i = 0; i < g_dilatedCount;) {
        if (g_dilated[i].Alive()) { ++i; continue; }
        g_dilated[i] = g_dilated[--g_dilatedCount];
        g_dilated[g_dilatedCount].Reset();
    }
}

// Dilate one effect and remember it. One the table cannot hold, even after the dead entries are reclaimed, is
// left alone and said once: an actor not remembered could not be put back.
void DilateEffect(void* effect, void* ctx) {
    bool known = false;
    for (int i = 0; i < g_dilatedCount && !known; ++i) known = g_dilated[i].Is(effect);
    if (!known) {
        if (g_dilatedCount >= kMaxDilatedEffects) ReclaimDeadEffects();
        if (g_dilatedCount >= kMaxDilatedEffects) {
            if (!g_saidDilatedFull) {
                g_saidDilatedFull = true;
                UE_LOGW("sleep_rest: more than %d effect actors in the world -- the extra ones stay at factor 1",
                        kMaxDilatedEffects);
            }
            return;
        }
        g_dilated[g_dilatedCount++].Set(effect);
    }
    EN::SetCustomTimeDilation(effect, *static_cast<const float*>(ctx));
}

void BeginRest() {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    void* const body = CurrentBody();
    if (!gm || !body) {
        // The degrade: no body to dilate, so the world stays at 1 and the sleep runs at 1/20 of the game's speed.
        UE_LOGW("sleep_rest: a rest began with no local body (%p) or gamemode (%p) -- it restores at the world's rate",
                body, gm);
        return;
    }
    if (!EN::SetCustomTimeDilation(body, kNativeSleepDilation)) return;
    g_gm.Set(gm);
    g_body.Set(body);
    g_resting = true;
    g_haveFood = g_haveCoffee = false;
    UE_LOGI("sleep_rest: rest begun -- the world stays at 1, the body and the world's effects run at %.0f",
            kNativeSleepDilation);
}

void EndRest(const char* why) {
    if (void* b = g_body.Get()) EN::SetCustomTimeDilation(b, 1.f);
    for (int i = 0; i < g_dilatedCount; ++i) {
        if (void* e = g_dilated[i].Get()) EN::SetCustomTimeDilation(e, 1.f);
        g_dilated[i].Reset();
    }
    g_dilatedCount = 0;
    g_resting = false;
    g_gm.Reset();
    g_body.Reset();
    g_haveFood = g_haveCoffee = false;
    g_saidDilatedFull = false;  // said once per rest
    UE_LOGI("sleep_rest: rest ended (%s) -- the body and the effects are back at 1", why);
}

// The game refused this client's sleep. Its refusals are lib_C::getEvent ("I do not want to sleep... i can sense
// something..."): `activeEvents > 0` or the camera outside the base box; hunger; a fall; the stress (sanity <= 0.5);
// elevation. Say which of the ones a client can see, once per refusal. The count is the game's refcount of
// setEvent(true) callers kept in activeEvents_senders; a sender destroyed or retired without its setEvent(false)
// (a mirror, a per-viewer figure) leaves the count above the senders still alive, and nothing ever lowers it
// again, so every later sleep is refused for an event that no longer exists. When no live sender backs part of the
// count, that part is taken back and said; the player's next try goes through. A count backed by live senders is
// the game's own and is left alone.
void OnSleepRefused() {
    int32_t count = 0;
    AE::Senders arr{};
    const bool haveCount = AE::EnsureResolved() && AE::ReadCount(count) && AE::ReadSenders(arr);
    std::string classes;
    int32_t live = 0;
    if (haveCount && arr.num >= 0 && arr.num <= 4096) {
        for (int32_t i = 0; i < arr.num; ++i) {
            void* obj = arr.data ? arr.data[i] : nullptr;
            if (!obj || !R::IsLive(obj)) continue;
            ++live;
            if (classes.size() < 200) {
                for (wchar_t c : R::ClassNameOf(obj)) classes.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
                classes += ' ';
            }
        }
    }
    ue_wrap::sanity_ticker::State st;
    const bool haveSanity = ue_wrap::sanity_ticker::Read(st);
    UE_LOGI("sleep_rest: the game refused this client's sleep -- activeEvents=%d, %d live sender(s) [%s], sanity=%.3f "
            "(refused at <= 0.5), %s", haveCount ? count : -1, live, classes.empty() ? "none" : classes.c_str(),
            haveSanity ? st.sanity : -1.f,
            !haveCount ? "the event registry does not resolve"
            : count > 0 ? "the 'can sense something' check holds on the count"
                        : "no event count; the other refusals (hunger, a fall, stress, elevation, camera outside the box) stand");
    if (haveCount) coop::event_count_upkeep::ReclaimDead("sleep refused");
}

// The call has run: the game set the world's dilation and the sleeping flag inside it. A refusal leaves the
// flag down and nothing to undo. Our own reflected calls come through here as the game's do. The game has no
// isSleep guard on `sleep` (mainGamemode.cpp:5495-5583), so a second call while resting sets the world's
// factor again: it is put back to 1 here as well, and begins no new rest. A second call the game refused
// leaves the world at the 1 the rest holds, which is legal and returns before the factor check.
void OnSleepPost(const sg::Call&) {
    if (!ConnectedClient()) return;
    float asleep = 0.f;
    if (!V::Read(V::Field::Sleeping, &asleep)) return;
    if (asleep == 0.f) {
        if (!g_resting) OnSleepRefused();
        return;
    }
    if (!SLP::EnsureResolved()) {
        UE_LOGE("sleep_rest: a sleep began and the dilation calls do not resolve -- the world stays at the game's factor");
        return;
    }
    const float found = SLP::GetGlobalTimeDilation();
    // Inside a rest the world already runs at 1: a second call the game refused set nothing, and there is
    // nothing to compare or undo.
    if (g_resting && std::fabs(found - 1.f) <= 0.01f) return;
    if (std::fabs(found - kNativeSleepDilation) > 0.01f && !g_saidFactor) {
        g_saidFactor = true;
        UE_LOGW("sleep_rest: the game's sleep set a world dilation of %.3f, not the pinned %.1f -- a game update "
                "changed the speed-up; the body and the effects are given the pinned factor all the same",
                found, kNativeSleepDilation);
    }
    if (!SLP::SetGlobalTimeDilation(1.f)) {
        UE_LOGE("sleep_rest: the world's dilation could not be set back to 1 after the game's sleep");
        return;
    }
    if (!g_resting) BeginRest();
}

// Register the watch, then ask the gate to resolve its name until it is live or settled dead. At most once a
// second: a registration the gate refuses is said once, and asked again at that pace.
void EnsureWatch() {
    if (g_watchSettled) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextWatchMs) return;
    g_nextWatchMs = now + 1000;
    if (!g_watchRegistered) g_watchRegistered = sg::WatchName(kRowSleep, kTagSleepRest, nullptr, &OnSleepPost);
    if (!g_watchRegistered) {
        if (!g_saidWatchFail) {
            g_saidWatchFail = true;
            UE_LOGW("sleep_rest: the gate did not take the sleep watch -- asked again each second");
        }
        return;
    }
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(kRowSleep, kTagSleepRest)) {
        g_watchSettled = true;
        UE_LOGI("sleep_rest: a client's sleep is seen at the call (a script-gate watch on mainGamemode_C::sleep)");
    } else if (sg::NameWatchSettled(kRowSleep, kTagSleepRest)) {
        g_watchSettled = true;
        UE_LOGE("sleep_rest: the sleep watch is dead -- a client's sleep runs the world at the game's factor");
    }
}

// A fall since the last read: food gets (1 - 1/F) of it back, coffee loses (F - 1) times it more. A rise
// is not touched, and the baseline is read again after the write.
void SettleNeeds() {
    constexpr float kF = kNativeSleepDilation;
    float food = 0.f, coffee = 0.f;
    if (V::Read(V::Field::Food, &food)) {
        const float fall = g_haveFood ? g_foodBase - food : 0.f;
        if (fall > 0.f && V::Write(V::Field::Food, food + fall * (1.f - 1.f / kF))) V::Read(V::Field::Food, &food);
        g_foodBase = food;
        g_haveFood = true;
    }
    if (V::Read(V::Field::CoffeePower, &coffee)) {
        const float fall = g_haveCoffee ? g_coffeeBase - coffee : 0.f;
        if (fall > 0.f && V::Write(V::Field::CoffeePower, std::max(coffee - fall * (kF - 1.f), 0.f)))
            V::Read(V::Field::CoffeePower, &coffee);
        g_coffeeBase = coffee;
        g_haveCoffee = true;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (s->role() == coop::net::Role::Client) EnsureWatch();
    if (!g_resting) return;
    if (!ConnectedClient()) return EndRest("the session left");
    void* const gm = ue_wrap::world_singleton::Gamemode();
    if (!gm || gm != g_gm.Raw() || !g_gm.Alive()) return EndRest("the gamemode changed");
    void* const body = CurrentBody();
    if (!body || body != g_body.Raw() || !g_body.Alive()) return EndRest("the body changed");
    // The wake itself happens in the game; the body and the effects run at the native factor until this tick
    // sees the flag fall, one frame at most.
    float asleep = 0.f;
    if (!V::Read(V::Field::Sleeping, &asleep) || asleep == 0.f) return EndRest("the sleeping flag fell");
    float factor = kNativeSleepDilation;
    FX::ForEachWorldEffect(&DilateEffect, &factor);
    SettleNeeds();
}

void OnDisconnect() {
    if (g_resting) EndRest("the session ended");
}

}  // namespace coop::sleep_rest
