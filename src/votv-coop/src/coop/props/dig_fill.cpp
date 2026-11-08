// coop/props/dig_fill.cpp -- see dig_fill.h.

#include "dig_fill.h"

#include "dig_out.h"

#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/dig_wire.h"
#include "coop/props/prop_echo_suppress.h"

#include "ue_wrap/actors/dirt.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace coop::dig_fill {
namespace {

namespace D  = ue_wrap::dirt;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace W  = coop::dig_wire;
namespace sg = ue_wrap::script_gate;

constexpr size_t   kMaxPending = 8;
constexpr uint64_t kAckWaitMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};

struct Pending {
    ue_wrap::CachedObjRef ball;
    uint64_t              untilMs = 0;
};
std::unordered_map<uint32_t, Pending> g_pending;
uint32_t g_nextReq = 0x80000000u;   // dig_sync counts its own requests up from 1: the two never meet

R::ParamOffset g_otherParam{L"OtherActor"};

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}

bool Pends(void* ball) {
    for (const auto& [id, p] : g_pending)
        if (p.ball.Get() == ball) return true;
    return false;
}

void RequestFill(void* hole, void* ball) {
    if (g_pending.size() >= kMaxPending || Pends(ball)) return;   // a bounce back into the box is the same dirt
    D::HoleState hs;
    if (!D::ReadHole(hole, hs)) return;
    W::Intent i;
    i.op = W::IntentOp::Fill;
    i.reqId = g_nextReq++;
    i.pose = W::Pose{hs.location.X, hs.location.Y, hs.location.Z, hs.rotation.Pitch, hs.rotation.Yaw, hs.rotation.Roll};
    Pending p;
    p.ball.Set(ball);
    p.untilMs = coop::net::NowMs() + kAckWaitMs;
    g_pending[i.reqId] = std::move(p);
    dig_out::Enqueue(-1, W::Pack(i));
    UE_LOGI("[DIG] client: fill #%u sent: a thrown dirt ball in the hole at (%.0f,%.0f,%.0f)", i.reqId,
            static_cast<double>(hs.location.X), static_cast<double>(hs.location.Y), static_cast<double>(hs.location.Z));
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

sg::Verdict OnHoleOverlapPre(const sg::Call& c) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return sg::Verdict::Run;
    const int32_t off = g_otherParam.Of(c.function);
    void* other = nullptr;
    if (off >= 0 && c.locals) std::memcpy(&other, c.locals + off, sizeof(other));
    if (other && D::IsDirtBall(other) && D::IsHole(c.object) && !coop::prop_echo_suppress::IsMirrorSpawn(other))
        RequestFill(c.object, other);
    return sg::Verdict::Cancel;
}

bool OnAck(uint32_t reqId, bool ok) {
    auto it = g_pending.find(reqId);
    if (it == g_pending.end()) return false;
    void* ball = it->second.ball.Get();
    g_pending.erase(it);
    if (!ok) {
        UE_LOGW("[DIG] client: the host refused fill #%u -- the dirt ball stays", reqId);
        return true;
    }
    if (ball && D::IsDirtBall(ball)) E::DestroyActor(ball);   // the game's overlap would have spent it with the hole
    UE_LOGI("[DIG] client: the host ran fill #%u -- the hole is filled and the dirt ball is spent", reqId);
    return true;
}

void Tick(uint64_t nowMs) {
    for (auto it = g_pending.begin(); it != g_pending.end();)
        it = (nowMs > it->second.untilMs) ? g_pending.erase(it) : std::next(it);
}

void Reset() {
    g_pending.clear();
    g_nextReq = 0x80000000u;
}

}  // namespace coop::dig_fill
