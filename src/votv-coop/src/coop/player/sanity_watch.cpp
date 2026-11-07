// coop/player/sanity_watch.cpp -- see coop/player/sanity_watch.h. Read-only instrument.

#include "coop/player/sanity_watch.h"

#include "coop/net/session.h"
#include "coop/world/time_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/daynightcycle.h"
#include "ue_wrap/world/sanity_ticker.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace coop::sanity_watch {
namespace {

namespace ST  = ue_wrap::sanity_ticker;
namespace DNC = ue_wrap::daynightcycle;
using Clock = std::chrono::steady_clock;

constexpr auto kLineEvery         = std::chrono::seconds(10);
constexpr auto kCensusWhileDrain  = std::chrono::seconds(30);
constexpr auto kCensusOtherwise   = std::chrono::seconds(120);
constexpr size_t kCensusKeep      = 8;

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_nextLine{};
Clock::time_point g_nextCensus{};
void* g_ticker = nullptr;          // the ticker last read: a new one is a new world
int32_t g_lastTier = -2;           // -2: none read in this world yet
void* g_saidHold = nullptr;        // the playerHold a mismatch was last said for
bool g_saidUnread = false;
// The world's running figures, for the last line.
uint32_t g_lines = 0, g_drainLines = 0, g_tierChanges = 0;
float g_minSanity = 1.f;

const char* RoleName(const coop::net::Session* s) {
    return s->role() == coop::net::Role::Host ? "host" : "client";
}

const char* Trend(const ST::State& st) {
    if (!st.enabled()) return "off";
    if (st.sleeping) return "paused (asleep)";
    if (st.drainLerp > 0.f) return "recovering";
    if (st.drainLerp < 0.f) return "draining";
    return "steady";
}

// The clock as the clocks_C prop shows it: frac(day / maxTime) of a day, in minutes.
void ClockText(char* buf, size_t n) {
    float total = 0.f, day = 0.f, scale = 0.f, maxT = 0.f;
    int32_t h = 0, m = 0, dz = -1;
    DNC::Rates rates{};
    const bool clock = DNC::ReadClock(total, day, scale) && DNC::ReadMaxTime(maxT) && maxT > 0.f;
    DNC::ReadSavedTime(h, m, dz);
    const bool haveRates = DNC::ReadRates(rates);
    if (!clock) {
        std::snprintf(buf, n, "clock unread");
        return;
    }
    double frac = std::fmod(static_cast<double>(day) / maxT + 1.0, 1.0);
    const int minutes = static_cast<int>(frac * 1440.0);
    char host[48] = "";
    if (const int32_t hz = coop::time_sync::LastHostDayZ(); hz >= 0) std::snprintf(host, sizeof(host), " host day number %d", hz);
    std::snprintf(buf, n, "clock %02d:%02d (day=%.2f / maxTime=%.2f, scale %.2f) day number %d%s realtime=%s",
                  minutes / 60, minutes % 60, day, maxT, scale, dz, host,
                  haveRates ? (rates.realtime ? "yes" : "no") : "?");
}

void SayCensus(const char* role, const ST::State& st) {
    ue_wrap::FVector at{};
    if (!ST::LocationOf(st.playerHold, at)) return;
    ST::Census c;
    if (!ST::CensusNear(at, kCensusKeep, c)) {
        UE_LOGI("sanity_watch[%s]: lights near the hand: no light class loaded", role);
        return;
    }
    std::string list;
    for (const ST::Light& l : c.nearest) {
        char row[256];
        std::snprintf(row, sizeof(row), "%s%ls/%ls %.0fcm i=%.2f r=%.0f %s", list.empty() ? "" : "; ", l.owner.c_str(),
                      l.kind.c_str(), l.distance, l.intensity, l.radius, l.visible ? "shown" : "HIDDEN");
        list += row;
    }
    UE_LOGI("sanity_watch[%s]: lights near the hand (%.0f, %.0f, %.0f): %d walked, %d in reach, %d lit -- nearest: %s",
            role, at.X, at.Y, at.Z, c.scanned, c.inReach, c.litInReach, list.empty() ? "none" : list.c_str());
}

void ResetWorld() {
    g_ticker = nullptr;
    g_lastTier = -2;
    g_saidHold = nullptr;
    g_saidUnread = false;
    g_lines = g_drainLines = g_tierChanges = 0;
    g_minSanity = 1.f;
    g_nextLine = g_nextCensus = Clock::time_point{};
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const auto now = Clock::now();
    const char* role = RoleName(s);

    // The tier is checked every tick (a few field reads), the full line every 10 s.
    ST::State st;
    if (!ST::Read(st)) {
        if (now >= g_nextLine && !g_saidUnread) {
            g_saidUnread = true;
            UE_LOGI("sanity_watch[%s]: the sanity ticker does not read yet (no world, or its rows unresolved)", role);
        }
        return;
    }
    g_saidUnread = false;
    if (st.ticker != g_ticker) {
        const bool first = g_ticker == nullptr;
        ResetWorld();
        g_ticker = st.ticker;
        UE_LOGI("sanity_watch[%s]: %s sanity ticker %p -- sanity starts at %.3f (the game saves none: a world load "
                "starts it at the class default), floor %.3f (original %.3f)", role,
                first ? "the world's" : "a new world's", st.ticker, st.sanity, st.floor, st.floorOriginal);
    }
    if (st.sanity < g_minSanity) g_minSanity = st.sanity;
    if (st.eventTier != g_lastTier) {
        if (g_lastTier != -2) {
            ++g_tierChanges;
            UE_LOGI("sanity_watch[%s]: tier %d -> %d (sanity %.3f, light %.3f, floor %.3f, %s)", role, g_lastTier,
                    st.eventTier, st.sanity, st.lightLevel, st.floor, Trend(st));
        }
        g_lastTier = st.eventTier;
    }
    if (st.playerHold != st.gamemodePlayer && st.playerHold != g_saidHold) {
        g_saidHold = st.playerHold;
        UE_LOGW("sanity_watch[%s]: the ticker samples %p, not this machine's player %p -- its light is read at "
                "another body", role, st.playerHold, st.gamemodePlayer);
    }
    if (now < g_nextLine) return;
    g_nextLine = now + kLineEvery;
    ++g_lines;
    const bool draining = st.enabled() && !st.sleeping && st.drainLerp < 0.f;
    if (draining) ++g_drainLines;

    char clock[200];
    ClockText(clock, sizeof(clock));
    ue_wrap::FVector at{};
    const bool haveAt = ST::LocationOf(st.playerHold, at);
    UE_LOGI("sanity_watch[%s]: sanity=%.3f tier=%d light=%.3f (hand %.3f+%.3f) lerp=%.3f floor=%.3f/%.3f dayMult=%.2f "
            "drain=%.3f lerp=%.3f -> %s | rule=%d manual=%d save=%s shadows=%d flashlight=%d | %s | at (%.0f, %.0f, %.0f)%s",
            role, st.sanity, st.eventTier, st.lightLevel, st.playerLight1, st.playerLight2, st.lightLevelLerp, st.floor,
            st.floorOriginal, st.dayMult, st.drain, st.drainLerp, Trend(st), st.ruleEnable ? 1 : 0,
            st.drainManual ? 1 : 0, st.saveEnableRead ? (st.saveEnable ? "1" : "0") : "?", st.shadowCast ? 1 : 0,
            st.flashlight ? 1 : 0, clock, at.X, at.Y, at.Z, haveAt ? "" : " (unread)");

    if (now >= g_nextCensus) {
        g_nextCensus = now + (draining ? kCensusWhileDrain : kCensusOtherwise);
        SayCensus(role, st);
    }
}

void OnDisconnect() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (g_ticker && s)
        UE_LOGI("sanity_watch[%s]: session over -- %u line(s), %u draining, %u tier change(s), lowest sanity %.3f",
                RoleName(s), g_lines, g_drainLines, g_tierChanges, g_minSanity);
    ResetWorld();
}

}  // namespace coop::sanity_watch
