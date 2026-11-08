// ue_wrap/actors/puppet_gesture.cpp -- see ue_wrap/actors/puppet_gesture.h.

#include "ue_wrap/actors/puppet_gesture.h"
#include "puppet_internal.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine_component.h"

#include <cmath>

namespace ue_wrap::puppet {
namespace {

namespace P = profile;
namespace R = reflection;
namespace E = engine;

constexpr float kDeg2Rad = 0.01745329252f;
constexpr float kRad2Deg = 57.2957795131f;
constexpr uint8_t kBoneModeAdditive = 2;   // EBoneModificationMode::BMM_Additive
constexpr uint8_t kBoneSpaceWorld = 0;     // EBoneControlSpace::BCS_WorldSpace

struct Quat {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

Quat Multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat FromAxisAngle(const FVector& axis, float deg) {
    const float half = deg * kDeg2Rad * 0.5f;
    const float s = std::sin(half);
    return {axis.X * s, axis.Y * s, axis.Z * s, std::cos(half)};
}

// FRotator::Quaternion.
Quat FromRotator(const FRotator& r) {
    const float sp = std::sin(r.Pitch * kDeg2Rad * 0.5f), cp = std::cos(r.Pitch * kDeg2Rad * 0.5f);
    const float sy = std::sin(r.Yaw * kDeg2Rad * 0.5f), cy = std::cos(r.Yaw * kDeg2Rad * 0.5f);
    const float sr = std::sin(r.Roll * kDeg2Rad * 0.5f), cr = std::cos(r.Roll * kDeg2Rad * 0.5f);
    return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy};
}

// FQuat::Rotator.
FRotator ToRotator(const Quat& q) {
    const float singularity = q.z * q.x - q.w * q.y;
    const float yawY = 2.f * (q.w * q.z + q.x * q.y);
    const float yawX = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    constexpr float kThreshold = 0.4999995f;
    FRotator out{};
    out.Yaw = std::atan2(yawY, yawX) * kRad2Deg;
    if (singularity < -kThreshold) {
        out.Pitch = -90.f;
        out.Roll = NormalizeAxis(-out.Yaw - 2.f * std::atan2(q.x, q.w) * kRad2Deg);
    } else if (singularity > kThreshold) {
        out.Pitch = 90.f;
        out.Roll = NormalizeAxis(out.Yaw - 2.f * std::atan2(q.x, q.w) * kRad2Deg);
    } else {
        out.Pitch = std::asin(2.f * singularity) * kRad2Deg;
        out.Roll = std::atan2(-2.f * (q.w * q.x + q.y * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y)) * kRad2Deg;
    }
    return out;
}

// The animation-instance class the node offsets in the profile were measured on.
bool IsRegularKerfurAnim(void* anim) {
    if (!anim || !R::IsLive(anim)) return false;
    static void* sClass = nullptr;
    if (!sClass) sClass = R::FindClass(P::name::AnimBPKerfurRegularClass);
    return sClass && R::ClassOf(anim) == sClass;
}

uint8_t* NodeAt(void* anim, size_t offset) { return reinterpret_cast<uint8_t*>(anim) + offset; }

// Take one node when it modifies `bone`: its rotation inputs saved, then additive in world space with no turn yet.
void ClaimNode(void* anim, size_t offset, const wchar_t* bone, ReachGesture::Node& n) {
    uint8_t* const node = NodeAt(anim, offset);
    if (!R::NameEquals(*reinterpret_cast<R::FName*>(node + P::anim::LookAtMod_BoneToModify), bone)) return;
    n.offset = offset;
    n.claimed = true;
    n.rotation = *reinterpret_cast<FRotator*>(node + P::anim::ModBone_Rotation);
    n.rotationMode = node[P::anim::ModBone_RotationMode];
    n.rotationSpace = node[P::anim::ModBone_RotationSpace];
    *reinterpret_cast<FRotator*>(node + P::anim::ModBone_Rotation) = FRotator{};
    node[P::anim::ModBone_RotationMode] = kBoneModeAdditive;
    node[P::anim::ModBone_RotationSpace] = kBoneSpaceWorld;
}

void ReleaseNode(void* anim, const ReachGesture::Node& n) {
    if (!n.claimed) return;
    uint8_t* const node = NodeAt(anim, n.offset);
    *reinterpret_cast<FRotator*>(node + P::anim::ModBone_Rotation) = n.rotation;
    node[P::anim::ModBone_RotationMode] = n.rotationMode;
    node[P::anim::ModBone_RotationSpace] = n.rotationSpace;
}

void DriveNode(void* anim, const ReachGesture::Node& n, const FRotator& rot) {
    if (n.claimed) *reinterpret_cast<FRotator*>(NodeAt(anim, n.offset) + P::anim::ModBone_Rotation) = rot;
}

void* BodyMesh(void* puppetActor) {
    if (!puppetActor || !R::IsLive(puppetActor)) return nullptr;
    void* const mesh = GetNativeBodyMeshComponent(puppetActor);
    return mesh && R::IsLive(mesh) ? mesh : nullptr;
}

}  // namespace

bool ReadBodyOffset(void* puppetActor, BodyOffset& out) {
    void* const mesh = BodyMesh(puppetActor);
    if (!mesh) return false;
    out.loc = ReadAt<FVector>(mesh, P::off::USceneComponent_RelativeLocation);
    out.rot = ReadAt<FRotator>(mesh, P::off::USceneComponent_RelativeRotation);
    return true;
}

bool WriteBodyOffset(void* puppetActor, const BodyOffset& offset) {
    void* const mesh = BodyMesh(puppetActor);
    if (!mesh) return false;
    const bool loc = E::SetComponentRelativeLocation(mesh, offset.loc);
    const bool rot = E::SetComponentRelativeRotation(mesh, offset.rot);
    return loc && rot;
}

FRotator RotatorFromAxisAngle(const FVector& axis, float deg) { return ToRotator(FromAxisAngle(axis, deg)); }

FRotator RotateAbout(const FRotator& base, const FVector& axis, float deg) {
    return ToRotator(Multiply(FromAxisAngle(axis, deg), FromRotator(base)));
}

bool BeginReachGesture(void* puppetActor, ReachGesture& out) {
    out = ReachGesture{};
    if (!puppetActor || !R::IsLive(puppetActor)) return false;
    // Both bodies a player puppet draws with carry an AnimInstance of their own, as the head look writes them.
    void* const meshes[2] = {GetMeshPlayerVisibleComponent(puppetActor), GetNativeBodyMeshComponent(puppetActor)};
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        void* const anim = meshes[i] && R::IsLive(meshes[i]) ? LiveAnimInstance(meshes[i]) : nullptr;
        if (!IsRegularKerfurAnim(anim)) continue;
        ReachGesture::Instance& inst = out.instances[i];
        inst.anim = anim;
        inst.animIdx = R::InternalIndexOf(anim);
        ClaimNode(anim, P::anim::kKerfurModifyBone_4, P::name::BoneUpperArmR, inst.upperArmR);
        ClaimNode(anim, P::anim::kKerfurModifyBone_3, P::name::BoneUpperArmL, inst.upperArmL);
        ClaimNode(anim, P::anim::kKerfurModifyBone_5, P::name::BoneBelly, inst.belly);
        any = any || inst.upperArmR.claimed || inst.upperArmL.claimed || inst.belly.claimed;
    }
    out.active = any;
    return any;
}

void DriveReachGesture(const ReachGesture& gesture, const FVector& armAxis, float armDeg, float bellyDeg) {
    if (!gesture.active) return;
    const FRotator arm = RotatorFromAxisAngle(armAxis, armDeg);
    const FRotator belly = RotatorFromAxisAngle(armAxis, -bellyDeg);   // the spine points up: it tips the opposite way
    for (const ReachGesture::Instance& inst : gesture.instances) {
        if (!inst.anim || !R::IsLiveByIndex(inst.anim, inst.animIdx)) continue;
        DriveNode(inst.anim, inst.upperArmR, arm);
        DriveNode(inst.anim, inst.upperArmL, arm);
        DriveNode(inst.anim, inst.belly, belly);
    }
}

void EndReachGesture(ReachGesture& gesture) {
    for (const ReachGesture::Instance& inst : gesture.instances) {
        if (!inst.anim || !R::IsLiveByIndex(inst.anim, inst.animIdx)) continue;
        ReleaseNode(inst.anim, inst.upperArmR);
        ReleaseNode(inst.anim, inst.upperArmL);
        ReleaseNode(inst.anim, inst.belly);
    }
    gesture = ReachGesture{};
}

}  // namespace ue_wrap::puppet
