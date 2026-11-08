// coop/interactables/window_stroke_sync.cpp -- see coop/interactables/window_stroke_sync.h.

#include "coop/interactables/window_stroke_sync.h"

#include "coop/config/config.h"
#include "coop/element/object_scan_hub.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/window_canvas.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace coop::window_stroke_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace WC = ue_wrap::window_canvas;
namespace SG = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

UE_FACT_FN(kRowK2DrawMaterial, "window_stroke_sync", L"Canvas", L"K2_DrawMaterial", Declared);

// A held sponge dabs every 0.25 s, but the pressure washer's shots each dab where they land, a few
// dozen a second while it sweeps the glass. These bounds hold half a minute of that.
constexpr size_t kMaxQueued = 1024;
constexpr auto kInboundTtl = std::chrono::seconds(30);
// Dabs replayed per tick. A backlog of washer dabs drains in a second or two at sixty frames, and
// no frame pays for more than sixteen canvas draws.
constexpr int kDrainPerTick = 16;

bool Verbose() {
    static const bool s = ::coop::config::ResolveFlag(::coop::config_registry::rows::window_stroke_log);
    return s;
}

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_armed{false};  // connected: the observer ignores every draw otherwise
int  g_wireApplyDepth = 0;         // GT: our own replay's draw is not a local stroke
bool g_hookInstalled = false;
bool g_hookArmed = false;
ue_wrap::facts::Resolved g_drawHook;  // the function the post-hook is installed on, armed and disarmed there
int  g_resolveRetry = 0;   // frames left before the next resolve attempt (GT only)

// Outbound: dabs read from the draw call inside the Func pre hook, sent from Tick (both game thread).
std::vector<WC::Dab> g_outbound;

// Inbound: dabs waiting for the window to be present and the canvas to be free.
struct Pending {
    WC::Dab dab;
    Clock::time_point deadline;
};
std::deque<Pending> g_inbound;

uint64_t g_sent = 0, g_applied = 0, g_unsent = 0, g_expired = 0, g_retried = 0;
// Per-source counts for the once-a-minute summary: sent and applied in the window so far.
uint32_t g_minSent[WC::kDabSourceCount] = {}, g_minApplied[WC::kDabSourceCount] = {};
Clock::time_point g_minStart{};

void SummaryTick() {
    const auto now = Clock::now();
    if (g_minStart == Clock::time_point{}) g_minStart = now;
    if (now - g_minStart < std::chrono::minutes(1)) return;
    g_minStart = now;
    uint32_t any = 0;
    for (uint8_t i = 0; i < WC::kDabSourceCount; ++i) any += g_minSent[i] + g_minApplied[i];
    if (any) {
        UE_LOGI("[CLEAN] glass last minute: sent sponge=%u washer=%u pour=%u mop=%u, applied sponge=%u "
                "washer=%u pour=%u mop=%u, queued=%zu, session expired=%llu retried=%llu",
                g_minSent[0], g_minSent[1], g_minSent[2], g_minSent[3], g_minApplied[0], g_minApplied[1],
                g_minApplied[2], g_minApplied[3], g_inbound.size(), static_cast<unsigned long long>(g_expired),
                static_cast<unsigned long long>(g_retried));
    }
    for (uint8_t i = 0; i < WC::kDabSourceCount; ++i) g_minSent[i] = g_minApplied[i] = 0;
}

uint8_t SourceIndex(WC::DabSource s) {
    const uint8_t i = static_cast<uint8_t>(s);
    return i < WC::kDabSourceCount ? i : 0;
}

// ---- the window singleton, found by the shared scan ----
void*    g_window = nullptr;
int32_t  g_windowIdx = -1;
uint32_t g_windowGen = 0;
void*    g_scanFound = nullptr;
int32_t  g_scanIdx = -1;

void HubPassBegin(void*, bool) { g_scanFound = nullptr; g_scanIdx = -1; }

void HubMatch(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !R::IsLive(obj)) return;
    g_scanFound = obj;
    g_scanIdx = R::InternalIndexOf(obj);
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const void* before = g_window;
    if (g_scanFound) {
        g_window = g_scanFound;
        g_windowIdx = g_scanIdx;
    } else if (isFull || !R::IsLiveByIndex(g_window, g_windowIdx)) {
        g_window = nullptr;
        g_windowIdx = -1;
    }
    g_windowGen = worldGen;
    if (g_window != before) UE_LOGI("window_stroke: window %p (world gen %u)", g_window, worldGen);
    return g_window ? 1 : 0;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "window_stroke", nullptr, &WC::EnsureResolved, &WC::IsWindow,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

