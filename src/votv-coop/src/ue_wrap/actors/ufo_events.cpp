// ue_wrap/actors/ufo_events.cpp -- see ue_wrap/actors/ufo_events.h.

#include "ue_wrap/actors/ufo_events.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_timeline.h"

#include <cmath>
#include <cstdint>

namespace ue_wrap::ufo_events {
namespace {

namespace R  = reflection;
namespace P  = profile;
namespace TL = engine_timeline;

constexpr float kPi = 3.14159265358979f;
constexpr uint8_t kLocalSpace = 0;   // ESplineCoordinateSpace::Local

bool IsClass(void* actor, const wchar_t* name) {
    return actor && R::IsLive(actor) && R::NameEquals(R::NameOf(R::ClassOf(actor)), name);
}

void* ObjectMember(void* actor, const wchar_t* prop) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), prop);
    if (off < 0) return nullptr;
    void* o = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(actor) + off);
    return (o && R::IsLive(o)) ? o : nullptr;
}

template <typename T>
bool ValueMember(void* actor, const wchar_t* prop, T& out) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), prop);
    if (off < 0) return false;
    out = *reinterpret_cast<const T*>(static_cast<const uint8_t*>(actor) + off);
    return true;
}

bool WriteFloatMember(void* actor, const wchar_t* prop, float v) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), prop);
    if (off < 0) return false;
    *reinterpret_cast<float*>(static_cast<uint8_t*>(actor) + off) = v;
    return true;
}

// ---- a USplineComponent's verbs ----

bool SplineVector(void* spline, const wchar_t* fn, float time, FVector& out) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(spline), fn);
    ParamFrame frame(f);
    if (!f || !frame.valid()) return false;
    frame.Set<float>(L"Time", time);
    frame.Set<uint8_t>(L"CoordinateSpace", kLocalSpace);
    frame.Set<bool>(L"bUseConstantVelocity", true);
    return Call(spline, frame) && frame.GetRaw(L"ReturnValue", &out, sizeof(out));
}

bool SplinePointLocation(void* spline, int32_t index, FVector& out) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(spline), P::name::SplineLocationAtPointFn);
    ParamFrame frame(f);
    if (!f || !frame.valid()) return false;
    frame.Set<int32_t>(L"PointIndex", index);
    frame.Set<uint8_t>(L"CoordinateSpace", kLocalSpace);
    return Call(spline, frame) && frame.GetRaw(L"ReturnValue", &out, sizeof(out));
}

bool SplineClear(void* spline) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(spline), P::name::SplineClearPointsFn);
    ParamFrame frame(f);
    if (!f || !frame.valid()) return false;
    frame.Set<bool>(L"bUpdateSpline", false);
    return Call(spline, frame);
}

bool SplineAddPoint(void* spline, const FVector& p) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(spline), P::name::SplineAddPointFn);
    ParamFrame frame(f);
    if (!f || !frame.valid()) return false;
    frame.Set<FVector>(L"Position", p);
    frame.Set<uint8_t>(L"CoordinateSpace", kLocalSpace);
    frame.Set<bool>(L"bUpdateSpline", false);
    return Call(spline, frame);
}

bool SplineUpdate(void* spline) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(spline), P::name::SplineUpdateFn);
    ParamFrame frame(f);
    return f && frame.valid() && Call(spline, frame);
}

bool SplineLength(void* spline, float& out) {
    void* f = R::FindDispatchFunctionCached(R::ClassOf(spline), P::name::SplineLengthFn);
    ParamFrame frame(f);
    return f && frame.valid() && Call(spline, frame) && frame.GetRaw(L"ReturnValue", &out, sizeof(out));
}

// ---- vector math the course is built from ----

