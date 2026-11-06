// coop/props/join_keyed_replay.cpp -- see join_keyed_replay.h.

#include "coop/props/join_keyed_replay.h"

#include "coop/player/players_registry.h"  // Registry::Local: the player whose grab owns a held prop
#include "coop/props/prop_drive_stream.h"  // IsParked
#include "coop/props/prop_throw.h"         // SeqFor
#include "coop/props/remote_prop.h"        // IsActorUnderAnyDrive
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine_mainplayer.h"  // IsMainPlayerGrabbing

#include <unordered_map>

namespace coop::join_keyed_replay {
namespace {
struct Retained {
    coop::remote_prop_spawn::HostPose pose;
    std::wstring key;
    std::wstring cls;
};
// Game thread only (the event_feed drain and the sweep), like the claim set it belongs to.
std::unordered_map<uint32_t, Retained> g_retained;
}  // namespace

void NoteSpawn(uint32_t eid, const coop::remote_prop_spawn::HostPose& pose, const std::wstring& keyW,
               const std::wstring& classW) {
    if (eid == 0) return;
    g_retained[eid] = Retained{pose, keyW, classW};
}

void NoteSnapPos(uint32_t eid, const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t physFlags) {
    const auto it = g_retained.find(eid);
    if (it == g_retained.end()) return;
    it->second.pose = coop::remote_prop_spawn::HostPose{loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll, physFlags, /*hostAuthored=*/true};
}

bool ReplayOnto(uint32_t eid, void* actor) {
    if (!actor) return false;
    const auto it = g_retained.find(eid);
    if (it == g_retained.end()) return false;
    const Retained& r = it->second;
    void* localPlayer = coop::players::Registry::Get().Local();
    if ((localPlayer && ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, actor)) ||
        coop::remote_prop::IsActorUnderAnyDrive(actor) || coop::prop_drive_stream::IsParked(actor) ||
        coop::prop_throw::SeqFor(actor) != 0) {
        UE_LOGI("join_keyed_replay: eid=%u key='%ls' re-create %p is held, driven or thrown -- its stream owns the pose, "
                "retained row not replayed", static_cast<unsigned>(eid), r.key.c_str(), actor);
        return false;
    }
    UE_LOGI("join_keyed_replay: eid=%u key='%ls' -- replaying the host's join-bracket pose (%.1f,%.1f,%.1f) flags=0x%02x "
            "onto the re-create %p", static_cast<unsigned>(eid), r.key.c_str(), r.pose.locX, r.pose.locY, r.pose.locZ,
            static_cast<unsigned>(r.pose.physFlags), actor);
    coop::remote_prop_spawn::ConvergeKeyedToHost(actor, r.pose, r.key, r.cls);
    return true;
}

void Clear() { g_retained.clear(); }

}  // namespace coop::join_keyed_replay
