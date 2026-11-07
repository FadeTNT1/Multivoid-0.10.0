// coop/farming/plant_sync.cpp -- see coop/farming/plant_sync.h. The lane's glue: the session, the shared outbound queue,
// a plant's cross-peer name, the verb watches and which side each call goes to, the routers' entries and the edges.

#include "coop/farming/plant_sync.h"

#include "plant_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/portable_identity.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/world/game_rules.h"

#include <string.h>  // _stricmp
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace coop::farming {
namespace detail {
namespace {

namespace GP = ue_wrap::growing_plant;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the outbound queue: the host's state, or a client's intents; a send the link refuses stays ----

constexpr size_t kMaxQueuedOut = 1024;

struct Out {
    int                  slot;   // -1: every client (host) or the host (client)
    std::vector<uint8_t> bytes;
};
std::deque<Out> g_out;
uint32_t g_sendSeq = 1;
bool g_fullSaid = false;

// Whether any client's world is up. A joiner gets every row in its snapshot, so what no one hears is not kept.
bool AnyClientReady(const coop::net::Session& s) {
    for (int i = 1; i < static_cast<int>(coop::net::kMaxPeers); ++i)
        if (s.IsSlotWorldReady(i)) return true;
    return false;
}

void DrainOut(coop::net::Session& s) {
    const bool host = s.role() == coop::net::Role::Host;
    if (!s.running() || (!host && !s.connected())) return;
    const auto kind = host ? coop::net::ReliableKind::PlantState : coop::net::ReliableKind::PlantIntent;
    for (int n = 0; n < 32 && !g_out.empty(); ++n) {
        const Out& o = g_out.front();
        // Nobody to hear it: a broadcast with no client up, or a message to a slot whose world is down (it left, or
        // re-seeds and is sent its snapshot again).
        if (host && (o.slot < 0 ? !AnyClientReady(s) : !s.IsSlotWorldReady(o.slot))) {
            g_out.pop_front();
            continue;
        }
        const bool ok = (host && o.slot >= 0) ? coop::blob_chunks::SendBlobToSlot(&s, o.slot, kind, g_sendSeq, o.bytes)
                                              : coop::blob_chunks::SendBlob(&s, kind, g_sendSeq, o.bytes);
        ++g_sendSeq;       // a refused blob goes again whole next tick, under a fresh number
        if (!ok) return;
        g_out.pop_front();
    }
}

// ---- the host's own action-4 runs: whether a destroy inside one is a harvest, and what the harvest yielded ----

R::ParamOffset g_actionParam{L"action"};

// One entry per action-4 body running on the host. A crop's yield is not the action's: the action destroys the plant
// and ReceiveDestroyed -> dropFruits_grass rolls randomIntegerInRange(amount_range) afresh at every loop step and spawns
// one prop per step that passes, so a row whose range starts at 0 (the wild cane's is 0..1) yields nothing about half
// the time in the unmodded game. The entry carries what the roll started from and counts the props born inside the
// action, so the log says "range 0..1, 0 born" instead of leaving a silent no-drop to look like a lost item.
struct HostAction {
    bool         harvest = false;   // a harvest (not a discard)
    int          born = 0;          // props that finished init inside the action
    uint8_t      type = 0;          // the row's plantType
    int32_t      rangeMin = 0, rangeMax = 0;
    float        grow = 0.f;
    std::wstring row;
};
std::vector<HostAction> g_hostAction;

// The wild sugarcane's yield, fixed (see OnActionPreAll).
constexpr int32_t kWildCaneYield = 2;

// The world's Funny setting (struct_gameRules funnySetting), from the process copy every rule is read from. A
// harvest is rare, so the snapshot's allocation is fine here. False when the rules are not readable.
bool FunnyRuleOn() {
    ue_wrap::game_rules::Snapshot snap;
    if (!ue_wrap::game_rules::ReadLocal(snap)) return false;
    for (const auto& f : snap.fields)
        if (f.kind == ue_wrap::game_rules::Kind::Bool && _stricmp(f.key.c_str(), ue_wrap::profile::name::GameRuleFunnyKey) == 0)
            return f.bval;
    return false;
}

uint8_t ActionOf(const sg::Call& c) {
    const int32_t off = g_actionParam.Of(c.function);
    return (off >= 0 && c.locals) ? c.locals[off] : 0;
}

sg::Verdict OnActionPreAll(const sg::Call& c) {
    if (!IsHost()) return OnActionPre(c);
    if (ActionOf(c) == 4) {
        HostAction a;
        bool discard = false;
        GP::ReadDiscard(c.object, discard);
        a.harvest = !discard;
        GP::State st;
        if (a.harvest && GP::ReadState(c.object, st)) {
            a.type = st.plantType;
            a.rangeMin = st.amountMin;
            a.rangeMax = st.amountMax;
            a.grow = st.grow;
            a.row = st.plant;
            // House rule (user decision, 2026-11-07): with the world's Funny setting on, a wild cane always yields
            // two. Its row's 0..1 range drops nothing half the time, which players read as the cane "turning into
            // nothing". With Funny off the game's own 0.10.0 roll stands. The host's harvest is the only one that
            // runs and every peer plays under the host's rules, so this plant's copy of the range covers everyone.
            if (a.type == 0 && a.row == ue_wrap::profile::name::FarmPlantWildSugarcaneRow && FunnyRuleOn() &&
                GP::WriteAmountRange(c.object, kWildCaneYield, kWildCaneYield)) {
                a.rangeMin = kWildCaneYield;
                a.rangeMax = kWildCaneYield;
            }
        }
        g_hostAction.push_back(std::move(a));
    }
    return sg::Verdict::Run;
}

void OnActionPost(const sg::Call& c) {
    if (!IsHost() || ActionOf(c) != 4 || g_hostAction.empty()) return;
    const HostAction a = std::move(g_hostAction.back());
    g_hostAction.pop_back();
    if (a.harvest && a.type == 0)
        UE_LOGI("farm: host harvest of %ls (grow %.3f): yield range %d..%d, %d prop(s) born%s", a.row.c_str(),
                static_cast<double>(a.grow), a.rangeMin, a.rangeMax, a.born,
                (a.rangeMin <= 0 && a.born == 0) ? " -- the game rolled 0 (its range starts at 0), not a lost item" : "");
}

// HOST: a prop finished init inside a harvest's action body -- the crop's drop, spawned by the destroyed plant.
void OnPropInitPost(const sg::Call& c) {
    if (g_hostAction.empty() || !g_hostAction.back().harvest || !c.object || !IsHost()) return;
    if (!ue_wrap::prop::IsDescendantOfProp(c.object)) return;
    ++g_hostAction.back().born;
}

// HOST: a plant began play (a save load, a spawner, a pot, a burial); its first row waits for its loaded data.
void OnBeginPlayPost(const sg::Call& c) {
    if (!c.object) return;
    if (IsHost()) HostOnBeginPlay(c.object);
    else ClientOnBeginPlay(c.object);
}

// A plant's destroy. CLIENT: its body drops the seed or the produce, which are the host's births; refused. HOST: the
// row's Gone, harvested when the destroy runs inside an action-4 harvest.
sg::Verdict OnDestroyedPre(const sg::Call& c) {
    if (coop::spawn_authority::IsActiveClientSession()) {
        NoteRefusal(kRefusalReceiveDestroyed);
        return sg::Verdict::Cancel;
    }
    if (IsHost() && c.object) HostOnDestroyed(c.object, !g_hostAction.empty() && g_hostAction.back().harvest);
    return sg::Verdict::Run;
}

// HOST: the berry a client's harvest spawned is not put into the puppet's inventory; it stays a world prop the client
// pockets once it arrives.
R::ParamOffset g_retargetItem{ue_wrap::profile::name::MainPlayerRetargetItemParam};
sg::Verdict OnRetargetPre(const sg::Call& c) {
    void* puppet = IsHost() ? HostHarvestPuppet() : nullptr;
    if (!puppet || c.object != puppet) return sg::Verdict::Run;
    const int32_t off = g_retargetItem.Of(c.function);
    void* item = nullptr;
    if (off >= 0 && c.locals) item = *reinterpret_cast<void* const*>(c.locals + off);
    HostCaptureItem(item);
    return sg::Verdict::Cancel;
}

UE_FACT_FN(kRowBeginPlay, "plant_sync", L"growingPlant_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowDestroyed, "plant_sync", L"growingPlant_C", L"ReceiveDestroyed", Declared);
UE_FACT_FN(kRowAction, "plant_sync", L"growingPlant_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowPropInit, "plant_sync", nullptr, L"init", AnyClass);
UE_FACT_FN(kRowMedicine, "plant_sync", L"growingPlant_C", L"addMedicine", Declared);
UE_FACT_FN(kRowRetarget, "plant_sync", L"mainPlayer_C", L"retarget add object to inventory", Declared);
UE_FACT_FN(kRowPotUse, "plant_sync", L"prop_farmPot_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowWaterCan, "plant_sync", L"prop_watercan_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowBucket, "plant_sync", L"prop_bucket_C", L"getFromWater", Declared);
UE_FACT_FN(kRowFertilizer, "plant_sync", L"prop_fertilizer_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowPoo, "plant_sync", L"prop_poo_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowGrowinator, "plant_sync", L"prop_growinator_C", L"fired", Declared);
UE_FACT_FN(kRowPoog, "plant_sync", L"prop_poog_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowToolGrow, "plant_sync", L"tool_plantGrow_C", L"init", Declared);
UE_FACT_FN(kRowToolWater, "plant_sync", L"tool_plantWater_C", L"init", Declared);
UE_FACT_FN(kRowToolFert, "plant_sync", L"tool_plantFert_C", L"init", Declared);
UE_FACT_FN(kRowToolFruit, "plant_sync", L"tool_plantFruit_C", L"init", Declared);

struct Watch {
    const ue_wrap::facts::Function* row;
    const char*                     what;
    sg::PreFn                       pre;
    sg::PostFn                      post;
};
const Watch kWatches[] = {
    {&kRowBeginPlay, "growingPlant_C::ReceiveBeginPlay", nullptr, &OnBeginPlayPost},
    {&kRowDestroyed, "growingPlant_C::ReceiveDestroyed", &OnDestroyedPre, nullptr},
    {&kRowAction, "growingPlant_C::actionOptionIndex", &OnActionPreAll, &OnActionPost},
    {&kRowPropInit, "prop init (harvest yield count)", nullptr, &OnPropInitPost},
    {&kRowMedicine, "growingPlant_C::addMedicine", &OnMedicinePre, nullptr},
    {&kRowRetarget, "mainPlayer_C::retarget add object to inventory", &OnRetargetPre, nullptr},
    {&kRowPotUse, "prop_farmPot_C::playerUsedOn", &OnPotUsePre, nullptr},
    {&kRowWaterCan, "prop_watercan_C::playerHandUse_LMB", &OnTendPre, &OnTendPost},
    {&kRowBucket, "prop_bucket_C::getFromWater", &OnTendPre, &OnTendPost},
    {&kRowFertilizer, "prop_fertilizer_C::playerHandUse_LMB", &OnTendPre, &OnTendPost},
    {&kRowPoo, "prop_poo_C::playerHandUse_LMB", &OnTendPre, &OnTendPost},
    {&kRowGrowinator, "prop_growinator_C::fired", &OnTendPre, &OnTendPost},
    {&kRowPoog, "prop_poog_C::playerHandUse_LMB", &OnTendPre, &OnTendPost},
    {&kRowToolGrow, "tool_plantGrow_C::init", &OnTendPre, &OnTendPost},
    {&kRowToolWater, "tool_plantWater_C::init", &OnTendPre, &OnTendPost},
    {&kRowToolFert, "tool_plantFert_C::init", &OnTendPre, &OnTendPost},
    {&kRowToolFruit, "tool_plantFruit_C::init", &OnTendPre, &OnTendPost},
};
constexpr int kWatchCount = static_cast<int>(std::size(kWatches));
constexpr int kTagBase = 0x464d5600;   // 'FMV', then the watch
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        const Watch& w = kWatches[i];
        if (sg::WatchName(*w.row, kTagBase + i, w.pre, w.post)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("farm: the watch on %s did not register -- that verb stays on its own peer", w.what);
    }
}

void Settle() {
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kTagBase + i)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("farm: %d/%d verb watches are live (begin-play, destroy, harvest, medicine, pocket, pot, tending)", live,
                kWatchCount);
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        for (int i = 0; i < kWatchCount; ++i)
            if (g_reg[i] != Reg::Registered || !sg::NameWatchLive(*kWatches[i].row, kTagBase + i))
                UE_LOGE("farm: the watch on %s is dead -- that verb stays on its own peer", kWatches[i].what);
    }
}

// A pre/post bracket stays open when its post never fires: another consumer's pre cancelled the body after this lane's
// ran, or the ProcessEvent firewall absorbed a fault in it (script_gate.h, IsBodyActive). With no watched body running
// on the game thread, every bracket is stale: it is closed here, so a later destroy is not read as a harvest and a later
// tend snapshots again.
bool g_unwindSaid = false;
void UnwindBrackets() {
    if (sg::CurrentThreadCall().active) return;
    const bool host = !g_hostAction.empty();
    g_hostAction.clear();
    const bool tend = ClientVerbsUnwind();
    if ((host || tend) && !g_unwindSaid) {
        g_unwindSaid = true;
        UE_LOGW("farm: a %s bracket lost its post (its body was cancelled elsewhere) -- closed", host ? "harvest" : "tend");
    }
}

coop::blob_chunks::Assembler g_asmIntent, g_asmState;
uint64_t g_nextSweepMs = 0;
uint64_t g_dropped = 0;

}  // namespace

coop::net::Session* SessionOf() { return g_session.load(std::memory_order_acquire); }

bool IsHost() {
    coop::net::Session* s = SessionOf();
    return s && s->running() && s->role() == coop::net::Role::Host;
}

bool IsClient() {
    coop::net::Session* s = SessionOf();
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}

uint64_t NowMs() { return coop::net::NowMs(); }

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '?');
    return s;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void Enqueue(int slot, std::vector<uint8_t>&& bytes) {
    if (g_out.size() >= kMaxQueuedOut) {
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("farm: the outbound queue is full (%zu) -- a message is dropped", g_out.size());
        }
        return;
    }
    g_out.push_back(Out{slot, std::move(bytes)});
}

