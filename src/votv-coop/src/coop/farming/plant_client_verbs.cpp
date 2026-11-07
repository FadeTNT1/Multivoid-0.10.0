// coop/farming/plant_client_verbs.cpp -- the client's plant verbs, at the script-body gate. The harvest and the discard
// (actionOptionIndex, action 4) and a pot's planting are cancelled and sent: the host runs them with this player's
// puppet. The tending family (watering can, bucket, fertilizer, poo, growinator, poog, the plant dev tools) runs here,
// because the acting player's own Blueprint already wrote the plant and spent the item: a snapshot of the named plants
// near the player before the body and a diff after it send each raised field to the host, which raises its own copy.
// A medicine is cancelled and sent: the host adds the dose.

#include "plant_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/player/players_registry.h"

#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#include <cstring>
#include <string>
#include <vector>

namespace coop::farming::detail {
namespace {

namespace E  = ue_wrap::engine;
namespace GP = ue_wrap::growing_plant;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr float    kTendRadiusUU = 1500.0f;
constexpr uint64_t kSayMs = 10000;

R::ParamOffset g_actionParam{L"action"};
R::ParamOffset g_actionPlayer{L"player"};
R::ParamOffset g_medType{ue_wrap::profile::name::GrowingPlantMedicineTypeParam};
R::ParamOffset g_medTime{ue_wrap::profile::name::GrowingPlantMedicineTimeParam};
R::ParamOffset g_potHold{L"holdObject"};
R::ParamOffset g_potHoldName{L"holdPropName"};
R::ParamOffset g_potPlayer{L"player"};

uint64_t g_unboundSayMs = 0;

void* ObjectParam(R::ParamOffset& off, const sg::Call& c) {
    const int32_t o = off.Of(c.function);
    if (o < 0 || !c.locals) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, c.locals + o, sizeof(v));
    return v;
}

bool IsRemotePlayer(void* player) { return player && !coop::players::Registry::Get().IsLocal(player); }

void SayUnbound(const char* verb) {
    const uint64_t now = NowMs();
    if (now < g_unboundSayMs) return;
    g_unboundSayMs = now + kSayMs;
    UE_LOGI("farm: client %s on a plant the host has not named here yet -- refused, nothing sent", verb);
}

// ---- the tending family: before and after ----

struct Snap {
    void*       plant;
    std::string key;
    GP::State   st;
};
std::vector<Snap> g_tendSnap;
int g_tendDepth = 0;

void TakeSnapshot() {
    g_tendSnap.clear();
    void* local = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!local || !E::TryGetActorLocation(local, at)) return;
    const float p[3] = {at.X, at.Y, at.Z};
    for (const Near& n : ClientBoundNear(p, kTendRadiusUU)) {
        Snap s{n.plant, n.key, {}};
        if (GP::ReadState(n.plant, s.st)) g_tendSnap.push_back(std::move(s));
    }
}

void SendRaised() {
    for (const Snap& s : g_tendSnap) {
        GP::State now;
        if (!R::IsLive(s.plant) || ClientKeyOf(s.plant) != s.key || !GP::ReadState(s.plant, now)) continue;
        W::Intent m;
        m.op = W::IntentOp::Tend;
        m.key = s.key;
        if (now.water > s.st.water) { m.mask |= W::kTendWater; m.water = now.water; }
        if (now.fertilizer > s.st.fertilizer) { m.mask |= W::kTendFertilizer; m.fertilizer = now.fertilizer; }
        if (now.grow > s.st.grow) { m.mask |= W::kTendGrow; m.grow = now.grow; }
        if (now.fruitgrow > s.st.fruitgrow) { m.mask |= W::kTendFruit; m.fruitgrow = now.fruitgrow; }
        if (m.mask) ClientSend(std::move(m), Pending::Tend, nullptr);
    }
    g_tendSnap.clear();
}

}  // namespace

