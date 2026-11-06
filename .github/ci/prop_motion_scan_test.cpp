// Host-compiled regression of coop/props/prop_motion_scan.cpp: the production source with its engine
// and coop boundaries replaced by the fixture below (prop_motion_scan_drill.py builds and runs it).

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ue_wrap/core/types.h"

namespace fixture {
struct Prop {
    ue_wrap::FVector  loc{};
    ue_wrap::FRotator rot{};
    bool simulating = true;
    bool mirror     = false;
    bool live       = true;
    bool inHand     = false;
    bool isProp     = true;
};
std::vector<Prop> props;                 // actor i is &props[i]; eid i + 1
std::unordered_set<void*> driven;        // rows the drive channel holds (all coasting here)
std::vector<void*> coastOrder;           // every Coast the scan asked for, in order
uint64_t nowMs = 0;
std::unordered_map<int, uint64_t> endAtMs;   // eid -> when an End last applied to its copy
ue_wrap::FVector camera{};
int eidOf(void* a) { return static_cast<int>(static_cast<Prop*>(a) - props.data()) + 1; }
}  // namespace fixture

#define UE_ASSERT_GAME_THREAD(x) ((void)0)
#define UE_LOGI(...) ((void)0)
#define UE_LOGW(...) ((void)0)

namespace coop::net { class Session {}; }
namespace coop::element {
using ElementId = uint32_t;
enum class ElementType : uint8_t { Unknown = 0, Player = 1, Prop = 2 };
class Registry {
public:
    static Registry& Get() { static Registry r; return r; }
    struct ActorIdPair { void* actor; ElementId id; int32_t internalIdx; bool mirror; };
    size_t SnapshotActorsByType(ElementType, std::vector<ActorIdPair>& out) const {
        out.clear();
        for (size_t i = 0; i < fixture::props.size(); ++i)
            out.push_back({&fixture::props[i], static_cast<ElementId>(i + 1), static_cast<int32_t>(i),
                           fixture::props[i].mirror});
        return out.size();
    }
};
}  // namespace coop::element
namespace coop {
class RemotePlayer { public: void* GetActor() const { return nullptr; } };
namespace players {
inline constexpr uint8_t kMaxPeers = 4;
class Registry {
public:
    static Registry& Get() { static Registry r; return r; }
    RemotePlayer* Puppet(uint8_t) { return nullptr; }
};
}  // namespace players
namespace active_drive { inline uint64_t NowMs() { return fixture::nowMs; } }
namespace prop_lifecycle { inline bool IsWireSuppressedPropClass(const std::wstring&) { return false; } }
namespace prop_drive_host {
inline bool IsDriven(void* a) { return fixture::driven.count(a) != 0; }
inline bool InAHand(void* a) { return static_cast<fixture::Prop*>(a)->inHand; }
inline size_t CoastingCount() { return fixture::driven.size(); }
inline void Coast(void* a, const char*) {
    fixture::coastOrder.push_back(a);
    if (!InAHand(a)) fixture::driven.insert(a);
}
}  // namespace prop_drive_host
namespace prop_drive_stream {
inline uint64_t LastEndAgeMs(uint32_t eid) {
    const auto it = fixture::endAtMs.find(static_cast<int>(eid));
    return it == fixture::endAtMs.end() ? UINT64_MAX : fixture::nowMs - it->second;
}
}  // namespace prop_drive_stream
}  // namespace coop
namespace ue_wrap {
namespace reflection {
inline bool IsLiveByIndex(void* a, int32_t) { return static_cast<fixture::Prop*>(a)->live; }
inline bool IsLive(void* a) { return a != nullptr; }
inline std::wstring ClassNameOf(void*) { return L"prop_C"; }
}  // namespace reflection
namespace prop {
inline bool IsDescendantOfProp(void* a) { return static_cast<fixture::Prop*>(a)->isProp; }
inline void* GetStaticMesh(void* a) { return a; }   // the mesh stands for its prop
}  // namespace prop
namespace engine {
inline bool TryGetActorLocation(void* a, FVector& out) { out = static_cast<fixture::Prop*>(a)->loc; return true; }
inline bool TryGetActorRotation(void* a, FRotator& out) { out = static_cast<fixture::Prop*>(a)->rot; return true; }
inline FVector GetCameraLocation() { return fixture::camera; }
inline bool IsComponentSimulatingPhysics(void* m) { return static_cast<fixture::Prop*>(m)->simulating; }
}  // namespace engine
}  // namespace ue_wrap

#include PROP_MOTION_SCAN_SOURCE

namespace {
namespace S = coop::prop_motion_scan;
int g_fail = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fail; }
}
coop::net::Session g_session;
// Ticks at ~60 Hz.
void Run(int ticks) {
    for (int i = 0; i < ticks; ++i) {
        fixture::nowMs += 16;
        S::Tick(g_session, /*isHost=*/true);
    }
}
void Reset(size_t n, float farX) {
    S::OnDisconnect();
    fixture::props.assign(n, fixture::Prop{});
    for (size_t i = 0; i < n; ++i) fixture::props[i].loc = ue_wrap::FVector{farX + 10.f * i, 0.f, 0.f};
    fixture::driven.clear();
    fixture::coastOrder.clear();
    fixture::endAtMs.clear();
    fixture::nowMs = 1000;
    fixture::camera = ue_wrap::FVector{};
}
}  // namespace

