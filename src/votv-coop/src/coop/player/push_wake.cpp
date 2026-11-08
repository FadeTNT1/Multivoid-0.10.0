// coop/player/push_wake.cpp -- see coop/player/push_wake.h. The wire ingest, the helpers the three files share, and a
// client's side of an event: the animation, the sleeper's wake on the host's word, and the host's refusal.

#include "coop/player/push_wake.h"

#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/push_wake_animation.h"
#include "coop/player/push_wake_internal.h"
#include "coop/player/remote_player.h"
#include "coop/player/sleep_sync.h"
#include "coop/player/sleepers.h"
#include "coop/text/utf8_codec.h"
#include "l10n/l10n.h"

#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/world/votv_lib.h"

#include <atomic>
#include <cstring>

namespace coop::push_wake {
namespace detail {
namespace {

namespace E = ue_wrap::engine;
namespace SLP = ue_wrap::sleep;
using coop::net::SleepPeerState;

std::atomic<coop::net::Session*> g_session{nullptr};

bool IsLocalSlot(int slot) {
    return slot >= 0 && slot == static_cast<int>(coop::players::Registry::Get().LocalPeerId());
}

// The local body's ragdoll and dead flags; false while they do not read.
bool LocalRagdollDead(bool& ragdoll, bool& dead) {
    void* const local = coop::players::Registry::Get().Local();
    return local && E::ReadMainPlayerRagdollState(local, ragdoll, dead);
}

}  // namespace

coop::net::Session* Sess() { return g_session.load(std::memory_order_acquire); }

bool IsHost() {
    auto* s = Sess();
    return s && s->role() == coop::net::Role::Host;
}

bool PositionOf(int slot, ue_wrap::FVector& out) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    auto& reg = coop::players::Registry::Get();
    if (IsLocalSlot(slot)) {
        void* const local = reg.Local();
        return local && E::TryGetActorLocation(local, out);
    }
    coop::RemotePlayer* const p = reg.Puppet(static_cast<uint8_t>(slot));
    return p && p->valid() && p->GetActor() && E::TryGetActorLocation(p->GetActor(), out);
}

bool IsAlive(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    if (IsLocalSlot(slot)) {
        bool ragdoll = false, dead = true;
        return LocalRagdollDead(ragdoll, dead) && !dead;
    }
    return coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(slot)) == coop::players::PuppetLife::Alive;
}

bool IsWakeableSleeper(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    if (IsLocalSlot(slot)) {
        bool ragdoll = true, dead = true;
        return sleep_sync::LocalState() == SleepPeerState::Resting && !sleep_sync::LocalBed().empty() &&
               LocalRagdollDead(ragdoll, dead) && !ragdoll && !dead;
    }
    if (sleep_sync::PeerState(slot) != SleepPeerState::Resting || sleep_sync::BedOf(slot).empty()) return false;
    coop::RemotePlayer* const p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    return p && p->valid() && !p->IsRagdollDisplayed() && coop::sleepers::IsLyingOnline(slot);
}

bool WakeLocalSleeper(uint32_t eventId) {
    if (!IsWakeableSleeper(static_cast<int>(coop::players::Registry::Get().LocalPeerId()))) {
        UE_LOGW("push_wake: event %u: not waking the local player -- not resting in a bed (state %d)", eventId,
                static_cast<int>(sleep_sync::LocalState()));
        return false;
    }
    const bool ok = SLP::CallWakeup();
    UE_LOGI("push_wake: event %u: the local player %s (gamemode.wakeup)", eventId, ok ? "was woken" : "could NOT be woken");
    return ok;
}

void ShowHint(const std::string& utf8) {
    void* const local = coop::players::Registry::Get().Local();
    if (!local || utf8.empty()) return;
    const std::wstring text = coop::text::FromUtf8Lossy(utf8.c_str());
    if (!ue_wrap::votv_lib::AddHint(text, 0, local))
        UE_LOGW("push_wake: the hint did not show (lib_C::addHint unresolved): '%s'", utf8.c_str());
}

