// coop/player/push_wake_animation.cpp -- see coop/player/push_wake_animation.h.

#include "coop/player/push_wake_animation.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/sleepers.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/actors/puppet_gesture.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/engine_bones.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace coop::push_wake_animation {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace N = ue_wrap::profile::name;
namespace Pup = ue_wrap::puppet;
using ue_wrap::FRotator;
using ue_wrap::FVector;
using Clock = std::chrono::steady_clock;

constexpr uint16_t kMinDurationMs = 300;
constexpr uint16_t kMaxDurationMs = 5000;
constexpr int   kShoves = 3;
constexpr float kPi = 3.14159265f;
constexpr float kEdgeFraction = 0.1f;       // the share of the event spent easing in, and easing out

constexpr float kSleeperSwayCm = 5.f;       // along the capsule's X, which stays level on a body lying on its side
constexpr float kSleeperRockDeg = 7.f;      // about the capsule's Z, the lying body's long axis
constexpr float kWakerArmDeg = 58.f;        // arms swung from hanging toward the sleeper
constexpr float kWakerArmPumpDeg = 14.f;
constexpr float kWakerBellyDeg = 16.f;
constexpr float kWakerBellyPumpDeg = 6.f;
constexpr float kWakerLeanDeg = 8.f;        // the whole body about the feet, which shows even with no arm nodes
constexpr float kWakerLeanPumpDeg = 4.f;
constexpr float kWakerPushCm = 4.f;

constexpr float kSlapVolume = 0.35f;
constexpr float kSlapPitch = 1.f;

// One body this event moves: its place under the capsule as it was, to go back to.
struct Body {
    void* actor = nullptr;
    int32_t idx = -1;
    Pup::BodyOffset base{};
    bool Live() const { return actor && R::IsLiveByIndex(actor, idx); }
};

struct Event {
    uint32_t id = 0;
    uint8_t waker = 0, sleeper = 0;
    Clock::time_point start{};
    float seconds = 0.f;
    int shoves = 0;
    bool sleeperRemote = false, wakerRemote = false;
    Body sleeperBody, wakerBody;
    Pup::ReachGesture reach;
    FRotator armAtStart{};
    bool armAtStartRead = false, armProbed = false;
};

std::vector<Event> g_events;
ue_wrap::CachedObjRef g_slap, g_attenuation;

void ResolveSounds() {
    if (!g_slap.Alive()) g_slap.Set(ue_wrap::asset_load::LoadObjectByPath(N::PushWakeSlapCuePath));
    if (!g_attenuation.Alive()) g_attenuation.Set(ue_wrap::asset_load::LoadObjectByPath(N::PushWakeAttenuationPath));
    if (!g_slap.Alive() || !g_attenuation.Alive())
        UE_LOGW("push_wake: the slap cue or its attenuation did not load (cue %d, attenuation %d) -- the shove is "
                "silent or not spatial", g_slap.Alive() ? 1 : 0, g_attenuation.Alive() ? 1 : 0);
}

// The live puppet actor of a slot, or null: the local player's slot, a slot with no puppet, a puppet shown as a ragdoll.
void* PuppetActor(uint8_t slot) {
    coop::RemotePlayer* const p = coop::players::Registry::Get().Puppet(slot);
    if (!p || !p->valid() || p->IsRagdollDisplayed()) return nullptr;
    return p->GetActor();
}

bool TakeBody(void* actor, Body& out) {
    if (!actor || !Pup::ReadBodyOffset(actor, out.base)) return false;
    out.actor = actor;
    out.idx = R::InternalIndexOf(actor);
    return true;
}

void RestoreBody(Body& b) {
    if (b.Live()) Pup::WriteBodyOffset(b.actor, b.base);
    b.actor = nullptr;
}

void Finish(Event& e, const char* why) {
    Pup::EndReachGesture(e.reach);
    RestoreBody(e.wakerBody);
    RestoreBody(e.sleeperBody);
    UE_LOGI("push_wake: event %u (waker %u, sleeper %u) ended: %s", e.id, static_cast<unsigned>(e.waker),
            static_cast<unsigned>(e.sleeper), why);
}

// Where the sleeper is on this peer: its puppet, or the local body when it is this player. False when neither reads.
bool SleeperAt(const Event& e, FVector& out) {
    void* const actor = e.sleeperRemote ? e.sleeperBody.actor : coop::players::Registry::Get().Local();
    return actor && E::TryGetActorLocation(actor, out);
}