int main() {
    // A client's tick does nothing.
    Reset(10, 0.f);
    S::Tick(g_session, /*isHost=*/false);
    for (auto& p : fixture::props) p.loc.Z += 100.f;
    S::Tick(g_session, false);
    Check(fixture::coastOrder.empty(), "a client never coasts");

    // Resting props cost nothing; moved simulating ones coast, at most kMaxNewCoastsPerTick a tick.
    Reset(100, 0.f);
    Run(120);   // every baseline taken
    Check(fixture::coastOrder.empty(), "props at rest never coast");
    for (int i = 0; i < 10; ++i) fixture::props[i * 7].loc.Z -= 30.f;
    size_t prev = 0;
    bool perTickOk = true;
    for (int t = 0; t < 120; ++t) {
        Run(1);
        if (fixture::coastOrder.size() - prev > S::kMaxNewCoastsPerTick) perTickOk = false;
        prev = fixture::coastOrder.size();
    }
    Check(perTickOk, "no tick opens more than kMaxNewCoastsPerTick coasts");
    Check(fixture::coastOrder.size() == 10, "each moved prop coasts exactly once");
    Check(S::CoastsOpened() == 10, "the count follows");

    // A jitter under the epsilons is not motion.
    Reset(20, 0.f);
    Run(60);
    for (auto& p : fixture::props) { p.loc.X += 1.5f; p.rot.Yaw += 0.5f; }
    Run(60);
    Check(fixture::coastOrder.empty(), "sub-epsilon jitter never coasts");

    // The concurrent cap holds while nothing rests.
    Reset(60, 0.f);
    Run(60);
    for (auto& p : fixture::props) p.loc.Z -= 50.f;
    Run(200);
    Check(fixture::driven.size() == S::kMaxCoasting, "the concurrent cap bounds the coasting set");
    // As rows rest the rest get their turn: the over-budget props kept their old baseline.
    fixture::driven.clear();
    Run(200);
    Check(fixture::coastOrder.size() == 48 && fixture::driven.size() == S::kMaxCoasting,
          "the props held off coast once the channel has room");
    fixture::driven.clear();
    Run(200);
    Check(fixture::coastOrder.size() == 60, "every moved prop eventually coasts");

    // Near props go first: with the cap nearly full, the moved prop near the player takes the slot.
    Reset(400, 100000.f);   // all far
    fixture::props[399].loc = ue_wrap::FVector{100.f, 0.f, 0.f};   // one near the camera
    Run(300);   // every baseline taken
    // A full-but-one channel of other actors: the far movers and the near one compete for one slot.
    {
        static fixture::Prop dummies[S::kMaxCoasting];
        fixture::driven.clear();
        for (size_t i = 0; i < S::kMaxCoasting - 1; ++i) fixture::driven.insert(&dummies[i]);
    }
    for (int i = 0; i < 50; ++i) fixture::props[i].loc.Z -= 40.f;
    fixture::props[399].loc.Z -= 40.f;
    Run(1);
    Check(!fixture::coastOrder.empty() && fixture::coastOrder.front() == &fixture::props[399],
          "the near set takes the budget before the far one");

    // A kinematic prop moved by another lane never coasts, and its new place is the baseline.
    Reset(10, 0.f);
    Run(30);
    fixture::props[3].simulating = false;
    fixture::props[3].loc.Z += 200.f;
    Run(30);
    fixture::props[3].simulating = true;
    Run(30);
    Check(fixture::coastOrder.empty(), "a kinematic move never coasts, then or once it simulates again");

    // A peer's mirror, a prop in a hand and a dead actor never coast.
    Reset(10, 0.f);
    fixture::props[1].mirror = true;
    Run(30);
    fixture::props[1].loc.Z -= 50.f;
    fixture::props[2].inHand = true;
    fixture::props[2].loc.Z -= 50.f;
    fixture::props[4].live = false;
    fixture::props[4].loc.Z -= 50.f;
    Run(60);
    Check(fixture::coastOrder.empty(), "mirrors, held props and dead actors never coast");
    // Released where the hand left it: that place is the baseline, not a motion.
    fixture::props[2].inHand = false;
    Run(60);
    Check(fixture::coastOrder.empty(), "a prop let go of where the hand moved it does not coast");

    // A prop a row drives has its baseline follow the row, so its rest after the End does not re-coast.
    Reset(10, 0.f);
    Run(30);
    fixture::driven.insert(&fixture::props[5]);
    for (int i = 0; i < 20; ++i) { fixture::props[5].loc.X += 20.f; Run(3); }
    fixture::driven.clear();
    Run(60);
    Check(fixture::coastOrder.empty(), "the end of a row's motion is not coasted again");

    // A client's row the host just released settles from the End's pose: that motion is not coasted back to the
    // client that let go, and the settle's place is the baseline once the window is over.
    Reset(10, 0.f);
    Run(30);
    fixture::endAtMs[fixture::eidOf(&fixture::props[6])] = fixture::nowMs;
    for (int i = 0; i < 10; ++i) { fixture::props[6].loc.Z -= 3.f; Run(3); }
    Check(fixture::coastOrder.empty(), "a released row's settle inside the quiet window is not coasted");
    while (fixture::nowMs - fixture::endAtMs[fixture::eidOf(&fixture::props[6])] < S::kQuietAfterReleaseMs) Run(1);
    Run(60);
    Check(fixture::coastOrder.empty(), "the settle's place is the baseline after the window");
    fixture::props[6].loc.Z -= 30.f;
    Run(60);
    Check(fixture::coastOrder.size() == 1 && fixture::coastOrder.front() == &fixture::props[6],
          "a real move after the window coasts as before");

    if (g_fail == 0) std::printf("prop_motion_scan_test: all checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
