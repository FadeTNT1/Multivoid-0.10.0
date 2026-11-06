// ue_wrap/devices/window_canvas.cpp -- see ue_wrap/devices/window_canvas.h. Engine access for the
// base's bay window render target (Ad_window_C). Every offset is resolved by name from the live
// classes; the bytecode facts this relies on are in the header.

#include "ue_wrap/devices/window_canvas.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ue_wrap::window_canvas {
namespace {

namespace R = reflection;

// ---- core: the window and the draw (resolved together; published through g_resolved) ----
std::atomic<bool> g_resolved{false};
void*   g_winCls      = nullptr;  // d_window_C
int32_t g_offA        = -1;       // FVector2D A  -- the last dab's top-left corner
int32_t g_offCanv     = -1;       // UCanvas* canv -- the world's shared render-target canvas
int32_t g_cvByte      = -1;       // bool cv      -- a stroke session is open
uint8_t g_cvMask      = 0;
void*   g_canvasEvent = nullptr;  // d_window_C::Canvas -- opens/joins the session, re-arms its close
void*   g_drawFn      = nullptr;  // UCanvas::K2_DrawMaterial

template <class T>
T ReadField(void* obj, int32_t off) {
    T v{};
    std::memcpy(&v, static_cast<const char*>(obj) + off, sizeof(T));
    return v;
}

// ---- lazily resolved: the cleaner classes, material parameters, the signal-playback gate ----
namespace PN = ue_wrap::profile::name;

// One entry per DabSource, in its order: the class whose default `drawmat` is that source's brush.
const wchar_t* const kSourceClass[kDabSourceCount] = {PN::SpongeClass, PN::SpongeShotClass,
                                                      PN::SpongePourClass, PN::MopClass};
void*   g_sourceCls[kDabSourceCount] = {};
void*   g_spongeCls     = nullptr;  // prop_sponge_C: every cleaner derives from it
int32_t g_offDrawmat    = -1;       // MaterialInterface* drawmat (the brush parent material)
void*   g_setScalarFn   = nullptr;  // MaterialInstanceDynamic::SetScalarParameterValue
void*   g_matLibCdo     = nullptr;  // Default__KismetMaterialLibrary
void*   g_createMidFn   = nullptr;  // KismetMaterialLibrary::CreateDynamicMaterialInstance
int32_t g_offPanels     = -1;       // mainGamemode_C::analogPanels
void*   g_panelsCls     = nullptr;  // class the isPlayingSignal offset below was resolved on
int32_t g_playingByte   = -1;
uint8_t g_playingMask   = 0;
bool    g_drawSigChecked = false;
bool    g_drawSigOk      = false;
// K2_DrawMaterial's parameter offsets, read in a pre callback.
int32_t g_parMaterial = -1, g_parPos = -1, g_parSize = -1;

// Our brush MaterialInstanceDynamics, one per source (outer = transient package).
ue_wrap::GcPin g_brush[kDabSourceCount];

bool EnsureSponge() {
    if (g_spongeCls) return true;
    void* cls = R::FindClass(PN::SpongeClass);
    if (!cls) return false;
    g_offDrawmat = R::FindPropertyOffset(cls, PN::SpongeDrawMatProp);
    g_spongeCls = cls;
    UE_LOGI("window_canvas: sponge resolved drawmat@0x%X", g_offDrawmat);
    return true;
}

// The cleaner class `cls` is (or derives from), most derived first; Sponge for any other sponge.
DabSource SourceOfClass(void* cls) {
    for (uint8_t i = 1; i < kDabSourceCount; ++i) {
        if (!g_sourceCls[i]) g_sourceCls[i] = object_index::ClassByName(kSourceClass[i]);  // one lookup
        if (!g_sourceCls[i]) continue;
        void* bases[1] = {g_sourceCls[i]};
        if (R::IsDescendantOfAny(cls, bases, 1)) return static_cast<DabSource>(i);
    }
    return DabSource::Sponge;
}

bool EnsureMaterialFns() {
    if (g_setScalarFn) return true;
    void* mid = R::FindClass(L"MaterialInstanceDynamic");
    if (!mid) return false;
    g_setScalarFn = R::FindFunction(mid, L"SetScalarParameterValue");
    return g_setScalarFn != nullptr;
}

// The two parameter names, converted once. StringToFName DISPATCHES ProcessEvent, so converting a
// literal per call would put an engine dispatch in front of every scalar read and write -- four per
// dab replayed. The drone's dust parameter is cached the same way and for the same reason.
R::FName NameOnce(R::FName& slot, const wchar_t* name) {
    if (slot.ComparisonIndex == 0 && slot.Number == 0) slot = fname_utils::StringToFName(name);
    return slot;
}
R::FName g_nOpac{0, 0};
R::FName g_nCol{0, 0};

bool SetScalar(void* mid, R::FName name, float value) {
    ParamFrame f(g_setScalarFn);
    if (!f.valid()) return false;
    f.Set<R::FName>(L"ParameterName", name);
    f.Set<float>(L"Value", value);
    return Call(mid, f);
}

// A source's brush parent material: its class default's drawmat, else the sponge's, else the asset by name.
void* FindBrushParent(DabSource src) {
    if (EnsureSponge() && g_offDrawmat >= 0) {
        for (const wchar_t* cls : {kSourceClass[static_cast<uint8_t>(src)], PN::SpongeClass}) {
            void* cdo = R::FindClassDefaultObject(cls);
            if (!cdo) continue;
            void* m = ReadField<void*>(cdo, g_offDrawmat);
            if (m && R::IsLive(m)) return m;
        }
    }
    return R::FindObject(L"mat_cleanBrush_Inst", L"MaterialInstanceConstant");
}

void* EnsureBrush(void* worldContext, DabSource src) {
    ue_wrap::GcPin& pin = g_brush[static_cast<uint8_t>(src) % kDabSourceCount];
    if (pin.Held() && R::IsLive(pin.Raw())) return pin.Raw();
    if (!g_matLibCdo) g_matLibCdo = R::FindClassDefaultObject(L"KismetMaterialLibrary");
    if (!g_createMidFn) {
        if (void* lib = R::FindClass(L"KismetMaterialLibrary"))
            g_createMidFn = R::FindFunction(lib, L"CreateDynamicMaterialInstance");
    }
    void* parent = FindBrushParent(src);
    if (!g_matLibCdo || !g_createMidFn || !parent) return nullptr;
    ParamFrame f(g_createMidFn);
    if (!f.valid() || f.ParamOffset(L"Parent") < 0 || f.ParamOffset(L"ReturnValue") < 0) return nullptr;
    if (f.ParamOffset(L"WorldContextObject") >= 0) f.Set<void*>(L"WorldContextObject", worldContext);
    f.Set<void*>(L"Parent", parent);
    if (!Call(g_matLibCdo, f)) return nullptr;
    void* mid = f.Get<void*>(L"ReturnValue");
    if (!mid || !pin.Pin(mid)) return nullptr;
    UE_LOGI("window_canvas: %s brush material instance %p created from %p", DabSourceName(src), mid, parent);
    return mid;
}

// K2_DrawMaterial is set up by name; refuse (once, loudly) if the engine's signature differs.
bool DrawSignatureOk() {
    if (g_drawSigChecked) return g_drawSigOk;
    g_drawSigChecked = true;
    ParamFrame f(g_drawFn);
    const wchar_t* const names[] = {L"RenderMaterial", L"ScreenPosition", L"ScreenSize",
                                    L"CoordinatePosition", L"CoordinateSize", L"Rotation", L"PivotPoint"};
    g_drawSigOk = f.valid();
    for (const wchar_t* n : names) {
        if (f.ParamOffset(n) < 0) {
            UE_LOGE("window_canvas: UCanvas::K2_DrawMaterial has no parameter '%ls' -- refusing to draw", n);
            g_drawSigOk = false;
        }
    }
    g_parMaterial = R::FindParamOffset(g_drawFn, L"RenderMaterial");
    g_parPos      = R::FindParamOffset(g_drawFn, L"ScreenPosition");
    g_parSize     = R::FindParamOffset(g_drawFn, L"ScreenSize");
    return g_drawSigOk;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    void* winCls = R::FindClass(L"d_window_C");
    if (!winCls) return false;
    const int32_t offA    = R::FindPropertyOffset(winCls, L"A");
    const int32_t offCanv = R::FindPropertyOffset(winCls, L"canv");
    int32_t cvByte = -1;
    uint8_t cvMask = 0;
    const bool cvOk = R::FindBoolProperty(winCls, L"cv", cvByte, cvMask);
    void* canvasEvent = R::FindFunction(winCls, L"Canvas");
    void* canvasCls = R::FindClass(L"Canvas");
    void* drawFn = canvasCls ? R::FindFunction(canvasCls, L"K2_DrawMaterial") : nullptr;
    if (offA < 0 || offCanv < 0 || !cvOk || !canvasEvent || !drawFn) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            UE_LOGW("window_canvas: d_window_C incomplete (A@%d canv@%d cv=%d Canvas=%p K2_DrawMaterial=%p)",
                    offA, offCanv, cvOk ? 1 : 0, canvasEvent, drawFn);
        }
        return false;
    }
    g_winCls = winCls;
    g_offA = offA;
    g_offCanv = offCanv;
    g_cvByte = cvByte;
    g_cvMask = cvMask;
    g_canvasEvent = canvasEvent;
    g_drawFn = drawFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("window_canvas: resolved d_window_C=%p A@0x%X canv@0x%X cv@0x%X/0x%02X Canvas=%p K2_DrawMaterial=%p",
            winCls, offA, offCanv, cvByte, cvMask, canvasEvent, drawFn);
    return true;
}

