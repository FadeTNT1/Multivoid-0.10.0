// ue_wrap/actors/puppet_gesture.h -- short, restorable gestures on a puppet's visible body: an offset of the body mesh
// under the capsule, which leaves the actor's own transform alone, and a reach of the arms through the kerfur AnimBP's
// own ModifyBone nodes. Engine-wrapper layer: no gameplay, no network state. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>

namespace ue_wrap::puppet {

// The body mesh's place under the capsule (ACharacter::Mesh, the parent of the drawn mesh_playerVisible). Moving it
// moves what is drawn and no actor transform, so the pose stream and a sleeper's pose watch never see it.
struct BodyOffset {
    FVector loc{};
    FRotator rot{};
};
bool ReadBodyOffset(void* puppetActor, BodyOffset& out);
bool WriteBodyOffset(void* puppetActor, const BodyOffset& offset);

// The rotation of `deg` degrees about the unit `axis`, as an FRotator.
FRotator RotatorFromAxisAngle(const FVector& axis, float deg);
// `base` turned `deg` degrees about the unit `axis` of its parent's frame.
FRotator RotateAbout(const FRotator& base, const FVector& axis, float deg);

// A reach in progress: what each ModifyBone node held before it, so EndReachGesture puts it back. One entry per
// AnimInstance the puppet draws with; a node whose bone name does not match stays unclaimed.
struct ReachGesture {
    struct Node {
        size_t offset = 0;
        bool claimed = false;
        FRotator rotation{};
        uint8_t rotationMode = 0;
        uint8_t rotationSpace = 0;
    };
    struct Instance {
        void* anim = nullptr;
        int32_t animIdx = -1;
        Node upperArmR, upperArmL, belly;
    };
    Instance instances[2];
    bool active = false;
};

// Claim the arm and belly nodes of the puppet's AnimInstances. False when no kerfur AnimBP or no such node is found,
// and `out` then holds nothing to restore.
bool BeginReachGesture(void* puppetActor, ReachGesture& out);
// One frame of the reach: both upper arms turned `armDeg` about the unit world `armAxis`, carrying a hanging arm toward
// the target, and the belly tipped `bellyDeg` the same way toward it.
void DriveReachGesture(const ReachGesture& gesture, const FVector& armAxis, float armDeg, float bellyDeg);
// Put every claimed node back. An AnimInstance that has gone is skipped.
void EndReachGesture(ReachGesture& gesture);

}  // namespace ue_wrap::puppet
