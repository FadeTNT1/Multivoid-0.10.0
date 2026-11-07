// coop/player/blood_drop_fx.cpp -- see coop/player/blood_drop_fx.h.

#include "coop/player/blood_drop_fx.h"

#include "coop/element/player.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/blood_drops.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::blood_drop_fx {
namespace {

namespace N  = coop::net;
namespace BD = ue_wrap::blood_drops;
namespace sg = ue_wrap::script_gate;

// The bleeding effect's drop verb, and the gamemode's drop maker it calls. [RD] effect_bloodloss_c.droptimer:
// a line trace down from the local camera, then masteBloodDecals.addParticle(point, random * 25, size, 0).
UE_FACT_FN(kRowDropTimer, "blood_drop_fx", L"effect_bloodLoss_C", L"dropTimer", Declared);
UE_FACT_FN(kRowAddParticle, "blood_drop_fx", L"bloodDecalMaster_C", L"addParticle", Declared);

constexpr int kTagDropTimer = 1;
constexpr int kTagAddParticle = 2;
constexpr int kQueueCap = 16;              // drops waiting to be sent; past it the oldest go
constexpr float kMaxCoord = 2.0e6f;        // world cm
constexpr float kMaxSpeed = 1.0e4f;        // cm/s; the bleeding drop's is 25
constexpr float kMaxSize = 10.f;           // the bleeding drop's is 0.25..1.5

std::atomic<N::Session*> g_session{nullptr};

bool g_dropWatched = false;
bool g_addWatched = false;
int  g_inDrop = 0;                         // dropTimer bodies running on this thread; game thread only

BD::Drop g_queue[kQueueCap];
int      g_queued = 0;
uint16_t g_seq = 0;

struct Counts {
    uint32_t captured = 0, sent = 0, overflow = 0, heard = 0, dropped = 0, failed = 0, refused = 0;
};
Counts   g_minute;
uint64_t g_minuteStartMs = 0;

sg::Verdict OnDropTimerPre(const sg::Call&) {
    ++g_inDrop;
    return sg::Verdict::Run;
}

void OnDropTimerPost(const sg::Call&) {
    if (g_inDrop > 0) --g_inDrop;
}

// Inside the game's call: a frame read and a copy, nothing else. Only a drop the bleeding effect made is this
// peer's to share; one of ours (a received drop replayed) runs outside any dropTimer.
sg::Verdict OnAddParticlePre(const sg::Call& c) {
    if (g_inDrop <= 0 || c.fromOurCode) return sg::Verdict::Run;
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    BD::Drop d{};
    if (!BD::ReadAddParticle(c, &d)) return sg::Verdict::Run;
    if (g_queued == kQueueCap) {
        std::memmove(&g_queue[0], &g_queue[1], sizeof(g_queue[0]) * (kQueueCap - 1));
        --g_queued;
        ++g_minute.overflow;
    }
    g_queue[g_queued++] = d;
    ++g_minute.captured;
    return sg::Verdict::Run;
}

void TryWatch() {
    if (!g_dropWatched) g_dropWatched = sg::WatchName(kRowDropTimer, kTagDropTimer, &OnDropTimerPre, &OnDropTimerPost);
    if (!g_addWatched) g_addWatched = sg::WatchName(kRowAddParticle, kTagAddParticle, &OnAddParticlePre, nullptr);
}

void SendQueued(N::Session* s) {
    if (g_queued == 0) return;
    const uint32_t eid = static_cast<uint32_t>(coop::players::Registry::Get().LocalPlayerElementId());
    if (eid == 0) {   // no player element yet: nothing can name us
        g_queued = 0;
        return;
    }
    int at = 0;
    while (at < g_queued) {
        N::BloodDropFxPayload p{};
        p.originEid = eid;
        p.seq = ++g_seq;
        int n = 0;
        for (; n < N::kBloodDropFxMax && at < g_queued; ++n, ++at) {
            const BD::Drop& d = g_queue[at];
            float* o = p.drops[n];
            o[0] = d.location.X; o[1] = d.location.Y; o[2] = d.location.Z;
            o[3] = d.velocity.X; o[4] = d.velocity.Y; o[5] = d.velocity.Z;
            o[6] = d.size;
            o[7] = static_cast<float>(d.type);
        }
        p.count = static_cast<uint8_t>(n);
        if (s->SendReliable(N::ReliableKind::BloodDropFx, &p, static_cast<int>(sizeof(p)))) g_minute.sent += n;
    }
    g_queued = 0;
}

int SlotOfEid(uint32_t eid) {
    if (eid == 0) return -1;
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 0; slot < N::kMaxPeers; ++slot) {
        const coop::element::Player* pl = reg.GetPlayerElement(slot);
        if (pl && static_cast<uint32_t>(pl->GetId()) == eid) return slot;
    }
    return -1;
}