void ShowDenial(PushWakeReason why, uint16_t cooldownSecs) {
    char buf[160];
    switch (why) {
    case PushWakeReason::Cooldown:
        if (l10n::Fmt(buf, sizeof(buf), l10n::T("Cooldown: %1$d:%2$02d"), cooldownSecs / 60, cooldownSecs % 60) < 0) return;
        ShowHint(buf);
        return;
    case PushWakeReason::TooFar:      ShowHint(l10n::T("Too far away to wake them")); return;
    case PushWakeReason::Busy:        ShowHint(l10n::T("Someone is already waking them")); return;
    case PushWakeReason::Dead:        ShowHint(l10n::T("You can't wake anyone right now")); return;
    case PushWakeReason::RateLimited: ShowHint(l10n::T("Slow down")); return;
    case PushWakeReason::NotReady:
    case PushWakeReason::BadTarget:
    case PushWakeReason::NotAsleep:   ShowHint(l10n::T("They are not asleep")); return;
    default:                          ShowHint(l10n::T("They would not wake up")); return;
    }
}

void SendEvent(coop::net::Session& s, int toSlot, coop::net::PushWakeWhat what, uint32_t eventId, uint8_t waker,
               uint8_t sleeper, uint16_t durationMs, uint16_t cooldownSecs, PushWakeReason why) {
    coop::net::PushWakeEventPayload p{};
    p.op = static_cast<uint8_t>(coop::net::SleepOp::PushWakeEvent);
    p.what = static_cast<uint8_t>(what);
    p.wakerSlot = waker;
    p.sleeperSlot = sleeper;
    p.eventId = eventId;
    p.durationMs = durationMs;
    p.cooldownSecs = cooldownSecs;
    p.reason = static_cast<uint8_t>(why);
    if (toSlot >= 0) s.SendReliableToSlot(toSlot, coop::net::ReliableKind::SleepState, &p, sizeof(p));
    else s.SendReliable(coop::net::ReliableKind::SleepState, &p, sizeof(p));
}

}  // namespace detail

namespace {

namespace PWA = coop::push_wake_animation;
using coop::net::PushWakeEventPayload;
using coop::net::PushWakeWhat;

// The events this client was told to play: an Execute is trusted only for one of them, once.
struct View {
    uint32_t id = 0;
    uint8_t waker = 0, sleeper = 0;
    uint64_t atMs = 0;
    bool executed = false;
};
constexpr int kViews = 4;
View g_views[kViews];
int g_viewNext = 0;

View* FindView(uint32_t id) {
    for (View& v : g_views)
        if (v.id == id) return &v;
    return nullptr;
}

// One warning per refusal text, at most one every five seconds: a hostile sender cannot fill the log.
void Warn(const char* what, int sender) {
    static const char* s_last = nullptr;
    static uint64_t s_lastMs = 0;
    const uint64_t now = coop::net::NowMs();
    if (s_last == what && now - s_lastMs < 5000) return;
    s_last = what;
    s_lastMs = now;
    UE_LOGW("push_wake: refused -- %s (sender slot %d)", what, sender);
}

bool ZeroReserved(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != 0) return false;
    return true;
}

// CLIENT: the host's word.
void OnEvent(const PushWakeEventPayload& p, uint8_t sender) {
    if (sender != 0) return Warn("an event from a client", sender);
    if (p.what > static_cast<uint8_t>(PushWakeWhat::Denial)) return Warn("event kind", sender);
    if (p.reason >= coop::net::kPushWakeReasonCount) return Warn("event reason", sender);
    if (!ZeroReserved(p.reserved, sizeof(p.reserved))) return Warn("event reserved bytes", sender);
    const auto what = static_cast<PushWakeWhat>(p.what);
    if (what != PushWakeWhat::Denial && p.eventId == 0) return Warn("event id 0", sender);
    if ((what == PushWakeWhat::Start || what == PushWakeWhat::Denial) &&
        (p.sleeperSlot >= coop::players::kMaxPeers || (what == PushWakeWhat::Start && p.wakerSlot >= coop::players::kMaxPeers)))
        return Warn("event slot", sender);

    switch (what) {
    case PushWakeWhat::Start: {
        if (p.wakerSlot == p.sleeperSlot) return Warn("event with one slot twice", sender);
        View& v = g_views[g_viewNext];
        g_viewNext = (g_viewNext + 1) % kViews;
        v = View{p.eventId, p.wakerSlot, p.sleeperSlot, coop::net::NowMs(), false};
        if (p.wakerSlot == coop::players::Registry::Get().LocalPeerId()) detail::NoteRequestAnswered();
        PWA::StartPushWake(p.eventId, p.wakerSlot, p.sleeperSlot, p.durationMs);
        break;
    }
    case PushWakeWhat::Cancel:
        PWA::CancelPushWake(p.eventId);
        if (View* v = FindView(p.eventId)) *v = View{};
        break;
    case PushWakeWhat::Execute: {
        View* v = FindView(p.eventId);
        if (!v || v->sleeper != coop::players::Registry::Get().LocalPeerId() || v->executed ||
            coop::net::NowMs() - v->atMs > detail::kExecuteWaitMs + 1000)
            return Warn("an execute for no event of this player's", sender);
        v->executed = true;
        detail::WakeLocalSleeper(p.eventId);
        break;
    }
    case PushWakeWhat::Denial:
        detail::NoteRequestAnswered();
        if (p.reason == static_cast<uint8_t>(detail::PushWakeReason::Cooldown))
            detail::NoteCooldown(p.sleeperSlot, p.cooldownSecs);
        detail::ShowDenial(static_cast<detail::PushWakeReason>(p.reason), p.cooldownSecs);
        break;
    }
}