bool IsWindow(void* obj) {
    if (!obj || !g_resolved.load(std::memory_order_acquire)) return false;
    void* cls = R::ClassOf(obj);
    void* bases[1] = {g_winCls};
    return cls && R::IsDescendantOfAny(cls, bases, 1);
}

bool IsStrokeDraw(void* sourceObject) {
    if (!sourceObject || !g_resolved.load(std::memory_order_acquire)) return false;
    if (R::ClassOf(sourceObject) != g_winCls) return false;
    const uint8_t b = ReadField<uint8_t>(sourceObject, g_cvByte);
    return (b & g_cvMask) != 0;
}

const char* DabSourceName(DabSource s) {
    switch (s) {
        case DabSource::Sponge: return "sponge";
        case DabSource::Washer: return "washer";
        case DabSource::Pour:   return "pour";
        case DabSource::Mop:    return "mop";
    }
    return "sponge";
}

bool ReadStrokeDraw(void* sourceObject, const uint8_t* parms, Dab& out) {
    if (!parms || !IsStrokeDraw(sourceObject) || !DrawSignatureOk()) return false;
    if (g_parMaterial < 0 || g_parPos < 0 || g_parSize < 0) return false;
    void* mat = ReadField<void*>(const_cast<uint8_t*>(parms), g_parMaterial);
    const FVector2D pos = ReadField<FVector2D>(const_cast<uint8_t*>(parms), g_parPos);
    const FVector2D size = ReadField<FVector2D>(const_cast<uint8_t*>(parms), g_parSize);
    out.x = pos.X;
    out.y = pos.Y;
    out.size = size.X;
    out.opac = 0.4f;  // the sponge's own still-held value, for a material that does not read
    out.col = 0.f;
    out.source = DabSource::Sponge;
    if (!mat || !R::IsLive(mat)) return true;
    // The cleaner's wash() set opac on this material right before the draw; read it back.
    float v = 0.f;
    if (ue_wrap::engine::GetScalarParameterValue(mat, NameOnce(g_nOpac, L"opac"), v)) out.opac = v;
    if (ue_wrap::engine::GetScalarParameterValue(mat, NameOnce(g_nCol, L"col"), v)) out.col = v;
    void* owner = R::OuterOf(mat);
    if (owner && R::IsLive(owner)) out.source = SourceOfClass(R::ClassOf(owner));
    return true;
}