std::string IdentityOf(void* plant) {
    const std::wstring key = GP::KeyOf(plant);
    if (!key.empty()) return Narrow(key);
    return Narrow(coop::element::PortableWireKey(plant));
}

}  // namespace detail

void Install(coop::net::Session* session) {
    detail::g_session.store(session, std::memory_order_release);
    if (!session) return;
    detail::RegisterWatches();
    detail::InstallRefusals();
}

void Tick(coop::net::Session& session) {
    if (!detail::g_settled || !detail::RefusalsSettled()) {
        ue_wrap::script_gate::ResolvePendingNames();
        detail::Settle();
    }
    detail::UnwindBrackets();
    if (!session.running()) return;
    if (session.role() == coop::net::Role::Host) detail::HostTick(session);
    else detail::ClientTick();
    detail::DrainOut(session);
    if (!detail::g_asmIntent.Idle() || !detail::g_asmState.Idle()) {
        const uint64_t now = detail::NowMs();
        if (now >= detail::g_nextSweepMs) {
            detail::g_nextSweepMs = now + 1000;
            detail::g_asmIntent.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
            detail::g_asmState.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
        }
    }
}

void OnPeerWorldReady(int slot) {
    if (!detail::IsHost() || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    detail::HostSnapshotTo(slot);
}

void OnIntentChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!detail::IsHost() || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (!detail::g_asmIntent.OnChunk(p, senderSlot, blob)) return;
    detail::W::Intent m;
    if (!detail::W::Parse(blob, m)) {
        ++detail::g_dropped;
        UE_LOGW("farm: host dropped a malformed PlantIntent (%zu B) from slot %u", blob.size(),
                static_cast<unsigned>(senderSlot));
        return;
    }
    detail::HostOnIntent(std::move(m), senderSlot);
}

