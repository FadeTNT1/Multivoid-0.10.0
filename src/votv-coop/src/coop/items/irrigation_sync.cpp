// coop/items/irrigation_sync.cpp -- the irrigation lane's entry and its client half: the watches on the placing and
// taking verbs, the births a client's own placement makes, and the host's state applied. The host's half is
// irrigation_host.cpp, the grammar and the apply irrigation_wire.cpp. See coop/items/irrigation_sync.h.

#include "coop/items/irrigation_sync.h"
#include "irrigation_detail.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::irrigation_sync {
namespace {

namespace D  = detail;
namespace IR = ue_wrap::irrigation;
namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowFaucetItemUse, "irrigation_sync", L"prop_faucet_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowHoseItemUse, "irrigation_sync", L"prop_hose_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowFaucetAction, "irrigation_sync", L"faucet_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowHoseAction, "irrigation_sync", L"groundhose_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowHoseDetach, "irrigation_sync", L"groundhose_C", L"detachSprinkler", Declared);
UE_FACT_FN(kRowSprinklerWater, "irrigation_sync", L"prop_sprinkler_C", L"doWater", Declared);
UE_FACT_FN(kRowFinishSpawningActor, "irrigation_sync", L"GameplayStatics", L"FinishSpawningActor", Declared);

constexpr int kTagFaucetUse = 0x49524731;    // 'IRG1'
constexpr int kTagHoseUse = 0x49524732;
constexpr int kTagFaucetAction = 0x49524733;
constexpr int kTagHoseAction = 0x49524734;
constexpr int kTagHoseDetach = 0x49524735;
constexpr int kTagSprinklerWater = 0x49524736;

struct Watch {
    const ue_wrap::facts::Function* row;
    int         tag;
    const char* what;
};
const Watch kWatches[] = {
    {&kRowFaucetItemUse, kTagFaucetUse, "prop_faucet_C.playerHandUse_LMB"},
    {&kRowHoseItemUse, kTagHoseUse, "prop_hose_C.playerHandUse_LMB"},
    {&kRowFaucetAction, kTagFaucetAction, "faucet_C.actionOptionIndex"},
    {&kRowHoseAction, kTagHoseAction, "groundhose_C.actionOptionIndex"},
    {&kRowHoseDetach, kTagHoseDetach, "groundhose_C.detachSprinkler"},
    {&kRowSprinklerWater, kTagSprinklerWater, "prop_sprinkler_C.doWater"},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

constexpr auto kSweepEvery = std::chrono::seconds(1);
constexpr auto kAssemblyTtl = std::chrono::seconds(20);
// A hose is committed this many ticks after it settles: connectHose links the sprinkler just after its finish().
constexpr int kSettleTicks = 2;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler g_asmCommit;   // host: clients' verbs
coop::blob_chunks::Assembler g_asmState;    // client: the host's state
uint32_t g_blobSeq = 0;
std::chrono::steady_clock::time_point g_lastSweep{};

R::ParamOffset g_actionParam{L"action"};
R::ParamOffset g_playerParam{L"player"};

// The finish-spawn seam, armed only while a held faucet's or hose's own left-button body runs on a client.
void* g_finishFn = nullptr;
bool  g_finishInstalled = false;
bool  g_finishRefused = false;
bool  g_finishArmed = false;
int   g_useDepth = 0;

// A client's own births, waiting to be committed.
std::vector<ue_wrap::CachedObjRef> g_ownFaucets;
struct OwnHose {
    ue_wrap::CachedObjRef ref;
    int settledTicks = 0;
};
std::vector<OwnHose> g_ownHoses;

// The keys the host named here; the ones its open snapshot named.
std::unordered_set<std::wstring> g_hostNamed;
std::unordered_set<std::wstring> g_snapNamed;
bool g_inSnap = false;
bool g_applying = false;
uint64_t g_waterCancelled = 0;
bool g_waterSaid = false;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}
bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }

void* PlayerArg(const sg::Call& c) {
    const int32_t off = g_playerParam.Of(c.function);
    return off >= 0 ? *reinterpret_cast<void* const*>(c.locals + off) : nullptr;
}
bool LocalPlayerArg(const sg::Call& c) {
    void* player = PlayerArg(c);
    return player && coop::players::Registry::Get().IsLocal(player);
}
int ActionArg(const sg::Call& c) {
    const int32_t off = g_actionParam.Of(c.function);
    return off >= 0 ? static_cast<int>(c.locals[off]) : -1;
}

