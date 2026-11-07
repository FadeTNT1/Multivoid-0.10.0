// coop/world/ufo_event_sync.cpp -- see coop/world/ufo_event_sync.h.

#include "coop/world/ufo_event_sync.h"

#include "coop/world/ufo_ball_sync.h"

#include "ue_wrap/actors/ufo_events.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_timeline.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace coop::ufo_event_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace UE = ue_wrap::ufo_events;
namespace TL = ue_wrap::engine_timeline;

constexpr float kMaxTimelineSeconds = 3600.f;   // far past any of the timelines
constexpr long long kSeekGiveUpMs = 8000;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// A seek that waits for its timeline to be running.
struct PendingSeek {
    void*     actor;
    int32_t   idx;
    uint8_t   phase;
    float     position;
    long long sinceMs;
    unsigned  eid;
};
std::vector<PendingSeek> g_pending;

}  // namespace

bool IsBirthActor(void* actor) {
    return coop::ufo_ball_sync::IsBallActor(actor) || UE::IsBoofo(actor) || UE::IsJoel(actor) || UE::IsPillfo(actor);
}

void FillBirth(void* actor, unsigned int eid, uint8_t* blob, size_t cap, uint8_t& len) {
    len = 0;
    if (coop::ufo_ball_sync::IsBallActor(actor)) {
        coop::ufo_ball_sync::FillBirth(actor, eid, blob, cap, len);
        return;
    }
    if (UE::IsBoofo(actor)) {
        UE::BoofoSetup s;
        if (!UE::BoofoRead(actor, s) || cap < sizeof(float) + 1u + s.count) {
            UE_LOGW("ufo_event[host]: boofo eid=%u -- course UNREADABLE, sending no birth content. The mirror will fly its "
                    "own random course, not the host's", eid);
            return;
        }
        std::memcpy(blob, &s.alpha, sizeof(float));
        blob[sizeof(float)] = s.count;
        std::memcpy(blob + sizeof(float) + 1, s.angle, s.count);
        len = static_cast<uint8_t>(sizeof(float) + 1u + s.count);
        UE_LOGI("ufo_event[host]: boofo eid=%u alpha=%.3f points=%u", eid, s.alpha, static_cast<unsigned>(s.count));
        return;
    }
    if (UE::IsJoel(actor) || UE::IsPillfo(actor)) {
        UE::TimelineProgress p;
        if (!UE::TimelineRead(actor, p) || cap < 1 + sizeof(float)) {
            UE_LOGW("ufo_event[host]: %s eid=%u -- timeline UNREADABLE, sending no birth content. A joiner's mirror will "
                    "start its flight from the beginning", UE::IsJoel(actor) ? "joel" : "pillfo", eid);
            return;
        }
        blob[0] = p.phase;
        std::memcpy(blob + 1, &p.position, sizeof(float));
        len = static_cast<uint8_t>(1 + sizeof(float));
        UE_LOGI("ufo_event[host]: %s eid=%u phase=%u position=%.2f", UE::IsJoel(actor) ? "joel" : "pillfo", eid,
                static_cast<unsigned>(p.phase), p.position);
    }
}

