// coop/interactables/toggle_verbs.cpp -- see coop/interactables/toggle_verbs.h.

#include "coop/interactables/toggle_verbs.h"

#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/swinger_hold.h"  // a lid's native Close on a copy another peer drives or owns
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load stays local

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/devices/door_box.h"
#include "ue_wrap/devices/garage.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/devices/security_camera.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>

namespace coop::toggle_verbs {
namespace {

namespace sg = ue_wrap::script_gate;

// A verb's name matches every class's function of that name, so each row's callback first tests
// the row's class.
UE_FACT_FN(kRowUse, "toggle_verbs", nullptr, L"use", AnyClass);
UE_FACT_FN(kRowRunTrigger, "toggle_verbs", nullptr, L"runTrigger", AnyClass);
UE_FACT_FN(kRowActionOptionIndex, "toggle_verbs", nullptr, L"actionOptionIndex", AnyClass);
UE_FACT_FN(kRowVisual, "toggle_verbs", nullptr, L"visual", AnyClass);
UE_FACT_FN(kRowOpen, "toggle_verbs", nullptr, L"open", AnyClass);
UE_FACT_FN(kRowClose, "toggle_verbs", nullptr, L"close", AnyClass);
UE_FACT_FN(kRowFix, "toggle_verbs", nullptr, L"fix", AnyClass);
// A security camera's writers all end in one of these (the player's use ends in updState; the wisp's
// and the gray events' writes in upd or updState), except the cursed camera's night roll, which calls
// neither. Declared on the camera class, so another class's function of the name is not asked.
UE_FACT_FN(kRowCameraActionOptionIndex, "toggle_verbs", L"prop_camera_bad_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowCameraUpd, "toggle_verbs", L"prop_camera_bad_C", L"upd", Declared);
UE_FACT_FN(kRowCameraUpdState, "toggle_verbs", L"prop_camera_bad_C", L"updState", Declared);
UE_FACT_FN(kRowCursedReceiveTick, "toggle_verbs", L"prop_camera_cursed_C", L"ReceiveTick", Declared);

struct Row {
    const ue_wrap::facts::Function* verb;
    int tag;
    const char* what;
    bool (*IsInstance)(void* obj);
    void (*Edge)(void* actor);
    bool lockerLane = false;  // a locker's row: a refusal of it is said once, for the locker sync is under test
    sg::Verdict (*Pre)(const sg::Call&) = nullptr;  // before the body: may cancel it
};
std::atomic<coop::net::Session*> g_session{nullptr};

// A lid's own Close() before its body: on a copy another peer's drag drives or parks, or that another peer owns while
// this player is not holding it, the game's tick would shut and lock a lid that is not this peer's to close (and the
// edge would then be published over the owner's open). The lane's own apply of a close runs.
sg::Verdict OnLidClosePre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !call.object) return sg::Verdict::Run;
    if (!ue_wrap::swinger::IsSwinger(call.object)) return sg::Verdict::Run;
    if (coop::interactable_sync::ApplyingContainer(call.object)) return sg::Verdict::Run;
    if (s->role() != coop::net::Role::Host && !coop::net_pump::IsInAnnouncedWorld(call.object))
        return sg::Verdict::Run;   // a client's own world load is the host's save
    const std::wstring key = coop::interactable_sync::ContainerKey(call.object);
    if (key.empty() || coop::swinger_hold::MayRunNativeClose(call.object, key)) return sg::Verdict::Run;
    return sg::Verdict::Cancel;
}

const Row kRows[] = {
    { &kRowUse, 0x4C535755 /*'LSWU'*/, "a light switch's use", &ue_wrap::lightswitch::IsLightSwitch,
      &coop::interactable_sync::OnLightSwitchVerb },
    { &kRowRunTrigger, 0x47525254 /*'GRRT'*/, "a garage's runTrigger", &ue_wrap::garage::IsGarage,
      &coop::interactable_sync::OnGarageVerb },
    { &kRowActionOptionIndex, 0x41504149 /*'APAI'*/, "an appliance's action", &ue_wrap::appliance::IsAppliance,
      &coop::interactable_sync::OnApplianceVerb },
    { &kRowVisual, 0x41505653 /*'APVS'*/, "a server box's visual", &ue_wrap::appliance::IsAppliance,
      &coop::interactable_sync::OnApplianceVerb },
    { &kRowOpen, 0x44424F50 /*'DBOP'*/, "a locker's open", &ue_wrap::door_box::IsLocker,
      &coop::interactable_sync::OnDoorBoxVerb, true },
    // The player's interaction on 0.10.0 reaches a locker through its actionOptionIndex as well (the pak's locker_C
    // declares it beside `open`): watched too, and the lane's per-key dedup (lastKnown_) makes an edge `open` already
    // sent a no-op, so a path that moves `opened` without calling `open` is not lost.
    { &kRowActionOptionIndex, 0x44424C41 /*'DBLA'*/, "a locker's action", &ue_wrap::door_box::IsLocker,
      &coop::interactable_sync::OnDoorBoxVerb, true },
    // Also a door-box row under test: its first edge and its session count are said, so a log shows whether the
    // console's lid verb reached the lane on each peer (it logged nothing before, which left a silent lid unprovable).
    { &kRowActionOptionIndex, 0x44424149 /*'DBAI'*/, "a drone console's action", &ue_wrap::door_box::IsDroneConsole,
      &coop::interactable_sync::OnDoorBoxVerb, true },
    { &kRowOpen, 0x53574F50 /*'SWOP'*/, "a lid's open", &ue_wrap::swinger::IsSwinger,
      &coop::interactable_sync::OnContainerVerb },
    { &kRowClose, 0x5357434C /*'SWCL'*/, "a lid's close", &ue_wrap::swinger::IsSwinger,
      &coop::interactable_sync::OnContainerVerb, false, &OnLidClosePre },
    { &kRowFix, 0x4F564658 /*'OVFX'*/, "an oven's repair", &ue_wrap::appliance::IsOven,
      &coop::interactable_sync::OnOvenVerb },
    // The camera lanes are the host's (HostAuth): the host's edge sends, a client's does nothing here.
    { &kRowCameraActionOptionIndex, 0x43414149 /*'CAAI'*/, "a camera's action", &ue_wrap::security_camera::IsCameraClass,
      &coop::interactable_sync::OnCameraVerb },
    { &kRowCameraUpd, 0x43415550 /*'CAUP'*/, "a camera's upd", &ue_wrap::security_camera::IsCameraClass,
      &coop::interactable_sync::OnCameraVerb },
    { &kRowCameraUpdState, 0x43415553 /*'CAUS'*/, "a camera's updState", &ue_wrap::security_camera::IsCameraClass,
      &coop::interactable_sync::OnCameraVerb },
    { &kRowCursedReceiveTick, 0x43435254 /*'CCRT'*/, "the cursed camera's tick", &ue_wrap::security_camera::IsCameraClass,
      &coop::interactable_sync::OnCameraVerb },
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles
// once every watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kRowCount] = {};
bool g_settled = false;
uint64_t g_edges[kRowCount] = {};
uint64_t g_calls[kRowCount] = {};       // the row's body ran on any object, whatever its class
uint8_t  g_saidNotInstance[kRowCount] = {};
uint8_t  g_saidWorldGate[kRowCount] = {};
bool     g_saidFirstEdge[kRowCount] = {};

bool NameHasLocker(void* obj) {
    std::wstring n = ue_wrap::reflection::ClassNameOf(obj);
    for (auto& c : n) c = static_cast<wchar_t>(std::towlower(c));
    return n.rfind(L"locker_", 0) == 0;  // locker_C and its subclasses, not lockerguy_C
}

const Row* RowOf(int tag, int* index) {
    for (int i = 0; i < kRowCount; ++i)
        if (kRows[i].tag == tag) { *index = i; return &kRows[i]; }
    return nullptr;
}

// Every peer, after the body.
void OnVerbPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !call.object) return;
    int i = -1;
    const Row* row = RowOf(call.tag, &i);
    if (!row) return;
    ++g_calls[i];
    if (!row->IsInstance(call.object)) {
        // A locker-named class the lane does not recognise is the family never resolving, or the class not a
        // descendant of the one it resolved: the lane would stay silent for good, so it is said, once a row.
        if (row->lockerLane && g_saidNotInstance[i] < 1 && NameHasLocker(call.object)) {
            ++g_saidNotInstance[i];
            UE_LOGW("[TOGGLE-VERB] %s ran on '%ls' (class '%ls') but door_box does not recognise it as a locker "
                    "(locker_C %s) -- no edge is sent", row->what,
                    ue_wrap::reflection::ToString(ue_wrap::reflection::NameOf(call.object)).c_str(),
                    ue_wrap::reflection::ClassNameOf(call.object).c_str(),
                    ue_wrap::door_box::LockerClassResolved() ? "resolved, so the class is no descendant" : "NOT resolved");
        }
        return;
    }
    // A client's load of a world is the host's save; its devices' own calls there are not its player's.
    if (s->role() != coop::net::Role::Host && !coop::net_pump::IsInAnnouncedWorld(call.object)) {
        if (row->lockerLane && g_saidWorldGate[i] < 3) {
            ++g_saidWorldGate[i];
            UE_LOGW("[TOGGLE-VERB] %s ran on a door box on this client but its world is not the announced one yet -- "
                    "the edge is dropped (said up to 3 times a row; expected only during the client's own world load)",
                    row->what);
        }
        return;
    }
    ++g_edges[i];
    if (row->lockerLane && !g_saidFirstEdge[i]) {
        g_saidFirstEdge[i] = true;
        UE_LOGI("[TOGGLE-VERB] first edge of %s on '%ls' -- handed to the door-box lane", row->what,
                ue_wrap::reflection::ToString(ue_wrap::reflection::NameOf(call.object)).c_str());
    }
    row->Edge(call.object);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kRows[i].verb, kRows[i].tag, kRows[i].Pre, &OnVerbPost)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[TOGGLE-VERB] the script-body gate refused the watch on %s -- for the rest of this process that "
                "lane cannot see it", kRows[i].what);
    }
}