void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!detail::IsClient() || senderSlot != 0) return;
    std::vector<uint8_t> blob;
    if (!detail::g_asmState.OnChunk(p, senderSlot, blob)) return;
    detail::W::State m;
    if (!detail::W::Parse(blob, m)) {
        ++detail::g_dropped;
        UE_LOGW("farm: client dropped a malformed PlantState (%zu B)", blob.size());
        return;
    }
    detail::ClientApply(m);
}

void OnPeerLeft(uint8_t slot) {
    detail::g_asmIntent.ClearSlot(slot);
    detail::HostPeerLeft(slot);
}

void OnDisconnect() {
    if (detail::g_dropped) UE_LOGI("farm: session end -- %llu malformed messages dropped",
                                   static_cast<unsigned long long>(detail::g_dropped));
    detail::HostReset();
    detail::ClientReset();
    detail::ClientVerbsReset();
    detail::RefusalsSummary();
    detail::g_out.clear();
    detail::g_asmIntent.Clear();
    detail::g_asmState.Clear();
    detail::g_hostAction.clear();
    detail::g_fullSaid = false;
    detail::g_dropped = 0;
}

void HostNotePlantWatered(void* plantActor) {
    if (detail::IsHost() && plantActor) detail::HostNoteWatered(plantActor);
}

}  // namespace coop::farming
