// coop/creatures/eyer_stare.cpp -- see coop/creatures/eyer_stare.h.

#include "coop/creatures/eyer_stare.h"

#include "coop/creatures/owner_entity_sync.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/eyer.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>

namespace coop::eyer_stare {
namespace {

namespace sg = ue_wrap::script_gate;

// goAngry is the custom event angrify (ten seconds of stare), instantAngry (a photo taken in view) and the
// ReceiveBeginPlay-bound timers reach; the kill sphere's begin-overlap handler is the bound event that bites.
UE_FACT_FN(kRowGoAngry, "eyer_stare", L"eyer_C", L"goAngry", Declared);
UE_FACT_FN(kRowKillSphere, "eyer_stare", L"eyer_C",
           L"BndEvt__eyer_killsphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);

constexpr int kTagGoAngry    = 0x45594741;   // 'EYGA'
constexpr int kTagKillSphere = 0x45594B53;   // 'EYKS'

bool g_goAngryWatched    = false;
bool g_killSphereWatched = false;
uint32_t g_puppetOverlaps = 0;

// After the body: an eyer became angry. owner_entity_sync tells a display copy (it is handed to this machine) from
// an eyer this machine owns (nothing to do).
void OnGoAngryPost(const sg::Call& c) {
    coop::owner_entity_sync::OnMirrorAngry(c.object);
}

// Before the body: the sphere began to overlap a peer's puppet. Nothing of the body should run.
sg::Verdict OnKillSpherePre(const sg::Call& c) {
    void* other = ue_wrap::eyer::OverlapOtherActor(c);
    if (!other || !coop::players::Registry::Get().IsPuppet(other)) return sg::Verdict::Run;
    const uint32_t n = ++g_puppetOverlaps;
    if (n <= 5 || (n % 50) == 0)
        UE_LOGI("eyer_stare: kill sphere %p touched a peer's puppet %p (#%u) -- the touch is ignored", c.object,
                other, n);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install() {
    if (g_goAngryWatched && g_killSphereWatched) return;
    if (!g_goAngryWatched)
        g_goAngryWatched = sg::WatchName(kRowGoAngry, kTagGoAngry, nullptr, &OnGoAngryPost);
    if (!g_killSphereWatched)
        g_killSphereWatched = sg::WatchName(kRowKillSphere, kTagKillSphere, &OnKillSpherePre, nullptr);
    if (g_goAngryWatched && g_killSphereWatched)
        UE_LOGI("eyer_stare: watching goAngry and the kill sphere's overlap on eyer_C");
}

}  // namespace coop::eyer_stare