FVector Sub(const FVector& a, const FVector& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
FVector Add(const FVector& a, const FVector& b) { return {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; }
FVector Mul(const FVector& a, float s) { return {a.X * s, a.Y * s, a.Z * s}; }
float   Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
FVector Cross(const FVector& a, const FVector& b) {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

// The frame gen() builds point i from: the travel spline's place, right vector and forward axis (cross(right, up)).
struct Frame { FVector loc, right, axis; };

bool FrameAt(void* travel, float t, Frame& out) {
    FVector up{};
    if (!SplineVector(travel, P::name::SplineLocationAtTimeFn, t, out.loc) ||
        !SplineVector(travel, P::name::SplineRightAtTimeFn, t, out.right) ||
        !SplineVector(travel, P::name::SplineUpAtTimeFn, t, up))
        return false;
    out.axis = Cross(out.right, up);
    return true;
}

struct Vec2 { float x = 0.f, y = 0.f; };   // an FVector2D

struct Course {
    void*   travel = nullptr;
    void*   fly = nullptr;
    int32_t splinePoints = 0;
    float   radius = 0.f;
    float   sx = 1.f, sy = 1.f;   // `squish`, an FVector2D
};

bool ResolveCourse(void* actor, Course& c) {
    c.travel = ObjectMember(actor, P::name::UfoBoofoTravelSplineProp);
    c.fly = ObjectMember(actor, P::name::UfoBoofoFlySplineProp);
    Vec2 squish;
    int32_t n = 0;
    const bool ok = c.travel && c.fly && ValueMember(actor, P::name::UfoBoofoSplinePointsProp, n) &&
                    ValueMember(actor, P::name::UfoBoofoRadiusProp, c.radius) &&
                    ValueMember(actor, P::name::UfoBoofoSquishProp, squish);
    c.splinePoints = n;
    c.sx = squish.x;
    c.sy = squish.y;
    if (!ok || n < 1 || !std::isfinite(c.radius) || !std::isfinite(c.sx) || !std::isfinite(c.sy)) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("ufo_events: a boofo member did not resolve (travel=%p fly=%p splinepoints=%d radius=%f squish=%f,%f) "
                    "-- its course cannot cross", c.travel, c.fly, n, c.radius, c.sx, c.sy);
        }
        return false;
    }
    return true;
}

}  // namespace

bool IsBoofo(void* actor) { return IsClass(actor, P::name::UfoBoofoClass); }
bool IsJoel(void* actor) { return IsClass(actor, P::name::UfoJoelClass); }
bool IsPillfo(void* actor) { return IsClass(actor, P::name::UfoPillfoClass); }

bool BoofoRead(void* actor, BoofoSetup& out) {
    if (!IsBoofo(actor)) return false;
    Course c;
    if (!ResolveCourse(actor, c)) return false;
    const int32_t count = c.splinePoints + 1;
    if (count > kMaxBoofoPoints) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("ufo_events: a boofo course has %d points, more than the %d a birth blob holds -- it cannot cross",
                    count, kMaxBoofoPoints);
        }
        return false;
    }
    // The squish scales two axes of the pushed-out vector; a tiny factor leaves the angle unrecoverable from them.
    if (std::fabs(c.sx) < 1.0e-3f || std::fabs(c.sy) < 1.0e-3f) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("ufo_events: a boofo's squish (%f, %f) is too small to recover its angles", c.sx, c.sy);
        }
        return false;
    }
    float alpha = 0.f;
    if (!ValueMember(actor, P::name::UfoBoofoAlphaProp, alpha) || !std::isfinite(alpha)) return false;
    BoofoSetup s;
    s.alpha = alpha;
    s.count = static_cast<uint8_t>(count);
    for (int32_t i = 0; i < count; ++i) {
        Frame fr;
        FVector p{};
        if (!FrameAt(c.travel, static_cast<float>(i) / static_cast<float>(c.splinePoints), fr) ||
            !SplinePointLocation(c.fly, i, p))
            return false;
        // Undo gen(): the point minus the travel spline's, the squish divided out, then the angle in the plane of the
        // right vector and the axis-by-right vector the rotation turns it through.
        FVector v = Sub(p, fr.loc);
        v.Y /= c.sx;
        v.Z /= c.sy;
        const FVector e2 = Cross(fr.axis, fr.right);
        float deg = std::atan2(Dot(v, e2), Dot(v, fr.right)) * (180.f / kPi);
        if (!std::isfinite(deg)) return false;
        if (deg < 0.f) deg += 360.f;
        s.angle[i] = static_cast<uint8_t>(static_cast<int>(std::lround(deg * (256.f / 360.f))) & 0xFF);
    }
    out = s;
    return true;
}