namespace {
// Which rows' watches are not live, by name: a locker row among them is the sync's own gate failing.
void SayNotLive() {
    for (int i = 0; i < kRowCount; ++i)
        if (!(g_reg[i] == Reg::Registered && sg::NameWatchLive(*kRows[i].verb, kRows[i].tag)))
            UE_LOGE("[TOGGLE-VERB] gate not live: %s ('%ls', %s)", kRows[i].what, kRows[i].verb->Name(),
                    g_reg[i] == Reg::Refused ? "refused" : g_reg[i] == Reg::Pending ? "never registered" : "unresolved or dead");
}
}  // namespace

void Tick() {
    sg::ResolvePendingNames();
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kRows[i].verb, kRows[i].tag)) ++live;
    }
    if (live == kRowCount) {
        g_settled = true;
        UE_LOGI("[TOGGLE-VERB] the toggle verb gates are live: %d of %d", live, kRowCount);
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[TOGGLE-VERB] %d of %d toggle verb gates are dead (refused, or resolved into a full table)",
                kRowCount - live, kRowCount);
        SayNotLive();
    } else {
        static const auto s_start = std::chrono::steady_clock::now();
        static bool s_saidSlow = false;
        if (!s_saidSlow && std::chrono::steady_clock::now() - s_start > std::chrono::seconds(60)) {
            s_saidSlow = true;
            UE_LOGW("[TOGGLE-VERB] %d of %d toggle verb gates still not live 60 s in", kRowCount - live, kRowCount);
            SayNotLive();
        }
    }
}

void OnDisconnect() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_edges[i])
            UE_LOGI("[TOGGLE-VERB] session end -- %s ran on a device of its row %llu time(s), the "
                    "lane's own applies among them", kRows[i].what, static_cast<unsigned long long>(g_edges[i]));
        else if (kRows[i].lockerLane)
            UE_LOGI("[TOGGLE-VERB] session end -- %s: body ran %llu time(s) on any object, 0 on a door box the lane took",
                    kRows[i].what, static_cast<unsigned long long>(g_calls[i]));
        g_edges[i] = 0;
        g_calls[i] = 0;
        g_saidFirstEdge[i] = false;
    }
}

}  // namespace coop::toggle_verbs
