// coop/farming/plant_refusals.cpp -- the plant producers a client must not run, refused at the script-body gate on an
// active client session. Each is a second author of state the host sends (the PlantState row and the prop lanes):
// the growth tick (growth, water, dryness, sun, medicine count-down, the death it can reach, the berry and tree fruit),
// the hourly disease roll and its pick, the dev tool's fruit, the tree fruit's spawn, the crop's destroy drops, the
// death, the disease damage, the physical damage and fire (the host's physics still kills its plant), the pine's log
// drop, the Stolas herb's lightning and self-destroy, the disease component's timers (spread, infect, the death plant's
// kill and its slither rewrite), the wild-cane grower's tick and spawn (a random cane field per peer), and the
// mushroom spore's hit (a random shroom plant per peer). A refused body changes nothing; what the host's own run
// produced arrives as a row, a Gone or a prop birth.

#include "plant_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <iterator>

namespace coop::farming::detail {
namespace {

namespace SG = ue_wrap::script_gate;

UE_FACT_FN(kRowPlantTick, "plant_sync", L"growingPlant_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowPlantAttemptDisease, "plant_sync", L"growingPlant_C", L"attemptDisease", Declared);
UE_FACT_FN(kRowPlantAddRandomDisease, "plant_sync", L"growingPlant_C", L"addRandomDisease", Declared);
UE_FACT_FN(kRowPlantFruit, "plant_sync", L"growingPlant_C", L"fruit", Declared);
UE_FACT_FN(kRowPlantAddFruit, "plant_sync", L"growingPlant_C", L"addFruit", Declared);
UE_FACT_FN(kRowPlantDropFruits, "plant_sync", L"growingPlant_C", L"dropFruits_grass", Declared);
UE_FACT_FN(kRowPlantTurnDead, "plant_sync", L"growingPlant_C", L"turnDead", Declared);
UE_FACT_FN(kRowPlantDamagePlant, "plant_sync", L"growingPlant_C", L"damagePlant", Declared);
UE_FACT_FN(kRowPlantAddDamage, "plant_sync", L"growingPlant_C", L"addDamage", Declared);
UE_FACT_FN(kRowPlantCustomEvent, "plant_sync", L"growingPlant_C", L"CustomEvent", Declared);
UE_FACT_FN(kRowPlantFireDamage, "plant_sync", L"growingPlant_C", L"fireDamage", Declared);
UE_FACT_FN(kRowPineBroken, "plant_sync", L"growingplant_pine_C", L"broken", Declared);
UE_FACT_FN(kRowHerbGrown, "plant_sync", L"growingplant_stolasherb_C", L"grown", Declared);
UE_FACT_FN(kRowDiseaseSpread, "plant_sync", L"plantDisease_C", L"spreadDisease", Declared);
UE_FACT_FN(kRowDiseaseInfect, "plant_sync", L"plantDisease_C", L"infect", Declared);
UE_FACT_FN(kRowDiseaseKillDeathPlant, "plant_sync", L"plantDisease_C", L"killDeathPlant", Declared);
UE_FACT_FN(kRowDiseaseEat, "plant_sync", L"plantDisease_C", L"eatfuckeat", Declared);
UE_FACT_FN(kRowCaneGrowerTick, "plant_sync", L"sugarcaneGrower_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowCaneGrowerSpawn, "plant_sync", L"sugarcaneGrower_C", L"spawn", Declared);
UE_FACT_FN(kRowSporeHit, "plant_sync", L"mushroomSpore_C",
           L"BndEvt__mushroomSpore_Sphere_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature", Declared);

struct Row {
    const ue_wrap::facts::Function* row;
    const char*                     what;
};
const Row kRows[] = {
    {&kRowPlantTick, "growingPlant_C::ReceiveTick"},
    {&kRowPlantAttemptDisease, "growingPlant_C::attemptDisease"},
    {&kRowPlantAddRandomDisease, "growingPlant_C::addRandomDisease"},
    {&kRowPlantFruit, "growingPlant_C::fruit"},
    {&kRowPlantAddFruit, "growingPlant_C::addFruit"},
    {&kRowPlantDropFruits, "growingPlant_C::dropFruits_grass"},
    {&kRowPlantTurnDead, "growingPlant_C::turnDead"},
    {&kRowPlantDamagePlant, "growingPlant_C::damagePlant"},
    {&kRowPlantAddDamage, "growingPlant_C::addDamage"},
    {&kRowPlantCustomEvent, "growingPlant_C::CustomEvent"},
    {&kRowPlantFireDamage, "growingPlant_C::fireDamage"},
    {&kRowPineBroken, "growingplant_pine_C::broken"},
    {&kRowHerbGrown, "growingplant_stolasherb_C::grown"},
    {&kRowDiseaseSpread, "plantDisease_C::spreadDisease"},
    {&kRowDiseaseInfect, "plantDisease_C::infect"},
    {&kRowDiseaseKillDeathPlant, "plantDisease_C::killDeathPlant"},
    {&kRowDiseaseEat, "plantDisease_C::eatfuckeat"},
    {&kRowCaneGrowerTick, "sugarcaneGrower_C::ReceiveTick"},
    {&kRowCaneGrowerSpawn, "sugarcaneGrower_C::spawn"},
    {&kRowSporeHit, "mushroomSpore_C::hit"},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kTagBase = 0x46524d00;   // 'FRM', then the row

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kRowCount] = {};
bool g_settled = false;

// Refusals per row, plus the destroy body plant_sync owns: logged at the first and each power of two.
uint64_t g_refused[kRowCount] = {};
uint64_t g_refusedDestroyed = 0;

void Say(const char* what, uint64_t n) {
    if ((n & (n - 1)) == 0)
        UE_LOGI("farm: client refused %s (count %llu)", what, static_cast<unsigned long long>(n));
}

SG::Verdict OnRowPre(const SG::Call& call) {
    if (!coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Run;
    Say(kRows[row].what, ++g_refused[row]);
    return SG::Verdict::Cancel;
}

}  // namespace

void InstallRefusals() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (SG::WatchName(*kRows[i].row, kTagBase + i, &OnRowPre, nullptr)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("farm: the watch on %s did not register -- a client runs it", kRows[i].what);
    }
}

bool RefusalsSettled() {
    if (g_settled) return true;
    int live = 0, pending = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && SG::NameWatchLive(*kRows[i].row, kTagBase + i)) ++live;
    }
    if (live == kRowCount) {
        g_settled = true;
        UE_LOGI("farm: %d/%d client refusal watches are live (plant tick, disease, fruit, death, damage, cane grower, "
                "spore)", live, kRowCount);
    } else if (pending == 0 && SG::PendingNameCount() == 0) {
        g_settled = true;
        for (int i = 0; i < kRowCount; ++i)
            if (g_reg[i] != Reg::Registered || !SG::NameWatchLive(*kRows[i].row, kTagBase + i))
                UE_LOGE("farm: the refusal of %s is dead -- a client runs it (its class declares no such body)",
                        kRows[i].what);
    }
    return g_settled;
}

void NoteRefusal(int row) {
    if (row == kRefusalReceiveDestroyed) Say("growingPlant_C::ReceiveDestroyed", ++g_refusedDestroyed);
}

void RefusalsSummary() {
    uint64_t total = g_refusedDestroyed;
    for (uint64_t n : g_refused) total += n;
    if (total) UE_LOGI("farm: session end -- %llu plant producer bodies refused here", static_cast<unsigned long long>(total));
    for (uint64_t& n : g_refused) n = 0;
    g_refusedDestroyed = 0;
}

}  // namespace coop::farming::detail