bool IsSignalPlaying() {
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;
    if (g_offPanels < 0) g_offPanels = R::FindPropertyOffset(R::ClassOf(gm), L"analogPanels");
    if (g_offPanels < 0) return false;
    void* panels = ReadField<void*>(gm, g_offPanels);
    if (!panels || !R::IsLive(panels)) return false;
    if (g_panelsCls != R::ClassOf(panels)) {
        g_panelsCls = R::ClassOf(panels);
        if (!R::FindBoolProperty(g_panelsCls, L"isPlayingSignal", g_playingByte, g_playingMask))
            g_playingByte = -1;
    }
    if (g_playingByte < 0) return false;
    return (ReadField<uint8_t>(panels, g_playingByte) & g_playingMask) != 0;
}

bool DrawDab(void* window, const Dab& d) {
    if (!window || !R::IsLive(window) || !g_resolved.load(std::memory_order_acquire)) return false;
    if (!DrawSignatureOk() || !EnsureMaterialFns()) return false;
    void* brush = EnsureBrush(window, d.source);
    if (!brush) return false;

    ParamFrame open(g_canvasEvent);
    if (!open.valid() || !Call(window, open)) return false;
    void* canv = ReadField<void*>(window, g_offCanv);
    if (!canv || !R::IsLive(canv)) return false;

    SetScalar(brush, NameOnce(g_nOpac, L"opac"), d.opac);
    SetScalar(brush, NameOnce(g_nCol, L"col"), d.col);
    ParamFrame f(g_drawFn);
    f.Set<void*>(L"RenderMaterial", brush);
    f.Set<FVector2D>(L"ScreenPosition", FVector2D{d.x, d.y});
    f.Set<FVector2D>(L"ScreenSize", FVector2D{d.size, d.size});
    f.Set<FVector2D>(L"CoordinatePosition", FVector2D{0.f, 0.f});
    f.Set<FVector2D>(L"CoordinateSize", FVector2D{1.f, 1.f});
    f.Set<float>(L"Rotation", 0.f);
    f.Set<FVector2D>(L"PivotPoint", FVector2D{0.5f, 0.5f});
    return Call(canv, f);
}

void ReleaseBrush() {
    for (ue_wrap::GcPin& pin : g_brush) pin.Release();
}

}  // namespace ue_wrap::window_canvas
