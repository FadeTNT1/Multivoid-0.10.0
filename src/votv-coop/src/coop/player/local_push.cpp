// coop/player/local_push.cpp -- see coop/player/local_push.h.

#include "coop/player/local_push.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/character_push.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::local_push {

namespace {

namespace CP = ue_wrap::character_push;
namespace R  = ue_wrap::reflection;

constexpr uint64_t kCheckMs = 1000;    // how often a live pawn is looked at again
constexpr unsigned kMaxSaid = 8;       // writes and restores logged without the dev flag

void*   g_pawn = nullptr;              // the pawn generation the state below belongs to
int32_t g_pawnIdx = -1;
bool    g_haveOrig = false;            // `g_orig` holds the game's own values for g_pawn
CP::PushValues g_orig{};
bool    g_written = false;             // this module changed g_pawn's movement: a restore is owed
uint64_t g_nextCheckMs = 0;
unsigned g_said = 0;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool Verbose() {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::local_prop_push_log);
    return on;
}

bool Close(float a, float b) {
    return std::fabs(a - b) <= 1e-4f + 1e-4f * std::fabs(b);
}

// The values the configured fractions leave of the game's own.
CP::PushValues Desired(const CP::PushValues& orig) {
    const float pushScale  = coop::config::ResolveFloat(coop::config_registry::rows::local_push_scale);
    const float standScale = coop::config::ResolveFloat(coop::config_registry::rows::local_stand_force_scale);
    CP::PushValues d = orig;
    d.push        = Scaled(orig.push, pushScale);
    d.initialPush = Scaled(orig.initialPush, pushScale);
    d.touch       = Scaled(orig.touch, pushScale);
    d.standing    = Scaled(orig.standing, standScale);
    return d;
}

bool Differs(const CP::PushValues& a, const CP::PushValues& b) {
    return (a.havePush && !Close(a.push, b.push)) || (a.haveInitialPush && !Close(a.initialPush, b.initialPush)) ||
           (a.haveTouch && !Close(a.touch, b.touch)) || (a.haveStanding && !Close(a.standing, b.standing));
}

void LogValues(const char* what, const CP::PushValues& v) {
    UE_LOGI("[LOCAL-PUSH] %s: physicsInteraction=%s push=%s%.1f initialPush=%s%.1f touch=%s%.2f standingDown=%s%.2f",
            what, !v.haveInteraction ? "n/a" : v.interaction ? "on" : "off",
            v.havePush ? "" : "n/a ", static_cast<double>(v.push), v.haveInitialPush ? "" : "n/a ",
            static_cast<double>(v.initialPush), v.haveTouch ? "" : "n/a ", static_cast<double>(v.touch),
            v.haveStanding ? "" : "n/a ", static_cast<double>(v.standing));
}

}  // namespace

void Tick() {
    UE_ASSERT_GAME_THREAD("local_push::Tick");
    void* const local = coop::players::Registry::Get().Local();
    if (!local) {
        g_pawn = nullptr;   // a level change or the menu: the next pawn is a fresh generation, and this one is gone
        g_haveOrig = false;
        g_written = false;
        return;
    }
    if (local != g_pawn || !R::IsLiveByIndex(g_pawn, g_pawnIdx)) {
        g_pawn = local;
        g_pawnIdx = R::InternalIndexOf(local);
        g_haveOrig = false;   // a new pawn starts from the game's own values
        g_written = false;
        g_nextCheckMs = 0;
    }
    const uint64_t now = NowMs();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckMs;

    if (!g_haveOrig) {
        CP::PushValues cur;
        if (!CP::Read(g_pawn, cur)) return;   // the movement component is not up yet: asked again next check
        g_orig = cur;
        g_haveOrig = true;
        LogValues("pawn generation, game values", g_orig);
    }
    const CP::PushValues want = Desired(g_orig);
    CP::PushValues now_v;
    if (!CP::Read(g_pawn, now_v)) return;
    if (!Differs(want, now_v)) {
        if (Verbose()) LogValues("already at target", now_v);
        return;
    }
    if (!CP::WriteForces(g_pawn, want)) return;
    g_written = true;
    if (Verbose() || g_said < kMaxSaid) {
        ++g_said;
        LogValues("applied for the session", want);
    }
}

void OnDisconnect() {
    if (g_written && g_pawn && R::IsLiveByIndex(g_pawn, g_pawnIdx) && g_haveOrig) {
        if (CP::WriteForces(g_pawn, g_orig)) LogValues("restored, session ended", g_orig);
    }
    g_pawn = nullptr;
    g_pawnIdx = -1;
    g_haveOrig = false;
    g_written = false;
    g_nextCheckMs = 0;
    g_said = 0;
}

}  // namespace coop::local_push