void SeedMirror(void* actor, unsigned int eid, const uint8_t* blob, size_t len) {
    if (coop::ufo_ball_sync::IsBallActor(actor)) {
        coop::ufo_ball_sync::SeedMirror(actor, eid, blob, len);
        return;
    }
    const bool boofo = UE::IsBoofo(actor);
    const bool joel = UE::IsJoel(actor);
    const bool pill = UE::IsPillfo(actor);
    if (!boofo && !joel && !pill) return;
    const char* name = boofo ? "boofo" : (joel ? "joel" : "pillfo");
    if (len == 0 || !blob) {
        UE_LOGW("ufo_event[client]: %s eid=%u -- no birth content; the mirror flies its own course", name, eid);
        return;
    }
    if (boofo) {
        UE::BoofoSetup s;
        if (len < sizeof(float) + 1u) {
            UE_LOGW("ufo_event[client]: boofo eid=%u birthLen=%zu too short -- dropping the setup", eid, len);
            return;
        }
        std::memcpy(&s.alpha, blob, sizeof(float));
        s.count = blob[sizeof(float)];
        if (!std::isfinite(s.alpha) || s.alpha < 0.f || s.alpha > 1.f || s.count == 0 ||
            s.count > UE::kMaxBoofoPoints || len != sizeof(float) + 1u + s.count) {
            UE_LOGW("ufo_event[client]: boofo eid=%u unusable setup (alpha=%f points=%u birthLen=%zu) -- dropping it", eid,
                    s.alpha, static_cast<unsigned>(s.count), len);
            return;
        }
        std::memcpy(s.angle, blob + sizeof(float) + 1, s.count);
        if (!UE::BoofoApply(actor, s)) {
            UE_LOGW("ufo_event[client]: boofo eid=%u -- the course did not apply (a member, the point count or a spline "
                    "verb); the mirror flies its own course", eid);
            return;
        }
        // The mirror's tick is parked by the lane and its begin-play enables it five seconds in, as the host's; a flight
        // that is already under way on the host has its tick on, so the joiner's does not wait.
        if (s.alpha > 0.f) ue_wrap::engine::SetActorTickEnabled(actor, true);
        UE_LOGI("ufo_event[client]: boofo eid=%u seeded alpha=%.3f points=%u", eid, s.alpha, static_cast<unsigned>(s.count));
        return;
    }
    if (len != 1 + sizeof(float)) {
        UE_LOGW("ufo_event[client]: %s eid=%u birthLen=%zu, expected %zu -- dropping the setup", name, eid, len,
                1 + sizeof(float));
        return;
    }
    const uint8_t phase = blob[0];
    float position = 0.f;
    std::memcpy(&position, blob + 1, sizeof(float));
    if (!std::isfinite(position) || position < 0.f || position > kMaxTimelineSeconds ||
        (phase != UE::kNoPhase && phase > (pill ? 1 : 0))) {
        UE_LOGW("ufo_event[client]: %s eid=%u unusable progress (phase=%u position=%f) -- dropping it", name, eid,
                static_cast<unsigned>(phase), position);
        return;
    }
    // A flight that has not started on the host (or just started) is the mirror's own start.
    if (phase == UE::kNoPhase || (phase == 0 && position < 0.05f)) return;
    if (pill) {
        // The pillfo's `a` was started by its begin-play inside the finish; seek it now.
        const bool ok = UE::TimelineSeek(actor, phase, position);
        UE_LOGI("ufo_event[client]: pillfo eid=%u seeked timeline %u to %.2fs%s", eid, static_cast<unsigned>(phase),
                position, ok ? "" : " -- the seek did not dispatch");
        return;
    }
    // The joel's timeline starts a second after its begin-play: seek it when it runs.
    g_pending.push_back(PendingSeek{actor, R::InternalIndexOf(actor), phase, position, NowMs(), eid});
    UE_LOGI("ufo_event[client]: joel eid=%u will seek its timeline to %.2fs once it runs", eid, position);
}

void TickClient() {
    if (g_pending.empty()) return;
    const long long now = NowMs();
    for (size_t i = 0; i < g_pending.size();) {
        PendingSeek& p = g_pending[i];
        bool done = !R::IsLiveByIndex(p.actor, p.idx) || now - p.sinceMs > kSeekGiveUpMs;
        if (!done) {
            void* tl = UE::TimelineOf(p.actor, p.phase);
            TL::State st;
            if (tl && TL::Read(tl, st) && st.playing) {
                const bool ok = UE::TimelineSeek(p.actor, p.phase, p.position);
                UE_LOGI("ufo_event[client]: joel eid=%u seeked its timeline to %.2fs%s", p.eid, p.position,
                        ok ? "" : " -- the seek did not dispatch");
                done = true;
            }
        }
        if (done) {
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void OnDisconnect() { g_pending.clear(); }

}  // namespace coop::ufo_event_sync