// actionOptionIndex on a client: action 4 is the harvest, or with `discard` set and the confirm key held the discard.
// What the body does alone here (a hint on a discard without the key, "no berries", the tree that has no harvest) runs.
sg::Verdict OnActionPre(const sg::Call& c) {
    if (!IsClient() || !c.object) return sg::Verdict::Run;
    const int32_t off = g_actionParam.Of(c.function);
    if (off < 0 || !c.locals || c.locals[off] != 4) return sg::Verdict::Run;
    void* player = ObjectParam(g_actionPlayer, c);
    if (IsRemotePlayer(player)) return sg::Verdict::Run;
    GP::State st;
    if (!GP::ReadState(c.object, st) || st.plantType == 2) return sg::Verdict::Run;
    bool discard = false, alt = false;
    GP::ReadDiscard(c.object, discard);
    GP::ReadAltHeld(player ? player : coop::players::Registry::Get().Local(), alt);
    if (discard && !alt) return sg::Verdict::Run;
    if (!discard && st.plantType == 1 && st.amount <= 0) return sg::Verdict::Run;
    const std::string key = ClientKeyOf(c.object);
    if (key.empty()) {
        SayUnbound(discard ? "discard" : "harvest");
        return sg::Verdict::Cancel;   // run here, it would harvest a plant the host does not know into a ghost
    }
    W::Intent m;
    m.op = discard ? W::IntentOp::Discard : W::IntentOp::Harvest;
    m.key = key;
    ClientSend(std::move(m), discard ? Pending::Discard : Pending::Harvest, nullptr);
    return sg::Verdict::Cancel;
}

sg::Verdict OnMedicinePre(const sg::Call& c) {
    if (!IsClient() || !c.object || !c.locals) return sg::Verdict::Run;
    const int32_t to = g_medType.Of(c.function), tm = g_medTime.Of(c.function);
    if (to < 0 || tm < 0) return sg::Verdict::Run;
    const std::string key = ClientKeyOf(c.object);
    if (key.empty()) {
        SayUnbound("medicine");
        return sg::Verdict::Cancel;
    }
    W::Intent m;
    m.op = W::IntentOp::Medicine;
    m.key = key;
    m.type = c.locals[to];
    std::memcpy(&m.time, c.locals + tm, sizeof(float));
    ClientSend(std::move(m), Pending::Medicine, nullptr);
    // The dose is the host's: its count-down runs in the plant's tick, which is refused here, so a dose kept here would
    // never expire and the item's own "already medicated" check would refuse the next one for good.
    return sg::Verdict::Cancel;
}

// prop_farmPot_C::playerUsedOn: a seed in this player's hand planted in a pot. A pot that would refuse it only shows a
// hint and spends nothing, so that runs here; one that would plant is the host's.
sg::Verdict OnPotUsePre(const sg::Call& c) {
    if (!IsClient() || !c.object || !c.locals) return sg::Verdict::Run;
    if (IsRemotePlayer(ObjectParam(g_potPlayer, c))) return sg::Verdict::Run;
    void* held = ObjectParam(g_potHold, c);
    const int32_t nameOff = g_potHoldName.Of(c.function);
    if (!held || !R::IsLive(held) || nameOff < 0) return sg::Verdict::Run;
    R::FName nm{0, 0};
    std::memcpy(&nm, c.locals + nameOff, sizeof(nm));
    std::wstring seedName = R::ToString(nm);
    if (seedName.empty() || seedName == L"None") seedName = ue_wrap::prop::GetPropNameString(held);
    if (!GP::PotWouldPlant(c.object, seedName)) return sg::Verdict::Run;
    const std::wstring potKey = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (potKey.empty() || potKey == L"None") {
        UE_LOGW("farm: client pot planting refused -- the pot has no key the host can resolve");
        return sg::Verdict::Cancel;   // a pot plant born here would be a second author
    }
    W::Intent m;
    m.op = W::IntentOp::PotPlant;
    m.key = Narrow(potKey);
    m.seedCls = Narrow(R::ClassNameOf(held));
    m.seedName = Narrow(seedName);
    ClientSend(std::move(m), Pending::PotPlant, held);
    return sg::Verdict::Cancel;
}

sg::Verdict OnTendPre(const sg::Call&) {
    if (!IsClient()) return sg::Verdict::Run;
    if (g_tendDepth++ == 0) TakeSnapshot();
    return sg::Verdict::Run;
}

void OnTendPost(const sg::Call&) {
    if (g_tendDepth == 0) return;
    if (--g_tendDepth == 0 && IsClient()) SendRaised();
}

void ClientVerbsReset() {
    g_tendSnap.clear();
    g_tendDepth = 0;
    g_unboundSayMs = 0;
}

}  // namespace coop::farming::detail
