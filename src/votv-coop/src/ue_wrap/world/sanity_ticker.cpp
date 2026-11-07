// ue_wrap/world/sanity_ticker.cpp -- see ue_wrap/world/sanity_ticker.h. Read-only.

#include "ue_wrap/world/sanity_ticker.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ue_wrap::sanity_ticker {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

template <typename T>
T At(void* base, int32_t off) {
    T v{};
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

struct BoolRow { int32_t byte = -1; uint8_t mask = 0; };

bool ReadBool(void* obj, const BoolRow& r) {
    return r.byte >= 0 && r.mask != 0 && (At<uint8_t>(obj, r.byte) & r.mask) != 0;
}

// An offset resolved by name, the first miss said once per name.
int32_t Offset(void* cls, const wchar_t* name) {
    const int32_t off = R::FindPropertyOffset(cls, name);
    if (off < 0) UE_LOGW("sanity_ticker: %ls.%ls did not resolve", R::ToString(R::NameOf(cls)).c_str(), name);
    return off;
}

BoolRow Bool(void* cls, const wchar_t* name) {
    BoolRow r;
    if (!R::FindBoolProperty(cls, name, r.byte, r.mask) || r.mask == 0) {
        UE_LOGW("sanity_ticker: %ls.%ls (bool) did not resolve", R::ToString(R::NameOf(cls)).c_str(), name);
        r = BoolRow{};
    }
    return r;
}

// The gamemode's rows.
void* g_gmCls = nullptr;
int32_t g_gmTicker = -1;
BoolRow g_gmSleep;

// The ticker's rows.
void* g_tkCls = nullptr;
bool g_tkComplete = false;
int32_t g_sanity = -1, g_light = -1, g_lightLerp = -1, g_pl1 = -1, g_pl2 = -1, g_floor = -1, g_floorOrig = -1,
        g_dayMult = -1, g_drain = -1, g_drainLerp = -1, g_tier = -1, g_hold = -1;
BoolRow g_aboveLerp, g_shadow, g_ruleEnable, g_drainManual;

// The sampled body's row.
void* g_pawnCls = nullptr;
BoolRow g_pawnEnable;

void ResolveGamemode(void* cls) {
    if (cls == g_gmCls) return;
    g_gmCls = cls;
    g_gmTicker = Offset(cls, P::name::GamemodeSanityTickerProp);
    g_gmSleep = Bool(cls, P::name::GamemodeIsSleep);
}

void ResolveTicker(void* cls) {
    if (cls == g_tkCls) return;
    g_tkCls = cls;
    g_sanity = Offset(cls, P::name::SanityTickerSanity);
    g_light = Offset(cls, P::name::SanityTickerLightLevel);
    g_lightLerp = Offset(cls, P::name::SanityTickerLightLevelLerp);
    g_pl1 = Offset(cls, P::name::SanityTickerPlayerLight1);
    g_pl2 = Offset(cls, P::name::SanityTickerPlayerLight2);
    g_floor = Offset(cls, P::name::SanityTickerFloor);
    g_floorOrig = Offset(cls, P::name::SanityTickerFloorOriginal);
    g_dayMult = Offset(cls, P::name::SanityTickerDayMult);
    g_drain = Offset(cls, P::name::SanityTickerDrain);
    g_drainLerp = Offset(cls, P::name::SanityTickerDrainLerp);
    g_tier = Offset(cls, P::name::SanityTickerEventTier);
    g_hold = Offset(cls, P::name::SanityTickerPlayerHold);
    g_aboveLerp = Bool(cls, P::name::SanityTickerAboveFloorLerp);
    g_shadow = Bool(cls, P::name::SanityTickerShadowCast);
    g_ruleEnable = Bool(cls, P::name::SanityTickerRuleEnable);
    g_drainManual = Bool(cls, P::name::SanityTickerDrainManual);
    g_tkComplete = g_sanity >= 0 && g_light >= 0 && g_lightLerp >= 0 && g_pl1 >= 0 && g_pl2 >= 0 && g_floor >= 0 &&
                   g_floorOrig >= 0 && g_dayMult >= 0 && g_drain >= 0 && g_drainLerp >= 0 && g_tier >= 0 &&
                   g_hold >= 0;
}

void ResolvePawn(void* cls) {
    if (cls == g_pawnCls) return;
    g_pawnCls = cls;
    g_pawnEnable = Bool(cls, P::name::PawnEnableSanity);
}

// The three local-light classes and the attenuation radius on each.
struct LightClass {
    const wchar_t* name;
    void* cls = nullptr;
    int32_t radiusOff = -1;
};
LightClass g_lightClasses[] = {
    {P::name::PointLightComponentClass},
    {P::name::SpotLightComponentClass},
    {P::name::RectLightComponentClass},
};

struct CensusCtx {
    FVector at;
    int32_t radiusOff = -1;
    Census* out = nullptr;
    std::vector<Light>* all = nullptr;
};

void VisitLight(void* ctx, void* comp, int32_t /*index*/) {
    auto* c = static_cast<CensusCtx*>(ctx);
    ++c->out->scanned;
    FVector loc{};
    if (!E::TryGetComponentLocation(comp, loc)) return;
    const float dx = loc.X - c->at.X, dy = loc.Y - c->at.Y, dz = loc.Z - c->at.Z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float radius = c->radiusOff >= 0 ? At<float>(comp, c->radiusOff) : 0.f;
    if (radius <= 0.f || dist > radius) return;
    Light l;
    l.distance = dist;
    l.radius = radius;
    l.intensity = At<float>(comp, static_cast<int32_t>(P::off::ULightComponentBase_Intensity));
    const bool vis = (At<uint8_t>(comp, static_cast<int32_t>(P::off::USceneComponent_VisFlagsByte)) & 0x10) != 0;
    const bool hidden = (At<uint8_t>(comp, static_cast<int32_t>(P::off::USceneComponent_HiddenFlagsByte)) & 0x04) != 0;
    l.visible = vis && !hidden;
    l.kind = R::ClassNameOf(comp);
    void* owner = R::OuterOf(comp);
    l.owner = owner ? R::ClassNameOf(owner) : L"?";
    ++c->out->inReach;
    if (l.visible && l.intensity > 0.f) ++c->out->litInReach;
    c->all->push_back(std::move(l));
}

}  // namespace