bool SendCommit(coop::net::Session* s, const D::Commit& c) {
    std::vector<uint8_t> blob;
    D::WriteCommit(blob, c);
    return coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::IrrigationCommit, D::NextBlobSeq(), blob);
}

bool IsOwnPending(const std::wstring& key) {
    if (key.empty()) return false;
    for (const OwnHose& h : g_ownHoses)
        if (void* a = h.ref.Get(); a && IR::KeyOf(a) == key) return true;
    return false;
}

void TrackOwnHose(void* hose) {
    for (const OwnHose& h : g_ownHoses)
        if (h.ref.Get() == hose) return;
    OwnHose h;
    h.ref.Set(hose);
    g_ownHoses.push_back(h);
}

// ---- the finish-spawn seam: a faucet or hose the local player's held item just placed -------------------------

void OnFinishSpawnPost(void* /*context*/, void* source, void* spawned) {
    if (!spawned || !source || !ue_wrap::game_thread::IsGameThread()) return;
    if (!IsClient(Session())) return;
    const IR::Kind k = IR::KindOf(spawned);
    if (k == IR::Kind::Faucet && IR::IsFaucetItem(source)) {
        ue_wrap::CachedObjRef r;
        r.Set(spawned);
        g_ownFaucets.push_back(r);
        UE_LOGI("[IRRIGATION] client: this player placed a faucet (%p) -- its record goes to the host", spawned);
    } else if (k == IR::Kind::Hose && IR::IsHoseItem(source)) {
        TrackOwnHose(spawned);
        UE_LOGI("[IRRIGATION] client: this player started a hose (%p) -- it goes to the host once it is laid", spawned);
    }
}

void Arm(bool on) {
    if (!g_finishInstalled) return;
    if (g_finishArmed == on) return;
    ue_wrap::ufunction_hook::SetArmed(g_finishFn, &OnFinishSpawnPost, on);
    g_finishArmed = on;
}

void InstallFinishSeam() {
    if (g_finishInstalled || g_finishRefused) return;
    if (!R::FindClass(P::name::GameplayStaticsClass)) return;
    const ue_wrap::facts::Resolved finish = ue_wrap::facts::Resolve(kRowFinishSpawningActor);
    g_finishFn = finish.Fn();
    if (!finish || !ue_wrap::ufunction_hook::InstallPostHook(finish, &OnFinishSpawnPost, /*armed=*/false)) {
        g_finishRefused = true;
        UE_LOGE("[IRRIGATION] the placement seam did NOT install (%s) -- a client's faucet and hose stand in its own "
                "world alone", g_finishFn ? "native hook table full" : "the function is not on the class");
        return;
    }
    g_finishInstalled = true;
    UE_LOGI("[IRRIGATION] placement seam installed on FinishSpawningActor, armed only inside a held faucet's or hose's "
            "own left-button body");
}

// ---- the watches ---------------------------------------------------------------------------------------------

sg::Verdict OnItemUsePre(const sg::Call& c) {
    if (!IsClient(Session()) || !c.locals || !LocalPlayerArg(c)) return sg::Verdict::Run;
    ++g_useDepth;
    Arm(true);
    return sg::Verdict::Run;
}

void OnItemUsePost(const sg::Call& /*c*/) {
    if (g_useDepth <= 0) return;
    if (--g_useDepth == 0) Arm(false);
}

// A client's take of a placed faucet (action 7): the host runs it.
sg::Verdict OnFaucetActionPre(const sg::Call& c) {
    coop::net::Session* s = Session();
    if (!IsClient(s) || !c.object || !c.locals) return sg::Verdict::Run;
    if (ActionArg(c) != P::name::IrrigationFaucetActionTake || !LocalPlayerArg(c)) return sg::Verdict::Run;
    const std::wstring key = IR::KeyOf(c.object);
    if (!D::KeyFits(key) || !g_hostNamed.count(key)) {
        UE_LOGW("[IRRIGATION] client: took up faucet key='%ls', which the host never named -- it runs here alone",
                key.c_str());
        return sg::Verdict::Run;
    }
    D::Commit cm;
    cm.op = D::CommitOp::TakeFaucet;
    cm.key = key;
    if (!SendCommit(s, cm)) {
        UE_LOGW("[IRRIGATION] client: faucet take key='%ls' was not sent -- it runs here alone", key.c_str());
        return sg::Verdict::Run;
    }
    UE_LOGI("[IRRIGATION] client: faucet take key='%ls' -> host (cancelled here; the host's state follows)", key.c_str());
    return sg::Verdict::Cancel;
}