void* ResolveWindow() {
    if (g_windowGen != ue_wrap::world_identity::Generation()) return nullptr;
    return R::IsLiveByIndex(g_window, g_windowIdx) ? g_window : nullptr;
}

// Func pre hook on UCanvas::K2_DrawMaterial: game thread, inside the engine call, before the draw.
// The dab is read from the call's own parameters -- position, size and the brush material -- so a
// stroke is sent whoever drew it: the held sponge, the mop, a thrown sponge, a bucket pour or the
// pressure washer's shot. Reading the brush off the player's hand, as this lane did, refused every
// cleaner but the held sponge, and the washer's glass never reached the other peers.
SG::Verdict OnDrawPre(void* /*canvas*/, void* sourceObject, const uint8_t* parms) {
    if (g_wireApplyDepth > 0 || !g_armed.load(std::memory_order_relaxed)) return SG::Verdict::Run;
    if (!WC::IsStrokeDraw(sourceObject)) return SG::Verdict::Run;
    WC::Dab dab;
    if (!WC::ReadStrokeDraw(sourceObject, parms, dab) || g_outbound.size() >= kMaxQueued) {
        ++g_unsent;
        return SG::Verdict::Run;
    }
    g_outbound.push_back(dab);
    return SG::Verdict::Run;
}

void SendOutbound(coop::net::Session* s) {
    if (g_outbound.empty()) return;
    for (const WC::Dab& d : g_outbound) {
        coop::net::WindowStrokePayload p{};
        p.x = d.x;
        p.y = d.y;
        p.size = d.size;
        p.opac = d.opac;
        p.col = d.col;
        p.source = static_cast<uint8_t>(d.source);
        if (!s->SendReliable(coop::net::ReliableKind::WindowStroke, &p, sizeof(p))) {
            ++g_unsent;
            continue;
        }
        ++g_sent;
        ++g_minSent[SourceIndex(d.source)];
        if (Verbose() || g_sent <= 3 || g_sent % 200 == 0)
            UE_LOGI("[CLEAN] glass dab sent src=%s #%llu at (%.1f,%.1f) size=%.0f opac=%.2f col=%.2f",
                    WC::DabSourceName(d.source), static_cast<unsigned long long>(g_sent), p.x, p.y, p.size,
                    p.opac, p.col);
    }
    g_outbound.clear();
}