void SummaryTick(uint64_t now) {
    if (g_minuteStartMs == 0) g_minuteStartMs = now;
    if (now - g_minuteStartMs < 60000) return;
    g_minuteStartMs = now;
    const Counts& c = g_minute;
    if (c.captured || c.sent || c.heard || c.refused || c.failed)
        UE_LOGI("[BLOOD-DROP] last minute: own drops captured=%u sent=%u overflow=%u; heard=%u dropped here=%u "
                "failed=%u refused=%u",
                c.captured, c.sent, c.overflow, c.heard, c.dropped, c.failed, c.refused);
    g_minute = Counts{};
}

bool Bounded(float v, float lim) { return std::isfinite(v) && v >= -lim && v <= lim; }

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

bool WellFormed(const N::BloodDropFxPayload& p, int len) {
    if (len < static_cast<int>(sizeof(N::BloodDropFxPayload))) return false;
    if (p.originEid == 0 || p.count == 0 || p.count > N::kBloodDropFxMax) return false;
    for (int i = 0; i < p.count; ++i) {
        const float* d = p.drops[i];
        for (int k = 0; k < 3; ++k)
            if (!Bounded(d[k], kMaxCoord)) return false;
        for (int k = 3; k < 6; ++k)
            if (!Bounded(d[k], kMaxSpeed)) return false;
        if (!std::isfinite(d[6]) || d[6] <= 0.f || d[6] > kMaxSize) return false;
        if (d[7] != 0.f && d[7] != 1.f) return false;
    }
    return true;
}

void OnReliable(const N::BloodDropFxPayload& p, int senderSlot) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.originEid == static_cast<uint32_t>(coop::players::Registry::Get().LocalPlayerElementId())) return;
    const int slot = SlotOfEid(p.originEid);
    // The host hears a client firsthand: its message must name its own player. A relayed copy reaches a client
    // from the host, which judged it here.
    if (slot < 0 || (s->role() == N::Role::Host && senderSlot >= 0 && senderSlot != slot)) {
        ++g_minute.refused;
        static int sSaid = 0;
        if (sSaid < 4) {
            ++sSaid;
            UE_LOGW("[BLOOD-DROP] refused drops naming player %u from slot %d (resolved slot %d)", p.originEid,
                    senderSlot, slot);
        }
        return;
    }
    g_minute.heard += p.count;
    static bool sFirst = true;
    for (int i = 0; i < p.count; ++i) {
        const float* d = p.drops[i];
        BD::Drop drop{};
        drop.location = ue_wrap::FVector{d[0], d[1], d[2]};
        drop.velocity = ue_wrap::FVector{d[3], d[4], d[5]};
        drop.size = d[6];
        drop.type = static_cast<int32_t>(d[7]);
        if (BD::AddParticle(drop)) {
            ++g_minute.dropped;
            if (sFirst) {
                sFirst = false;
                UE_LOGI("[BLOOD-DROP] first drop from slot %d dropped here at (%.0f, %.0f, %.0f) size %.2f type %d",
                        slot, static_cast<double>(d[0]), static_cast<double>(d[1]), static_cast<double>(d[2]),
                        static_cast<double>(d[6]), drop.type);
            }
        } else {
            ++g_minute.failed;
        }
    }
}

void Tick() {
    // Every dropTimer body has returned by the time the tick runs; a post that never came (a cancel elsewhere)
    // must not leave the next unrelated addParticle read as a bleeding drop.
    g_inDrop = 0;
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) {
        g_queued = 0;
        return;
    }
    TryWatch();
    SendQueued(s);
    SummaryTick(N::NowMs());
}

void OnDisconnect() {
    g_queued = 0;
    g_inDrop = 0;
    g_minuteStartMs = 0;
    g_minute = Counts{};
}

}  // namespace coop::blood_drop_fx