// A client's pickup of a placed hose (action 4): it runs here, and the host is told.
sg::Verdict OnHoseActionPre(const sg::Call& c) {
    coop::net::Session* s = Session();
    if (!IsClient(s) || !c.object || !c.locals) return sg::Verdict::Run;
    if (ActionArg(c) != P::name::IrrigationHoseActionPickup || !LocalPlayerArg(c)) return sg::Verdict::Run;
    bool doCollect = false;
    IR::ReadHoseDoCollect(c.object, doCollect);
    const bool collect = doCollect || !IR::HoseFaucet(c.object);
    const std::wstring key = IR::KeyOf(c.object);
    if (!collect) TrackOwnHose(c.object);   // resume(): laid again, committed again when it settles
    if (D::KeyFits(key) && g_hostNamed.count(key)) {
        D::Commit cm;
        cm.op = D::CommitOp::TakeHose;
        cm.key = key;
        cm.collect = collect;
        const bool sent = SendCommit(s, cm);
        UE_LOGI("[IRRIGATION] client: hose key='%ls' %s -> host%s", key.c_str(), collect ? "collected" : "resumed",
                sent ? "" : " -- NOT SENT");
        if (collect) g_hostNamed.erase(key);
    } else {
        UE_LOGI("[IRRIGATION] client: hose key='%ls' %s -- the host never named it, nothing to tell", key.c_str(),
                collect ? "collected" : "resumed");
    }
    return sg::Verdict::Run;
}

// A hose's sprinkler detached here (the local player grabbed or used the sprinkler): the host detaches it too.
void OnHoseDetachPost(const sg::Call& c) {
    coop::net::Session* s = Session();
    if (g_applying || !IsClient(s) || !c.object) return;
    const std::wstring key = IR::KeyOf(c.object);
    if (!D::KeyFits(key) || !g_hostNamed.count(key)) return;
    D::Commit cm;
    cm.op = D::CommitOp::DetachSprinkler;
    cm.key = key;
    const bool sent = SendCommit(s, cm);
    UE_LOGI("[IRRIGATION] client: hose key='%ls' let its sprinkler go -> host%s", key.c_str(), sent ? "" : " -- NOT SENT");
}

// A sprinkler waters the host's plants only.
sg::Verdict OnSprinklerWaterPre(const sg::Call& /*c*/) {
    if (SprinklerWatersHere()) return sg::Verdict::Run;
    ++g_waterCancelled;
    if (!g_waterSaid) {
        g_waterSaid = true;
        UE_LOGI("[IRRIGATION] client: a sprinkler's doWater is the host's -- cancelled here (said once)");
    }
    return sg::Verdict::Cancel;
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        const Watch& w = kWatches[i];
        sg::PreFn pre = nullptr;
        sg::PostFn post = nullptr;
        switch (w.tag) {
            case kTagFaucetUse:
            case kTagHoseUse: pre = &OnItemUsePre; post = &OnItemUsePost; break;
            case kTagFaucetAction: pre = &OnFaucetActionPre; break;
            case kTagHoseAction: pre = &OnHoseActionPre; break;
            case kTagHoseDetach: post = &OnHoseDetachPost; break;
            default: pre = &OnSprinklerWaterPre; break;
        }
        if (sg::WatchName(*w.row, w.tag, pre, post)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[IRRIGATION] the script-body gate refused the watch on %s -- that verb stays on its own peer", w.what);
    }
}

void Settle() {
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("[IRRIGATION] the irrigation watches are live: both placements, the faucet take, the hose pickup, the "
                "sprinkler detach and doWater");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[IRRIGATION] %d of %d irrigation watches are dead -- those verbs stay on their own peer",
                kWatchCount - live, kWatchCount);
    }
}

// ---- the client's commits ---------------------------------------------------------------------------------------

