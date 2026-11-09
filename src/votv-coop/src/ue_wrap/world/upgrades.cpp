// ue_wrap/world/upgrades.cpp -- see ue_wrap/world/upgrades.h.

#include "ue_wrap/world/upgrades.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/space_renderer.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <limits>
#include <string>

namespace ue_wrap::upgrades {
namespace {

namespace OI = ue_wrap::object_index;
namespace R  = ue_wrap::reflection;

// The table. One row per int member of Fstruct_upgrades, in the order the wire carries them.
//
// `prefix` resolves the member: a UserDefinedStruct field renders as "upg_downloadSpd_3_<GUID>"
// and the GUID is re-minted by a recook, so only the human head is stable. The trailing
// underscore matters -- "upg_scanner_" must not match "upg_scannerFr_...".
struct Row {
    const wchar_t* prefix;
    int            panelIndex;  // -1 = no panel row
    int32_t        price;
    int32_t        accum;
    int32_t        maxLevel;
};

// `panelIndex` is the `index` field of the ui_laptop row that buys this level, and price,
// accumulation and maxLevel are that row's own fields. Read statically out of the ui_laptop
// exports rather than off a live widget, because the widget only exists while the laptop UI is up
// and the HOST validating a client's purchase has no reason to have it open. The arithmetic they
// feed is uicomp_upgradeSlot's own, transcribed from its bytecode:
//   buy  when points >= price and level < maxLevel; charges price(level), level += 1
//   sell when level > 0;                            credits refund, level -= 1
// The buy test really does read the row's unaccumulated `price` while the charge is getPrice(),
// which accumulates -- nothing writes that member at runtime, and nothing clamps the balance, so
// from the second level up the game lets a purchase take it below zero.
//
// Three members have no row: downloadFiltSize, serverStability and transofrmer. The rows' switch names
// them but no row carries their index, so in play only the cheat menu and the save's defaults write
// them; the physical upgrades write a server box's and the transformer's own counts. They still ride
// the mirror, which mirrors the struct rather than a purchase.
constexpr Row kRows[kLevelCount] = {
    { L"upg_downloadSpd_",        0, 20,  4, 16 },
    { L"upg_downloadFiltSize_",  -1,  0,  0,  0 },
    { L"upg_serverStability_",   -1,  0,  0,  0 },
    { L"upg_processSpeed_",       3, 20,  4, 16 },
    { L"upg_coordDrift_",         4, 20,  2, 16 },
    { L"upg_coordPingSpeed_",     5, 10,  4, 16 },
    { L"upg_coordMovementSpeed_", 6,  5,  4, 16 },
    { L"upg_processLvl_",         7, 30, 10,  3 },
    { L"upg_coordRadarSpeed_",    8, 20,  2, 16 },
    { L"upg_coordCooldown_",      9, 15,  4, 16 },
    { L"upg_scanner_",           10, 20,  4, 16 },
    { L"upg_scannerFr_",         11, 15,  4, 16 },
    { L"upg_detecQual_",         20,  5,  1, 16 },
    { L"upg_radarHist_",         21, 30,  4,  4 },
    { L"upg_radar_speed_",       24, 15,  4, 16 },
    { L"upg_transofrmer_",       -1,  0,  0,  0 },
    { L"upg_compTime_",          26, 10,  2, 16 },
    { L"upg_triangleProb_",      27, 10,  2, 16 },
};

const wchar_t* const kRowClass = L"uicomp_upgradeSlot_C";
const wchar_t* const kRowRefresh = L"upd";

// Cached offsets. The saveSlot class and the struct type never change at runtime, so each is
// walked once; -1 = unresolved, and a re-resolve is retried on the next call.
int32_t g_offUpgrades = -1;               // saveSlot.upgrades, within the saveSlot
int32_t g_offMember[kLevelCount];         // each level, within the struct
bool    g_membersResolved = false;
bool    g_saidMissing = false;

// The base of the live upgrades struct, or null while unresolvable.
uint8_t* ResolveStruct() {
    void* save = ue_wrap::economy::SaveSlotPtr();
    if (!save) return nullptr;
    void* cls = R::ClassOf(save);
    if (!cls) return nullptr;
    if (g_offUpgrades < 0) g_offUpgrades = R::FindPropertyOffset(cls, L"upgrades");
    if (g_offUpgrades < 0) return nullptr;
    if (!g_membersResolved) {
        void* inner = R::PropertyInnerStruct(cls, L"upgrades");
        if (!inner) return nullptr;
        int resolved = 0;
        for (int i = 0; i < kLevelCount; ++i) {
            g_offMember[i] = R::FindPropertyOffsetByPrefix(inner, kRows[i].prefix);
            if (g_offMember[i] >= 0) ++resolved;
        }
        if (resolved != kLevelCount) {
            // A partial resolve is a recook that renamed or dropped a member. Serve nothing
            // rather than a struct with holes: a hole would mirror as a zero and wipe a level.
            if (!g_saidMissing) {
                g_saidMissing = true;
                for (int i = 0; i < kLevelCount; ++i)
                    if (g_offMember[i] < 0)
                        UE_LOGW("upgrades: member '%ls' did not resolve on Fstruct_upgrades",
                                kRows[i].prefix);
            }
            return nullptr;
        }
        g_membersResolved = true;
    }
    return reinterpret_cast<uint8_t*>(save) + g_offUpgrades;
}

int RowForPanelIndex(int panelIndex) {
    for (int i = 0; i < kLevelCount; ++i)
        if (kRows[i].panelIndex == panelIndex) return i;
    return -1;
}

}  // namespace

bool ReadLevels(int32_t* out) {
    if (!out) return false;
    uint8_t* base = ResolveStruct();
    if (!base) return false;
    for (int i = 0; i < kLevelCount; ++i)
        out[i] = *reinterpret_cast<int32_t*>(base + g_offMember[i]);
    return true;
}

bool WriteLevels(const int32_t* in) {
    if (!in) return false;
    uint8_t* base = ResolveStruct();
    if (!base) return false;
    for (int i = 0; i < kLevelCount; ++i)
        *reinterpret_cast<int32_t*>(base + g_offMember[i]) = in[i];
    return true;
}

bool ReadLevel(int panelIndex, int32_t* out) {
    const int row = RowForPanelIndex(panelIndex);
    if (row < 0 || !out) return false;
    uint8_t* base = ResolveStruct();
    if (!base) return false;
    *out = *reinterpret_cast<int32_t*>(base + g_offMember[row]);
    return true;
}

bool WriteLevel(int panelIndex, int32_t value) {
    const int row = RowForPanelIndex(panelIndex);
    if (row < 0) return false;
    uint8_t* base = ResolveStruct();
    if (!base) return false;
    *reinterpret_cast<int32_t*>(base + g_offMember[row]) = value;
    return true;
}

bool IsLevelRow(int panelIndex) { return RowForPanelIndex(panelIndex) >= 0; }

int32_t PriceAtLevel(int panelIndex, int32_t level) {
    const int row = RowForPanelIndex(panelIndex);
    if (row < 0) return 0;
    const int32_t steps = level > 1 ? level - 1 : 0;
    return kRows[row].price + kRows[row].accum * steps;
}

int32_t BasePrice(int panelIndex) {
    const int row = RowForPanelIndex(panelIndex);
    return row < 0 ? 0 : kRows[row].price;
}

int32_t RefundAtLevel(int panelIndex, int32_t level) {
    const int row = RowForPanelIndex(panelIndex);
    if (row < 0) return 0;
    // The sell button's own formula, floor included: it reads the price at the level being sold,
    // takes three quarters, then shaves one -- so selling a level back never returns what it cost
    // except at the bottom of an accumulating row.
    const int32_t priced = PriceAtLevel(panelIndex, level);
    const int32_t three_quarters =
        static_cast<int32_t>(std::floor(static_cast<double>(priced) * 0.75));
    const int32_t less_one = three_quarters - 1;
    return less_one > 1 ? less_one : 1;
}

int32_t MaxLevel(int panelIndex) {
    const int row = RowForPanelIndex(panelIndex);
    return row < 0 ? 0 : kRows[row].maxLevel;
}

bool ApplyUpgradedHook() {
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(gm), profile::name::UpgradedHookFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(gm, f);
}

// ---- the hook's consumers and what it derives ---------------------------------------------------------------------

namespace {

R::InstanceOffset g_offDeskGamemode{profile::name::DeskGamemodeProp};
R::InstanceOffset g_offMasterSr{profile::name::GamemodeMasterSpaceRendererProp};
R::InstanceOffset g_offDetectorQuality{profile::name::DeskUpgradeDetectorQualityProp};
R::InstanceOffset g_offMovementSpeed{profile::name::SpaceRendererMovementSpeedProp};
R::InstanceOffset g_offDrift{profile::name::SpaceRendererCoordinateDriftProp};
R::InstanceOffset g_offTriangle{profile::name::SpaceRendererTriangleLevelProp};
R::InstanceOffset g_offRadarsSpeed{profile::name::CoordsPanelRadarsSpeedProp};

// Wire-order rows the formulas read (the table above).
enum : int { kRowCoordDrift = 4, kRowCoordMovementSpeed = 6, kRowCoordRadarSpeed = 8, kRowCoordCooldown = 9,
             kRowDetecQual = 12, kRowTriangleProb = 17 };

void* LiveRef(void* base, R::InstanceOffset& member) {
    if (!base) return nullptr;
    const int32_t off = member.Of(base);
    if (off < 0) return nullptr;
    void* p = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

float ReadFloat(void* base, R::InstanceOffset& member) {
    if (!base) return std::numeric_limits<float>::quiet_NaN();
    const int32_t off = member.Of(base);
    if (off < 0) return std::numeric_limits<float>::quiet_NaN();
    return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + off);
}

// The game's curve: (level / 16) ^ 2, the Blueprint's MultiplyMultiply_FloatFloat.
float Curve(int32_t level) {
    const float t = static_cast<float>(level) / 16.f;
    return t * t;
}
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

void ReadConsumers(Consumers& out) {
    out = Consumers{};
    void* gm = world_singleton::Gamemode();
    if (!gm) return;
    out.desk = ue_wrap::console_desk::Instance();
    out.deskGamemode = LiveRef(out.desk, g_offDeskGamemode);
    out.deskReadsOurGamemode = out.deskGamemode && out.deskGamemode == gm;
    out.atlas = ue_wrap::console_desk::AtlasWidget();
    void* ui = ue_wrap::console_desk::AtlasUiCoordsSlot();
    out.uiCoords = (ui && R::IsLive(ui)) ? ui : nullptr;
    ue_wrap::space_renderer::EnsureResolved();
    out.spaceRenderer = ue_wrap::space_renderer::Instance();
    out.masterSpaceRenderer = LiveRef(gm, g_offMasterSr);
}

bool ConsumersReady(const Consumers& c, const char** missing) {
    const char* gone = nullptr;
    if (!c.desk) gone = "desk";
    else if (!c.atlas) gone = "desk widget (atlas)";
    else if (!c.uiCoords) gone = "coordinates panel";
    else if (!c.spaceRenderer) gone = "spaceRenderer";
    else if (!c.masterSpaceRenderer) gone = "gamemode.master_spacerenderer";
    if (missing) *missing = gone;
    return gone == nullptr;
}

void ReadDerived(Derived& out) {
    Consumers c;
    ReadConsumers(c);
    out.detectorQuality = ReadFloat(c.desk, g_offDetectorQuality);
    float cd = 0.f;
    out.maxCooldown = ue_wrap::console_desk::ReadMaxCooldown(cd) ? cd : std::numeric_limits<float>::quiet_NaN();
    out.movementSpeed = ReadFloat(c.spaceRenderer, g_offMovementSpeed);
    out.coordinateDrift = ReadFloat(c.spaceRenderer, g_offDrift);
    out.coordRadarsSpeed = ReadFloat(c.uiCoords, g_offRadarsSpeed);
    out.triangleLevel = ReadFloat(c.masterSpaceRenderer, g_offTriangle);
}

void ExpectedDerived(const int32_t* l, Derived& out) {
    out.detectorQuality = Curve(l[kRowDetecQual]);
    out.maxCooldown = Lerp(30.f, 5.f, Curve(l[kRowCoordCooldown]));
    out.movementSpeed = Lerp(2.f, 4.f, Curve(l[kRowCoordMovementSpeed]));
    out.coordinateDrift = Lerp(0.4f, 4.f, Curve(l[kRowCoordDrift]));
    out.coordRadarsSpeed = Lerp(600.f, 1200.f, Curve(l[kRowCoordRadarSpeed]));
    out.triangleLevel = Curve(l[kRowTriangleProb]);
}

const wchar_t* LevelLabel(int wireIndex) {
    return (wireIndex >= 0 && wireIndex < kLevelCount) ? kRows[wireIndex].prefix : nullptr;
}

int RefreshOpenRows() {
    void* cls = R::FindClass(kRowClass);
    if (!cls) return 0;
    // The memoised lookup: this runs on every purchase and on every mirror the client applies, and
    // a hit renders no name. It holds the answer by slot and serial, and holds a miss as a miss.
    void* fn = R::FindDispatchFunctionCached(cls, kRowRefresh);
    if (!fn) return 0;
    struct Ctx { void* fn; int n; } ctx{ fn, 0 };
    OI::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* x = static_cast<Ctx*>(c);
        if (!obj) return;
        // The class default object has no children to repaint and its upd() would read them. The
        // prefix test reads the name in place; rendering it to a string would allocate per row, and
        // the panel carries fifty.
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        ue_wrap::ParamFrame f(x->fn);
        if (!f.valid()) return;
        if (ue_wrap::Call(obj, f)) ++x->n;
    }, &ctx);
    return ctx.n;
}

}  // namespace ue_wrap::upgrades