void PlayShove(const Event& e) {
    FVector at{};
    void* const context = coop::players::Registry::Get().Local();
    void* const attenuation = g_attenuation.Get();
    // With no attenuation the engine plays it flat over the whole map; a shove is only heard near it.
    if (!context || !attenuation || !SleeperAt(e, at)) return;
    E::PlaySoundAtLocation(context, g_slap.Get(), at, attenuation, kSlapVolume, kSlapPitch);
}

// The unit level direction from the waker to the sleeper; the waker's facing when the sleeper's place does not read
// or they stand on top of each other.
FVector TowardSleeper(const Event& e, const FVector& wakerAt, float wakerYawRad) {
    FVector sleeperAt{};
    if (SleeperAt(e, sleeperAt)) {
        const float dx = sleeperAt.X - wakerAt.X, dy = sleeperAt.Y - wakerAt.Y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.f) return FVector{dx / len, dy / len, 0.f};
    }
    return FVector{std::cos(wakerYawRad), std::sin(wakerYawRad), 0.f};
}

void DriveSleeper(const Event& e, float sway, float envelope) {
    Pup::BodyOffset now = e.sleeperBody.base;
    now.loc.X += sway * kSleeperSwayCm * envelope;
    now.rot = Pup::RotateAbout(now.rot, FVector{0.f, 0.f, 1.f}, sway * kSleeperRockDeg * envelope);
    Pup::WriteBodyOffset(e.sleeperBody.actor, now);
}

void DriveWaker(Event& e, float sway, float envelope) {
    FVector at{};
    FRotator facing{};
    if (!E::TryGetActorLocation(e.wakerBody.actor, at) || !E::TryGetActorRotation(e.wakerBody.actor, facing)) return;
    const float yaw = facing.Yaw * 0.01745329252f;
    const FVector toward = TowardSleeper(e, at, yaw);
    // Each arm hangs down the world's -Z: toward x Z carries it onto `toward`.
    const FVector armAxis{toward.Y, -toward.X, 0.f};
    const float pump = (std::max)(sway, 0.f);
    Pup::DriveReachGesture(e.reach, armAxis, envelope * (kWakerArmDeg + pump * kWakerArmPumpDeg),
                           envelope * (kWakerBellyDeg + pump * kWakerBellyPumpDeg));
    // The same direction in the capsule's frame: the body mesh leans about its feet and is pushed along it.
    const FVector local{toward.X * std::cos(yaw) + toward.Y * std::sin(yaw),
                        -toward.X * std::sin(yaw) + toward.Y * std::cos(yaw), 0.f};
    Pup::BodyOffset now = e.wakerBody.base;
    now.loc.X += local.X * pump * kWakerPushCm * envelope;
    now.loc.Y += local.Y * pump * kWakerPushCm * envelope;
    now.rot = Pup::RotateAbout(now.rot, FVector{-local.Y, local.X, 0.f},
                               envelope * (kWakerLeanDeg + pump * kWakerLeanPumpDeg));
    Pup::WriteBodyOffset(e.wakerBody.actor, now);
}

// One line per event: whether the arm bone really moved, which tells a graph that plays the nodes from one that skips
// them.
void ProbeArm(Event& e) {
    if (e.armProbed || !e.armAtStartRead || !e.reach.active) return;
    e.armProbed = true;
    FRotator nowRot{};
    void* const mesh = Pup::GetSkeletalMeshComponent(e.wakerBody.actor);
    if (!mesh || !E::GetBoneWorldRotationByName(mesh, N::BoneUpperArmR, nowRot)) return;
    UE_LOGI("push_wake: event %u waker arm bone moved (pitch %.0f, yaw %.0f, roll %.0f) since the start", e.id,
            ue_wrap::NormalizeAxis(nowRot.Pitch - e.armAtStart.Pitch), ue_wrap::NormalizeAxis(nowRot.Yaw - e.armAtStart.Yaw),
            ue_wrap::NormalizeAxis(nowRot.Roll - e.armAtStart.Roll));
}

// Why this event must stop before its time, or null. Every body it moves is re-read here, so a body that went or a
// sleeper that got up is seen on the next frame.
const char* EndReason(const Event& e) {
    if (e.sleeperRemote) {
        if (!e.sleeperBody.Live()) return "the sleeper's body went";
        if (!coop::sleepers::IsLyingOnline(e.sleeper)) return "the sleeper is no longer lying";
    }
    if (e.wakerRemote && e.wakerBody.actor) {
        if (!e.wakerBody.Live()) return "the waker's body went";
        coop::RemotePlayer* const p = coop::players::Registry::Get().Puppet(e.waker);
        if (!p || p->IsRagdollDisplayed()) return "the waker went down";
    }
    return nullptr;
}