void CommitOwn(coop::net::Session* s) {
    for (const ue_wrap::CachedObjRef& r : g_ownFaucets) {
        void* a = r.Get();
        if (!a || !r.Alive()) continue;
        const std::wstring key = IR::RegisterKey(a);
        D::Commit cm;
        cm.op = D::CommitOp::PlaceFaucet;
        if (!D::KeyFits(key) || !D::WireRecord(a, IR::Kind::Faucet, key, cm.rec)) {
            UE_LOGW("[IRRIGATION] client: the placed faucet gave no key or record -- it stands here alone");
            continue;
        }
        const bool sent = SendCommit(s, cm);
        UE_LOGI("[IRRIGATION] client: faucet key='%ls' -> host%s", key.c_str(), sent ? "" : " -- NOT SENT");
    }
    g_ownFaucets.clear();

    for (size_t i = 0; i < g_ownHoses.size();) {
        OwnHose& h = g_ownHoses[i];
        void* a = h.ref.Alive() ? h.ref.Get() : nullptr;
        if (!a) {
            UE_LOGI("[IRRIGATION] client: a hose this player was laying is gone before it settled -- nothing to send");
            g_ownHoses.erase(g_ownHoses.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (!IR::HoseSettled(a)) {
            h.settledTicks = 0;
            ++i;
            continue;
        }
        if (++h.settledTicks < kSettleTicks) { ++i; continue; }
        const std::wstring key = IR::RegisterKey(a);
        D::Commit cm;
        cm.op = D::CommitOp::CommitHose;
        if (!D::KeyFits(key) || !D::WireRecord(a, IR::Kind::Hose, key, cm.rec)) {
            UE_LOGW("[IRRIGATION] client: the laid hose gave no key or record -- it lies here alone");
        } else {
            const bool sent = SendCommit(s, cm);
            UE_LOGI("[IRRIGATION] client: hose key='%ls' laid (%zu points, faucet='%ls', sprinkler='%ls') -> host%s",
                    key.c_str(), cm.rec.vectors.empty() ? size_t{0} : cm.rec.vectors[0].size(),
                    D::RecordLink(cm.rec, 0).c_str(), D::RecordLink(cm.rec, 1).c_str(), sent ? "" : " -- NOT SENT");
        }
        g_ownHoses.erase(g_ownHoses.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---- the client's apply of the host's state ---------------------------------------------------------------------

void ApplyState(D::State& st) {
    switch (st.op) {
        case D::StateOp::SnapBegin:
            g_inSnap = true;
            g_snapNamed.clear();
            UE_LOGI("[IRRIGATION] client: the host's snapshot begins");
            return;
        case D::StateOp::SnapEnd: {
            int dropped = 0;
            for (IR::Kind k : {IR::Kind::Hose, IR::Kind::Faucet}) {
                for (void* a : IR::AllOf(k)) {
                    const std::wstring key = IR::KeyOf(a);
                    if (g_snapNamed.count(key) || IsOwnPending(key)) continue;
                    if (k == IR::Kind::Hose && !IR::HoseSettled(a)) continue;   // one this player is laying
                    g_applying = true;
                    D::UnlinkAndDestroy(a);
                    g_applying = false;
                    ++dropped;
                    UE_LOGI("[IRRIGATION] client: %s key='%ls' is not in the host's world -- destroyed here",
                            IR::KindName(k), key.c_str());
                }
            }
            g_inSnap = false;
            UE_LOGI("[IRRIGATION] client: the host's snapshot ends -- %u named, %d of ours destroyed", st.count, dropped);
            return;
        }
        case D::StateOp::Upsert: {
            const std::wstring key = st.rec.key;
            if (!D::KeyFits(key)) return;
            g_hostNamed.insert(key);
            if (g_inSnap) g_snapNamed.insert(key);
            if (st.kind == IR::Kind::Hose && IsOwnPending(key)) {
                UE_LOGI("[IRRIGATION] client: hose key='%ls' from the host -- this player is laying it, kept as is",
                        key.c_str());
                return;
            }
            bool created = false;
            g_applying = true;
            void* a = st.kind == IR::Kind::Faucet ? D::ApplyFaucet(st.rec, nullptr, created)
                                                  : D::ApplyHose(st.rec, nullptr, created);
            g_applying = false;
            UE_LOGI("[IRRIGATION] client: %s key='%ls' %s (links '%ls' '%ls')%s", IR::KindName(st.kind), key.c_str(),
                    created ? "built" : "updated", D::RecordLink(st.rec, 0).c_str(), D::RecordLink(st.rec, 1).c_str(),
                    a ? "" : " -- FAILED");
            return;
        }
        case D::StateOp::Remove: {
            g_hostNamed.erase(st.key);
            if (IsOwnPending(st.key)) {
                UE_LOGI("[IRRIGATION] client: %s key='%ls' gone on the host -- this player is laying it, kept",
                        IR::KindName(st.kind), st.key.c_str());
                return;
            }
            void* a = D::Find(st.kind, st.key);
            if (a) {
                g_applying = true;
                D::UnlinkAndDestroy(a);
                g_applying = false;
            }
            UE_LOGI("[IRRIGATION] client: %s key='%ls' gone -- %s", IR::KindName(st.kind), st.key.c_str(),
                    a ? "destroyed here" : "not here");
            return;
        }
    }
}

uint8_t SlotOf(uint8_t s) { return s < coop::net::kMaxPeers ? s : 0xFF; }

}  // namespace

namespace detail {
uint32_t NextBlobSeq() { return ++g_blobSeq; }
}  // namespace detail

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastSweep >= kSweepEvery) {
        g_lastSweep = now;
        g_asmCommit.Sweep(now, kAssemblyTtl);
        g_asmState.Sweep(now, kAssemblyTtl);
    }
    if (!session.running()) return;
    if (IsClient(&session)) {
        InstallFinishSeam();
        // A body whose post never came (another watch cancelled it) must not leave the seam armed.
        if (g_useDepth > 0) {
            g_useDepth = 0;
            Arm(false);
        }
        CommitOwn(&session);
    } else if (IsHost(&session) && D::host::PollDue()) {
        D::host::Tick(session);
    }
}

void OnPeerWorldReady(int slot) {
    coop::net::Session* s = Session();
    if (!IsHost(s) || slot <= 0) return;
    D::host::SnapshotTo(*s, slot);
}

void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    coop::net::Session* s = Session();
    if (!ue_wrap::game_thread::IsGameThread() || !IsHost(s)) return;
    senderSlot = SlotOf(senderSlot);
    if (senderSlot == 0 || senderSlot == 0xFF) return;
    std::vector<uint8_t> blob;
    if (!g_asmCommit.OnChunk(p, senderSlot, blob)) return;
    D::Commit c;
    if (!D::ReadCommit(blob, c)) {
        UE_LOGW("[IRRIGATION] host: a malformed commit from slot %u (%zu bytes) -- dropped",
                static_cast<unsigned>(senderSlot), blob.size());
        return;
    }
    D::host::OnCommit(*s, senderSlot, c);
}

void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    coop::net::Session* s = Session();
    if (!ue_wrap::game_thread::IsGameThread() || !IsClient(s)) return;
    // The host's alone: a client receives only what the host sends or relays, and this kind is never relayed.
    std::vector<uint8_t> blob;
    if (!g_asmState.OnChunk(p, senderSlot, blob)) return;
    D::State st;
    if (!D::ReadState(blob, st)) {
        UE_LOGW("[IRRIGATION] client: a malformed state from the host (%zu bytes) -- dropped", blob.size());
        return;
    }
    ApplyState(st);
}

void OnPeerLeftSlot(int slot) {
    if (slot < 0 || slot >= coop::net::kMaxPeers) return;
    g_asmCommit.ClearSlot(static_cast<uint8_t>(slot));
}

void OnDisconnect() {
    Arm(false);
    g_useDepth = 0;
    g_ownFaucets.clear();
    g_ownHoses.clear();
    g_hostNamed.clear();
    g_snapNamed.clear();
    g_inSnap = false;
    g_asmCommit.Clear();
    g_asmState.Clear();
    D::host::Reset();
    if (g_waterCancelled)
        UE_LOGI("[IRRIGATION] session end -- %llu sprinkler doWater call(s) cancelled here",
                static_cast<unsigned long long>(g_waterCancelled));
    g_waterCancelled = 0;
    g_waterSaid = false;
}

bool SprinklerWatersHere() { return !IsClient(Session()); }

}  // namespace coop::irrigation_sync
