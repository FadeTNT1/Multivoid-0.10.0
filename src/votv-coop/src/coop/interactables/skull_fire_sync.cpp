// coop/interactables/skull_fire_sync.cpp -- see coop/interactables/skull_fire_sync.h.

#include "coop/interactables/skull_fire_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/wend_skull.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::skull_fire_sync {
namespace {

namespace MS = coop::net::microwave_state_flags;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowIgnite, "skull_fire_sync", L"prop_wendskull_C", L"ignite", Declared);
constexpr int kTagIgnite = 0x534B4C31;   // 'SKL1'

constexpr size_t kMaxSeen = 16;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_registered = false;
bool g_settled = false;

// The skulls already sent (host) or already run (client): an ignite is called again while a lit skull still sits in the
// loop, and a repeat would only be a second cue.
std::vector<ue_wrap::CachedObjRef> g_seen;
uint64_t g_sent = 0, g_ran = 0, g_repeated = 0, g_missing = 0;
bool g_missingSaid = false;

bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }
bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}

// Remember `skull`; false when it was remembered already. Dead entries are dropped first.
bool NoteSeen(void* skull) {
    for (size_t i = 0; i < g_seen.size();) {
        if (!g_seen[i].Alive()) {
            g_seen.erase(g_seen.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (g_seen[i].Is(skull)) return false;
        ++i;
    }
    if (g_seen.size() >= kMaxSeen) g_seen.erase(g_seen.begin());
    ue_wrap::CachedObjRef r;
    r.Set(skull);
    g_seen.push_back(std::move(r));
    return true;
}

// HOST: a skull's ignite has started. The cue names the skull by its prop key.
void OnIgnitePost(const sg::Call& c) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || !c.object || !s->connected() || !s->AnyWorldReadyPeer()) return;
    const std::wstring key = ue_wrap::prop::GetKeyString(c.object);
    if (key.empty() || key == L"None" || key.size() > sizeof(coop::net::WireKey::data)) {
        UE_LOGW("[SKULL] host: a skull lit with no usable prop key -- the other players do not get its effect");
        return;
    }
    if (!NoteSeen(c.object)) return;
    coop::net::MicrowaveStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.flags = MS::kSkullFire;
    if (!s->SendReliable(coop::net::ReliableKind::MicrowaveState, &p, sizeof(p))) {
        UE_LOGW("[SKULL] host: the cue for skull key='%ls' was not sent", key.c_str());
        return;
    }
    ++g_sent;
    UE_LOGI("[SKULL] host: skull key='%ls' lit -- its effect sent to every client (#%llu)", key.c_str(),
            static_cast<unsigned long long>(g_sent));
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session || g_registered) return;
    g_registered = true;
    if (!sg::WatchName(kRowIgnite, kTagIgnite, nullptr, &OnIgnitePost))
        UE_LOGE("[SKULL] the script-body gate refused the watch on the skull's ignite -- only the host gets its effect");
}

void Tick() {
    if (g_settled || !g_registered) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowIgnite, kTagIgnite)) return;
    g_settled = true;
    if (sg::NameWatchLive(kRowIgnite, kTagIgnite))
        UE_LOGI("[SKULL] the watch on the skull's ignite is live");
    else
        UE_LOGE("[SKULL] the watch on the skull's ignite is dead -- only the host gets the skull's effect");
}

void OnCue(const coop::net::MicrowaveStatePayload& p) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* skull = coop::prop_key_index::Find(key);
    if (!skull || !ue_wrap::wend_skull::IsWendSkull(skull)) {
        ++g_missing;
        if (!g_missingSaid) {
            g_missingSaid = true;
            UE_LOGW("[SKULL] client: the host's skull key='%ls' has no copy here -- its effect is skipped (said once)",
                    key.c_str());
        }
        return;
    }
    if (!NoteSeen(skull)) {
        ++g_repeated;
        return;
    }
    if (!ue_wrap::wend_skull::Ignite(skull)) {
        UE_LOGW("[SKULL] client: the skull key='%ls' did not take ignite", key.c_str());
        return;
    }
    // The skull destroys itself when the effect ends; the host destroys its own and says so.
    coop::prop_echo_suppress::MarkIncomingDestroy(skull);
    ++g_ran;
    UE_LOGI("[SKULL] client: ran the skull key='%ls' effect against this player (#%llu)", key.c_str(),
            static_cast<unsigned long long>(g_ran));
}

void OnDisconnect() {
    if (g_sent || g_ran || g_repeated || g_missing)
        UE_LOGI("[SKULL] session end -- cues sent=%llu run=%llu repeated=%llu without a copy=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_repeated), static_cast<unsigned long long>(g_missing));
    g_seen.clear();
    g_sent = g_ran = g_repeated = g_missing = 0;
    g_missingSaid = false;
}

}  // namespace coop::skull_fire_sync
