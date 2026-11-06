// coop/props/vent_census.cpp -- see coop/props/vent_census.h.

#include "coop/props/vent_census.h"

#include "coop/element/mirror_defer.h"
#include "coop/element/registry.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::vent_census {
namespace {

namespace R = ue_wrap::reflection;
namespace WI = ue_wrap::world_identity;

// The second census, this long after the first.
constexpr std::chrono::seconds kSecondCensusAfter{30};
// A world with more covers than this logs the summary and the first kMaxLines of them.
constexpr size_t kMaxLines = 64;

enum class Phase : uint8_t { Waiting, FirstDone, SecondDone };

Phase g_phase = Phase::Waiting;
uint32_t g_worldGen = 0;  // WI::Generation() the schedule belongs to
std::chrono::steady_clock::time_point g_firstAt;

struct Collect {
    std::vector<void*> actors;
};

void OnInstance(void* ctx, void* obj, int32_t /*index*/) {
    if (!obj || !R::IsLive(obj)) return;
    // The class default object is listed with the instances; it is not a cover in the world.
    if (R::ToString(R::NameOf(obj)).rfind(L"Default__", 0) == 0) return;
    static_cast<Collect*>(ctx)->actors.push_back(obj);
}

const char* Flag(bool read, bool value) { return !read ? "?" : (value ? "1" : "0"); }

struct Row {
    std::wstring key;
    uint32_t eid = 0;
    bool isStatic = false;
    bool frozen = false;
    ue_wrap::prop::PropVisibility vis{};
    ue_wrap::FVector loc{};
    bool locRead = false;
    bool held = false;
};

void Emit(const char* when, bool isHost) {
    void* cls = R::FindClass(ue_wrap::profile::name::PropVentClass);
    Collect found;
    if (cls) ue_wrap::object_index::ForEachInstance(cls, &OnInstance, &found);

    int visible = 0, hidden = 0, held = 0, unread = 0;
    std::vector<Row> rows;
    rows.reserve(found.actors.size());
    for (void* a : found.actors) {
        Row r;
        r.key = ue_wrap::prop::GetKeyString(a);
        r.eid = coop::element::Registry::Get().EidForActor(a);
        r.isStatic = ue_wrap::prop::IsStatic(a);
        r.frozen = ue_wrap::prop::IsFrozen(a);
        ue_wrap::prop::ReadPropVisibility(a, r.vis);
        r.locRead = ue_wrap::engine::TryGetActorLocation(a, r.loc);
        coop::mirror_defer::HideInfo hi{};
        r.held = !isHost && r.eid != 0u && coop::mirror_defer::HideInfoOf(r.eid, hi);
        // A cover a player cannot see: the actor flag, the mesh's own flags, or the join sweep's hold.
        const bool unseen = r.held || (r.vis.actorHiddenRead && r.vis.actorHidden) ||
                            (r.vis.meshRead && (!r.vis.meshVisible || r.vis.meshHiddenInGame));
        if (!r.vis.actorHiddenRead && !r.vis.meshRead) ++unread;
        if (unseen) ++hidden; else ++visible;
        if (r.held) ++held;
        rows.push_back(std::move(r));
    }

    UE_LOGI("[VENT-CENSUS] %s role=%s world-gen=%u covers=%zu visible=%d hidden=%d held-by-join=%d unread=%d%s",
            when, isHost ? "host" : "client", WI::Generation(), rows.size(), visible, hidden, held, unread,
            cls ? "" : " (prop_vent_C class not resolved)");
    size_t shown = 0;
    for (const Row& r : rows) {
        if (++shown > kMaxLines) {
            UE_LOGI("[VENT-CENSUS]   ... +%zu more", rows.size() - kMaxLines);
            break;
        }
        UE_LOGI("[VENT-CENSUS]   key='%ls' eid=%u static=%d frozen=%d actorHidden=%s meshVisible=%s "
                "meshHiddenInGame=%s collision=%d loc=(%.0f,%.0f,%.0f) join=%s",
                r.key.c_str(), r.eid, r.isStatic ? 1 : 0, r.frozen ? 1 : 0,
                Flag(r.vis.actorHiddenRead, r.vis.actorHidden), Flag(r.vis.meshRead, r.vis.meshVisible),
                Flag(r.vis.meshRead, r.vis.meshHiddenInGame), r.vis.rootCollision,
                r.locRead ? r.loc.X : 0.f, r.locRead ? r.loc.Y : 0.f, r.locRead ? r.loc.Z : 0.f,
                isHost ? "n/a" : (r.held ? "held" : "-"));
    }
}

}  // namespace

void Tick(bool isHost, bool ready) {
    UE_ASSERT_GAME_THREAD("vent_census::Tick");
    const uint32_t gen = WI::Generation();
    if (gen != g_worldGen) {  // a new world: the schedule starts over
        g_worldGen = gen;
        g_phase = Phase::Waiting;
    }
    if (!ready || WI::CurrentWorldKind() != WI::WorldKind::Gameplay) return;  // never census a menu world
    if (g_phase == Phase::Waiting) {
        g_phase = Phase::FirstDone;
        g_firstAt = std::chrono::steady_clock::now();
        Emit("world-ready", isHost);
    } else if (g_phase == Phase::FirstDone &&
               std::chrono::steady_clock::now() - g_firstAt >= kSecondCensusAfter) {
        g_phase = Phase::SecondDone;
        Emit("+30s", isHost);
    }
}

}  // namespace coop::vent_census
