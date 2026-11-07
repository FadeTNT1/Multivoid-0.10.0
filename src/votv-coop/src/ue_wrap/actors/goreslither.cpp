// ue_wrap/actors/goreslither.cpp -- see ue_wrap/actors/goreslither.h.

#include "ue_wrap/actors/goreslither.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>

namespace ue_wrap::goreslither {
namespace {

namespace P = profile;
namespace R = reflection;

R::InstanceOffset g_springArm{L"springArm"};
R::InstanceOffset g_timeAnim{L"timeAnim"};

// EAttachmentRule::KeepRelative, which BeginPlay passes for all three rules.
constexpr uint8_t kKeepRelative = 0;

// A live object in the pointer slot `off` bytes into `base`, or null.
void* LiveAt(void* base, size_t off) {
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

// Says a step failed, once per step for this slither's class (keyed by the class's name, which a world change does
// not move). False, so a step's failure reads `ok = Failed(...)`.
bool Failed(void* slither, const char* step) {
    struct Seen {
        R::FName    cls;
        const char* step;
    };
    static Seen seen[8];
    static int count = 0;
    const R::FName& name = R::NameOf(R::ClassOf(slither));
    for (int i = 0; i < count; ++i)
        if (seen[i].cls.ComparisonIndex == name.ComparisonIndex && seen[i].cls.Number == name.Number &&
            std::strcmp(seen[i].step, step) == 0)
            return false;
    if (count < 8) seen[count++] = Seen{name, step};
    UE_LOGW("goreslither: %ls mirror presentation step '%s' did not run", R::ClassNameOf(slither).c_str(), step);
    return false;
}

// Mesh->K2_AttachToComponent(springArm, None, KeepRelative x3, true).
bool AttachMesh(void* slither, void* mesh) {
    const int32_t off = g_springArm.Of(slither);
    void* arm = off >= 0 ? LiveAt(slither, static_cast<size_t>(off)) : nullptr;
    void* fn = arm ? R::FindDispatchFunctionCached(R::ClassOf(mesh), L"K2_AttachToComponent") : nullptr;
    ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    const R::FName none{};
    f.Set<void*>(L"Parent", arm);
    f.SetRaw(L"SocketName", &none, sizeof(none));
    f.Set<uint8_t>(L"LocationRule", kKeepRelative);
    f.Set<uint8_t>(L"RotationRule", kKeepRelative);
    f.Set<uint8_t>(L"ScaleRule", kKeepRelative);
    f.Set<bool>(L"bWeldSimulatedBodies", true);
    return Call(mesh, f);
}

// timeAnim = RandomFloat(): the phase of the slither's crawl, so a group does not move in step.
bool WriteTimeAnim(void* slither) {
    const int32_t off = g_timeAnim.Of(slither);
    if (off < 0) return false;
    static std::minstd_rand rng{std::random_device{}()};
    const float phase = std::uniform_real_distribution<float>(0.f, 1.f)(rng);
    std::memcpy(static_cast<uint8_t*>(slither) + off, &phase, sizeof(phase));
    return true;
}

}  // namespace

bool ApplyBeginPlayPresentation(void* slither) {
    if (!slither || !R::IsLive(slither)) return false;
    bool ok = true;
    if (!WriteTimeAnim(slither)) ok = Failed(slither, "timeAnim");
    void* mesh = LiveAt(slither, P::off::ACharacter_Mesh);
    if (!mesh || !AttachMesh(slither, mesh)) ok = Failed(slither, "springArm attach");
    return ok;
}

}  // namespace ue_wrap::goreslither