bool BoofoApply(void* actor, const BoofoSetup& in) {
    if (!IsBoofo(actor)) return false;
    Course c;
    if (!ResolveCourse(actor, c)) return false;
    if (static_cast<int32_t>(in.count) != c.splinePoints + 1 || in.count > kMaxBoofoPoints) return false;
    bool ok = SplineClear(c.fly);
    for (int32_t i = 0; ok && i < in.count; ++i) {
        Frame fr;
        if (!FrameAt(c.travel, static_cast<float>(i) / static_cast<float>(c.splinePoints), fr)) return false;
        // gen(): RotateAngleAxis(right * radius, angle, axis) * (1, squish.x, squish.y), added to the travel point.
        const float rad = (static_cast<float>(in.angle[i]) * (360.f / 256.f)) * (kPi / 180.f);
        const FVector base = Mul(fr.right, c.radius);
        const float cs = std::cos(rad), sn = std::sin(rad);
        FVector rot = Add(Add(Mul(base, cs), Mul(Cross(fr.axis, base), sn)),
                          Mul(fr.axis, Dot(fr.axis, base) * (1.f - cs)));
        rot.Y *= c.sx;
        rot.Z *= c.sy;
        ok = SplineAddPoint(c.fly, Add(fr.loc, rot));
    }
    ok = ok && SplineUpdate(c.fly);
    float length = 0.f;
    if (ok && SplineLength(c.fly, length) && std::isfinite(length) && length > 0.f)
        ok = WriteFloatMember(actor, P::name::UfoBoofoLengthProp, length);
    else
        ok = false;
    return WriteFloatMember(actor, P::name::UfoBoofoAlphaProp, in.alpha) && ok;
}

void* TimelineOf(void* actor, uint8_t phase) {
    if (IsJoel(actor)) return phase == 0 ? ObjectMember(actor, P::name::UfoJoelMoveTimelineProp) : nullptr;
    if (IsPillfo(actor))
        return ObjectMember(actor, phase == 0 ? P::name::UfoPillfoTimelineAProp : P::name::UfoPillfoTimeline0Prop);
    return nullptr;
}

bool TimelineRead(void* actor, TimelineProgress& out) {
    const bool pill = IsPillfo(actor);
    if (!pill && !IsJoel(actor)) return false;
    out = TimelineProgress{};
    // A pillfo's later timeline first: it runs after `a` ended.
    for (int phase = pill ? 1 : 0; phase >= 0; --phase) {
        void* tl = TimelineOf(actor, static_cast<uint8_t>(phase));
        if (!tl) return false;
        TL::State st;
        if (!TL::Read(tl, st)) return false;
        if (st.playing) {
            out.phase = static_cast<uint8_t>(phase);
            out.position = st.position;
            return true;
        }
    }
    return true;   // none running
}

bool TimelineSeek(void* actor, uint8_t phase, float position) {
    if (IsPillfo(actor) && phase == 1) {
        void* a = TimelineOf(actor, 0);
        void* t0 = TimelineOf(actor, 1);
        if (!a || !t0) return false;
        TL::Stop(a);
        if (!TL::PlayFromStart(t0)) return false;
        return TL::SetPosition(t0, position);
    }
    void* tl = TimelineOf(actor, phase);
    return tl && TL::SetPosition(tl, position);
}

}  // namespace ue_wrap::ufo_events