// An event whose sleeper is this player ends the moment they are awake on their own, before the host's Cancel arrives.
void CancelWhenSleeperWoke() {
    if (sleep_sync::LocalState() != coop::net::SleepPeerState::Awake) return;
    const uint8_t local = coop::players::Registry::Get().LocalPeerId();
    for (View& v : g_views) {
        if (v.id == 0 || v.sleeper != local || v.executed) continue;
        PWA::CancelPushWake(v.id);
        v = View{};
    }
}

}  // namespace

void Install(coop::net::Session* session) { detail::g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = detail::Sess();
    if (!s || !s->running()) return;
    const uint64_t now = coop::net::NowMs();
    for (View& v : g_views)
        if (v.id != 0 && now - v.atMs > 15000) v = View{};  // an old record: its event is long over
    if (detail::IsHost()) detail::HostTick(*s);
    CancelWhenSleeperWoke();
    detail::InputTick(*s);
}

void OnReliable(const uint8_t* payload, size_t len, uint8_t senderSlot) {
    auto* s = detail::Sess();
    if (!s || len < 1) return;
    const uint8_t op = payload[0];
    if (op == static_cast<uint8_t>(coop::net::SleepOp::PushWakeRequest)) {
        if (!detail::IsHost()) return Warn("a request sent to a client", senderSlot);
        if (len != sizeof(coop::net::PushWakeRequestPayload)) return Warn("request length", senderSlot);
        coop::net::PushWakeRequestPayload p{};
        std::memcpy(&p, payload, sizeof(p));
        if (senderSlot == 0 || senderSlot >= coop::players::kMaxPeers) return Warn("request sender", senderSlot);
        if (!ZeroReserved(p.reserved, sizeof(p.reserved))) return Warn("request reserved bytes", senderSlot);
        if (p.targetSlot >= coop::players::kMaxPeers) return Warn("request target", senderSlot);
        detail::HostRequest(*s, senderSlot, p.targetSlot);
        return;
    }
    if (op == static_cast<uint8_t>(coop::net::SleepOp::PushWakeEvent)) {
        if (detail::IsHost()) return Warn("an event sent to the host", senderSlot);
        if (len != sizeof(PushWakeEventPayload)) return Warn("event length", senderSlot);
        PushWakeEventPayload p{};
        std::memcpy(&p, payload, sizeof(p));
        OnEvent(p, senderSlot);
        return;
    }
    Warn("push-wake op", senderSlot);
}

void OnSlotGone(int slot) {
    auto* s = detail::Sess();
    if (s && detail::IsHost()) detail::HostOnSlotGone(*s, slot);
}

void OnSessionEnd() {
    for (View& v : g_views) {
        if (v.id != 0) PWA::CancelPushWake(v.id);
        v = View{};
    }
    g_viewNext = 0;
    detail::HostReset();
    detail::InputReset();
}

Hint HintFor(int slot, int* cooldownSecs) {
    if (cooldownSecs) *cooldownSecs = 0;
    if (slot < 0 || slot != detail::AimedSlot()) return Hint::None;
    const int left = detail::KnownCooldownSecs(slot);
    if (left > 0) {
        if (cooldownSecs) *cooldownSecs = left;
        return Hint::Cooldown;
    }
    return Hint::Wake;
}

}  // namespace coop::push_wake