void ApplyInbound() {
    if (g_inbound.empty()) return;
    const auto now = Clock::now();
    void* win = ResolveWindow();
    if (!win || WC::IsSignalPlaying()) {
        while (!g_inbound.empty() && now >= g_inbound.front().deadline) {
            g_inbound.pop_front();
            ++g_expired;
        }
        return;
    }
    // A budget, because the queue really does fill: while the window is unresolved or a signal is
    // playing, nothing leaves it but an expired dab, and a signal playing is ordinary play. A dab
    // costs a canvas open, two scalar writes and a draw -- six engine dispatches -- so draining a
    // full queue in one frame would spend some fifteen hundred of them in that frame. The rest keep
    // their place and their deadline and go out on the next tick, which at this budget is four
    // times the rate three peers can fill it at, and still above what nine can.
    for (int drawn = 0; drawn < kDrainPerTick && !g_inbound.empty(); ++drawn) {
        // The deadline holds on this path as well as on the parked one: a dab that waited out the
        // window is a wipe from a minute ago, and drawing it now paints over what has happened
        // since. Expiring here costs the draw, not the tick's budget.
        if (now >= g_inbound.front().deadline) {
            g_inbound.pop_front();
            ++g_expired;
            --drawn;
            continue;
        }
        const WC::Dab dab = g_inbound.front().dab;
        ++g_wireApplyDepth;
        const bool ok = WC::DrawDab(win, dab);
        --g_wireApplyDepth;
        if (!ok) {
            // One attempt, not one per queued dab: a draw fails because something it needs did not
            // resolve, and that answer will not differ for the dab behind it in the same frame. The
            // dab keeps its place, so a draw that fails once is tried again until its deadline
            // rather than lost.
            ++g_retried;
            if (g_retried <= 3 || g_retried % 200 == 0)
                UE_LOGW("window_stroke: replay failed at (%.1f,%.1f) -- holding it and the rest for next tick",
                        dab.x, dab.y);
            return;
        }
        g_inbound.pop_front();
        ++g_applied;
        ++g_minApplied[SourceIndex(dab.source)];
        if (Verbose() || g_applied <= 3 || g_applied % 200 == 0)
            UE_LOGI("[CLEAN] glass dab applied src=%s #%llu at (%.1f,%.1f) size=%.0f opac=%.2f",
                    WC::DabSourceName(dab.source), static_cast<unsigned long long>(g_applied), dab.x, dab.y,
                    dab.size, dab.opac);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();
}

void OnReliable(const coop::net::WindowStrokePayload& payload, uint8_t senderPeerSlot) {
    WC::Dab dab;
    dab.x = payload.x;
    dab.y = payload.y;
    dab.size = payload.size;
    dab.opac = payload.opac;
    dab.col = payload.col;
    // A value past the known sources is drawn with the sponge's brush.
    dab.source = payload.source < WC::kDabSourceCount ? static_cast<WC::DabSource>(payload.source)
                                                      : WC::DabSource::Sponge;
    if (g_inbound.size() >= kMaxQueued) {
        g_inbound.pop_front();
        ++g_expired;
    }
    g_inbound.push_back(Pending{dab, Clock::now() + kInboundTtl});
    if (Verbose())
        UE_LOGI("window_stroke: received src=%s (%.1f,%.1f) from slot %u", WC::DabSourceName(dab.source),
                dab.x, dab.y, static_cast<unsigned>(senderPeerSlot));
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    const bool connected = s && s->connected();
    g_armed.store(connected, std::memory_order_relaxed);
    // The resolve walks three classes and two functions, and a miss is not memoised, so a world
    // without the bay window would pay those walks every frame. Sixty frames between tries, the
    // shape the neighbouring installs use; the first success latches inside EnsureResolved.
    if (g_resolveRetry > 0) { --g_resolveRetry; return; }
    if (!WC::EnsureResolved()) {
        g_resolveRetry = 60;
        return;
    }
    RegisterWithScanHub();
    if (!g_hookInstalled) {
        // InstallPostHook is idempotent, so a failure is retried rather than latched: it fails when
        // the Func slot reads null or the table is full, and the first of those can be a resolve
        // that has not settled. Without the retry one bad tick left the lane dead for the process.
        g_drawHook = ue_wrap::facts::Resolve(kRowK2DrawMaterial);
        const bool ok = ue_wrap::ufunction_hook::InstallPreHook(g_drawHook, &OnDrawPre, /*armed=*/connected);
        if (ok) {
            g_hookInstalled = true;
            g_hookArmed = connected;
            UE_LOGI("window_stroke: K2_DrawMaterial pre hook installed");
        } else {
            g_resolveRetry = 60;
            static bool s_saidFailed = false;
            if (!s_saidFailed) {
                s_saidFailed = true;
                UE_LOGW("window_stroke: K2_DrawMaterial pre hook FAILED -- retrying; no dab of this "
                        "peer's own will be seen until it installs");
            }
            return;
        }
    }
    // The hook sits on a native the game draws through with no session at all, so it is armed only
    // while there is one: disarmed, the forwarder pays one load and a branch and never calls back.
    if (g_hookInstalled && g_hookArmed != connected) {
        ue_wrap::ufunction_hook::SetArmed(g_drawHook.Fn(), &OnDrawPre, connected);
        g_hookArmed = connected;
    }
    if (!connected) {
        g_outbound.clear();
        return;
    }
    SendOutbound(s);
    ApplyInbound();
    SummaryTick();
}

void OnDisconnect() {
    if (g_sent || g_applied || g_unsent || g_expired)
        UE_LOGI("window_stroke: session end -- sent=%llu applied=%llu unsent=%llu expired=%llu retried=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_unsent), static_cast<unsigned long long>(g_expired),
                static_cast<unsigned long long>(g_retried));
    g_armed.store(false, std::memory_order_relaxed);
    g_outbound.clear();
    g_inbound.clear();
    g_sent = g_applied = g_unsent = g_expired = g_retried = 0;
    for (uint8_t i = 0; i < WC::kDabSourceCount; ++i) g_minSent[i] = g_minApplied[i] = 0;
    g_minStart = Clock::time_point{};
    WC::ReleaseBrush();
}

}  // namespace coop::window_stroke_sync