bool Read(State& out) {
    out = State{};
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;
    ResolveGamemode(R::ClassOf(gm));
    if (g_gmTicker < 0) return false;
    void* tk = At<void*>(gm, g_gmTicker);
    if (!tk || !R::IsLive(tk)) return false;
    ResolveTicker(R::ClassOf(tk));
    if (!g_tkComplete) return false;
    out.ticker = tk;
    out.sanity = At<float>(tk, g_sanity);
    out.lightLevel = At<float>(tk, g_light);
    out.lightLevelLerp = At<float>(tk, g_lightLerp);
    out.playerLight1 = At<float>(tk, g_pl1);
    out.playerLight2 = At<float>(tk, g_pl2);
    out.floor = At<float>(tk, g_floor);
    out.floorOriginal = At<float>(tk, g_floorOrig);
    out.dayMult = At<float>(tk, g_dayMult);
    out.drain = At<float>(tk, g_drain);
    out.drainLerp = At<float>(tk, g_drainLerp);
    out.eventTier = At<int32_t>(tk, g_tier);
    out.aboveFloorLerp = ReadBool(tk, g_aboveLerp);
    out.shadowCast = ReadBool(tk, g_shadow);
    out.ruleEnable = ReadBool(tk, g_ruleEnable);
    out.drainManual = ReadBool(tk, g_drainManual);
    out.sleeping = ReadBool(gm, g_gmSleep);
    out.gamemodePlayer = At<void*>(gm, static_cast<int32_t>(P::off::mainGamemode_mainPlayer));
    out.playerHold = At<void*>(tk, g_hold);
    if (out.playerHold && R::IsLive(out.playerHold)) {
        ResolvePawn(R::ClassOf(out.playerHold));
        if (g_pawnEnable.mask != 0) {
            out.saveEnable = ReadBool(out.playerHold, g_pawnEnable);
            out.saveEnableRead = true;
        }
        out.flashlight = At<uint8_t>(out.playerHold, static_cast<int32_t>(P::off::AmainPlayer_flashlight)) != 0;
    }
    return true;
}

bool CensusNear(const FVector& at, size_t keep, Census& out) {
    out = Census{};
    std::vector<Light> all;
    bool any = false;
    for (LightClass& lc : g_lightClasses) {
        if (!lc.cls) {
            lc.cls = R::FindClass(lc.name);
            if (!lc.cls) continue;
            lc.radiusOff = R::FindPropertyOffset(lc.cls, P::name::LocalLightAttenuationRadiusProp);
        }
        any = true;
        CensusCtx ctx{at, lc.radiusOff, &out, &all};
        object_index::ForEachInstance(lc.cls, &VisitLight, &ctx);
    }
    std::sort(all.begin(), all.end(), [](const Light& a, const Light& b) { return a.distance < b.distance; });
    if (all.size() > keep) all.resize(keep);
    out.nearest = std::move(all);
    return any;
}

bool LocationOf(void* actor, FVector& out) {
    return actor && R::IsLive(actor) && E::TryGetActorLocation(actor, out);
}

}  // namespace ue_wrap::sanity_ticker