void Advance(Event& e, float fraction) {
    const float sway = std::sin(2.f * kPi * static_cast<float>(kShoves) * fraction);
    const float envelope = std::clamp((std::min)(fraction, 1.f - fraction) / kEdgeFraction, 0.f, 1.f);
    if (e.sleeperBody.actor) DriveSleeper(e, sway, envelope);
    if (e.wakerBody.actor) DriveWaker(e, sway, envelope);
    while (e.shoves < kShoves && fraction >= (static_cast<float>(e.shoves) + 0.25f) / static_cast<float>(kShoves)) {
        ++e.shoves;
        PlayShove(e);
        if (e.wakerBody.actor) ProbeArm(e);
    }
}

// End every running event `match` accepts, putting back what each moved.
template <class Pred>
void FinishWhere(Pred match, const char* why) {
    for (auto it = g_events.begin(); it != g_events.end();) {
        if (!match(*it)) { ++it; continue; }
        Finish(*it, why);
        it = g_events.erase(it);
    }
}

}  // namespace

void StartPushWake(uint32_t eventId, uint8_t wakerSlot, uint8_t sleeperSlot, uint16_t durationMs) {
    UE_ASSERT_GAME_THREAD("push_wake_animation::StartPushWake");
    if (wakerSlot == sleeperSlot || wakerSlot >= coop::players::kMaxPeers || sleeperSlot >= coop::players::kMaxPeers) {
        UE_LOGW("push_wake: event %u refused -- slots waker %u, sleeper %u", eventId, static_cast<unsigned>(wakerSlot),
                static_cast<unsigned>(sleeperSlot));
        return;
    }
    FinishWhere([&](const Event& e) { return e.id == eventId || e.waker == wakerSlot || e.sleeper == sleeperSlot; },
                "replaced by a new event");

    const uint8_t local = coop::players::Registry::Get().LocalPeerId();
    Event e;
    e.id = eventId;
    e.waker = wakerSlot;
    e.sleeper = sleeperSlot;
    e.start = Clock::now();
    e.seconds = static_cast<float>(std::clamp(durationMs ? durationMs : kDefaultDurationMs, kMinDurationMs, kMaxDurationMs)) / 1000.f;
    e.sleeperRemote = sleeperSlot != local;
    e.wakerRemote = wakerSlot != local;

    if (e.sleeperRemote) {
        if (!coop::sleepers::IsLyingOnline(sleeperSlot) || !TakeBody(PuppetActor(sleeperSlot), e.sleeperBody)) {
            UE_LOGW("push_wake: event %u refused -- slot %u is not lying online on this peer", eventId,
                    static_cast<unsigned>(sleeperSlot));
            return;
        }
    }
    if (e.wakerRemote && TakeBody(PuppetActor(wakerSlot), e.wakerBody)) {
        const bool arms = Pup::BeginReachGesture(e.wakerBody.actor, e.reach);
        void* const mesh = Pup::GetSkeletalMeshComponent(e.wakerBody.actor);
        e.armAtStartRead = arms && mesh && E::GetBoneWorldRotationByName(mesh, N::BoneUpperArmR, e.armAtStart);
        if (!arms) UE_LOGI("push_wake: event %u -- slot %u's AnimBP has no arm nodes; it leans only", eventId,
                           static_cast<unsigned>(wakerSlot));
    }
    ResolveSounds();
    UE_LOGI("push_wake: event %u started (waker %u%s, sleeper %u%s, %.2f s)", eventId, static_cast<unsigned>(wakerSlot),
            e.wakerRemote ? "" : " local", static_cast<unsigned>(sleeperSlot), e.sleeperRemote ? "" : " local", e.seconds);
    g_events.push_back(e);
}

void CancelPushWake(uint32_t eventId) {
    UE_ASSERT_GAME_THREAD("push_wake_animation::CancelPushWake");
    FinishWhere([&](const Event& e) { return e.id == eventId; }, "cancelled");
}

void Tick() {
    if (g_events.empty()) return;
    const auto now = Clock::now();
    for (auto it = g_events.begin(); it != g_events.end();) {
        Event& e = *it;
        const float fraction = std::chrono::duration<float>(now - e.start).count() / e.seconds;
        const char* why = EndReason(e);
        if (!why && fraction >= 1.f) why = "finished";
        if (why) {
            Finish(e, why);
            it = g_events.erase(it);
            continue;
        }
        Advance(e, fraction);
        ++it;
    }
}

void OnSlotGone(int slot) {
    if (g_events.empty()) return;
    FinishWhere([&](const Event& e) { return e.waker == slot || e.sleeper == slot; }, "a player's slot went");
}

}  // namespace coop::push_wake_animation
