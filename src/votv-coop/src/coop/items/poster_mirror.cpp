// coop/items/poster_mirror.cpp -- a client's placed posters, from the host's PosterState. See coop/items/poster_sync.h.

#include "poster_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/actors/poster.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace coop::poster_sync::detail {
namespace {

namespace E  = ue_wrap::engine;
namespace SO = coop::net::poster_state_ops;
namespace SF = coop::net::poster_state_flags;
namespace PO = ue_wrap::poster;
namespace R  = ue_wrap::reflection;

constexpr uint64_t kParkMs = 30000;

MirrorCounts g_counts;

uint64_t NowMs() { return coop::net::NowMs(); }

std::unordered_map<std::wstring, ue_wrap::CachedObjRef> g_client;   // key -> the placed poster here
struct Parked {
    coop::net::PosterStatePayload p;
    uint64_t                      sinceMs;
};
std::unordered_map<std::wstring, Parked> g_parked;   // a state whose class is not loaded here yet
std::unordered_set<std::wstring> g_snapSeen;

void* ClientPlaced(const std::wstring& key) {
    auto it = g_client.find(key);
    if (it != g_client.end()) {
        if (void* w = it->second.Get()) return w;
        g_client.erase(it);
    }
    for (void* w : PO::AllPlaced())
        if (PO::KeyOf(w) == key) {
            g_client[key].Set(w);
            return w;
        }
    return nullptr;
}

bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// Land a placed state. False when its class is not loaded here yet (park it).
bool ApplyPlaced(const coop::net::PosterStatePayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    const std::wstring clsName = coop::net::StringFromWireKey(p.cls);
    void* cls = ue_wrap::object_index::ClassByName(clsName.c_str());
    if (!cls) return false;
    if (!PO::IsPlacedClass(cls)) {
        ++g_counts.dropped;
        UE_LOGW("[POSTER] client: key='%ls' names class '%ls', which is no placed poster -- dropped", key.c_str(),
                clsName.c_str());
        return true;
    }
    PO::Look look;
    look.type = p.type;
    look.index = p.index;
    look.custom = (p.flags & SF::kCustom) != 0;
    if (!FileNameFromWire(p.fileName, look.fileName)) return true;
    PO::Placement at;
    at.location = {p.locX, p.locY, p.locZ};
    at.rotation = {p.pitch, p.yaw, p.roll};
    at.scale = {p.scaleX, p.scaleY, p.scaleZ};

    void* w = ClientPlaced(key);
    if (w && R::ClassOf(w) != cls) {
        E::DestroyActor(w);
        g_client.erase(key);
        w = nullptr;
    }
    bool fell = false;
    if (!w) {
        w = PO::SpawnPlaced(cls, at, look, true, key);
        if (!w) {
            UE_LOGW("[POSTER] client: key='%ls' did not spawn here", key.c_str());
            return true;
        }
        g_client[key].Set(w);
        ++g_counts.spawned;
        PO::Look now;
        if (PO::ReadLook(w, now) && now.custom) PO::Redraw(w, &fell);
        UE_LOGI("[POSTER] client: key='%ls' placed here as %ls type=%d file='%ls'", key.c_str(), clsName.c_str(),
                look.type, look.fileName.c_str());
    } else {
        PO::Placement cur;
        if (PO::ReadTransform(w, cur)) {
            if (!Near(cur.location.X, at.location.X, 0.5f) || !Near(cur.location.Y, at.location.Y, 0.5f) ||
                !Near(cur.location.Z, at.location.Z, 0.5f))
                E::SetActorLocation(w, at.location);
            if (!Near(cur.rotation.Pitch, at.rotation.Pitch, 0.1f) || !Near(cur.rotation.Yaw, at.rotation.Yaw, 0.1f) ||
                !Near(cur.rotation.Roll, at.rotation.Roll, 0.1f))
                E::SetActorRotation(w, at.rotation);
            if (!Near(cur.scale.X, at.scale.X, 0.001f) || !Near(cur.scale.Y, at.scale.Y, 0.001f) ||
                !Near(cur.scale.Z, at.scale.Z, 0.001f))
                E::SetActorScale3D(w, at.scale);
        }
        PO::Look cur2;
        if (!PO::ReadLook(w, cur2) || cur2 != look) {
            PO::WriteLook(w, look);
            PO::Redraw(w, &fell);
        }
    }
    if (fell) SayFallback(key, look.fileName);
    ++g_counts.applied;
    return true;
}

void ApplyRemoved(const std::wstring& key) {
    g_parked.erase(key);
    if (void* w = ClientPlaced(key)) {
        E::DestroyActor(w);
        ++g_counts.destroyed;
        UE_LOGI("[POSTER] client: key='%ls' removed here", key.c_str());
    }
    g_client.erase(key);
}

void ApplySnapshotEnd() {
    int pruned = 0;
    for (void* w : PO::AllPlaced()) {
        const std::wstring key = PO::KeyOf(w);
        if (!key.empty() && (g_snapSeen.count(key) || g_parked.count(key))) continue;
        E::DestroyActor(w);
        if (!key.empty()) g_client.erase(key);
        ++pruned;
    }
    UE_LOGI("[POSTER] client: the host's snapshot named %zu placed poster(s); %d here it did not were removed",
            g_snapSeen.size(), pruned);
    g_counts.destroyed += static_cast<uint64_t>(pruned);
    g_snapSeen.clear();
}

void RetryParked() {
    if (g_parked.empty()) return;
    const uint64_t now = NowMs();
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (ApplyPlaced(it->second.p)) {
            it = g_parked.erase(it);
            continue;
        }
        if (now - it->second.sinceMs > kParkMs) {
            UE_LOGW("[POSTER] client: key='%ls' waited %llu s for class '%ls' to load -- dropped", it->first.c_str(),
                    static_cast<unsigned long long>(kParkMs / 1000),
                    coop::net::StringFromWireKey(it->second.p.cls).c_str());
            it = g_parked.erase(it);
            continue;
        }
        ++it;
    }
}

}  // namespace

void MirrorState(const coop::net::PosterStatePayload& p) {
    if (p.op == SO::kSnapshotEnd) {
        ApplySnapshotEnd();
        return;
    }
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (p.op == SO::kRemoved) {
        ApplyRemoved(key);
        return;
    }
    if (p.flags & SF::kSnapshot) g_snapSeen.insert(key);
    if (ApplyPlaced(p)) {
        g_parked.erase(key);
        return;
    }
    g_parked[key] = Parked{p, NowMs()};
}

void MirrorTick() { RetryParked(); }

MirrorCounts Counts() { return g_counts; }

void MirrorReset() {
    g_client.clear();
    g_parked.clear();
    g_snapSeen.clear();
    g_counts = MirrorCounts{};
}

}  // namespace coop::poster_sync::detail
