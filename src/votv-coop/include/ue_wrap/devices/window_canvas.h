// ue_wrap/devices/window_canvas.h -- engine access for the base's big bay window (Ad_window_C),
// whose dirt is a 1645x512 render target that a sponge wipes clean one dab at a time. Principle-7
// wrapper: reflection offsets, the UFunctions and the draw; no network or coop state
// (coop::window_stroke_sync owns those).
//
// A stroke is d_window_C::cleanPhys(sponge, Hit): it maps the hit to a texture pixel, stores the
// dab's top-left corner in the field `A`, opens the window's canvas session through its `Canvas`
// event (sets `cv`; the session closes itself 0.25 s after the last dab) and draws the sponge's
// brush material with UCanvas::K2_DrawMaterial(dynmat, A, (size,size), (0,0), (1,1), 0, (.5,.5)).
// cleanPhys is a script function reached through EX_LocalVirtualFunction, so no hook that carries
// arguments sees it; the stroke is observed at its native draw instead. A K2_DrawMaterial whose
// calling frame is a d_window_C with `cv` set is a stroke: the window's only other draw, the
// periodic dirt splotch, refuses to run while `cv` is set. The window is a singleton without a
// save Key. External code must never call setDraw/endDraw: the session belongs to `Canvas`.

#pragma once

#include <cstdint>

namespace ue_wrap::window_canvas {

// Which cleaner drew a dab: the class of the prop_sponge_C whose brush material it painted with.
// Each draws with its own class default's brush, so a replay picks the brush by this.
enum class DabSource : uint8_t { Sponge = 0, Washer = 1, Pour = 2, Mop = 3 };
inline constexpr uint8_t kDabSourceCount = 4;
const char* DabSourceName(DabSource s);

// One dab, in the render target's pixel space.
struct Dab {
    float x = 0.f, y = 0.f;  // top-left corner (the draw's ScreenPosition, the window's field A)
    float size = 0.f;        // edge length (the draw's ScreenSize)
    float opac = 0.f;        // brush material scalar "opac"
    float col = 0.f;         // brush material scalar "col" (0 = paint clean)
    DabSource source = DabSource::Sponge;
};

// Resolve d_window_C (fields A, canv, cv; the Canvas event) and UCanvas::K2_DrawMaterial.
// Idempotent; false while the window's Blueprint class is not loaded. Game thread.
bool EnsureResolved();

// Class-pure: obj is a d_window_C. False if unresolved.
bool IsWindow(void* obj);

// True iff `sourceObject` -- the frame that issued a K2_DrawMaterial -- is a window with an open
// stroke session. Raw field reads only, so it is safe inside the Func hook.
bool IsStrokeDraw(void* sourceObject);

// The dab a stroke draw is about to paint, read from the K2_DrawMaterial call itself: `parms` is the
// call's parameter block (a ufunction_hook pre callback's), so position, size and the brush material are
// the ones the stroke passed, whoever drew it -- the held sponge, the mop, a thrown sponge, a bucket pour
// or the pressure washer's shot. opac/col are read from that material (two engine dispatches); the
// source from the material's outer, the cleaner that made it. False when `sourceObject` is not a stroke
// draw or a parameter does not read. Game thread.
bool ReadStrokeDraw(void* sourceObject, const uint8_t* parms, Dab& out);

// Signal playback owns the world's render-target canvas; the window refuses to draw meanwhile.
bool IsSignalPlaying();

// Replay one dab on `window`: open or join the session through Canvas, then draw with a brush
// material instance this module owns. False when anything is unresolved. Game thread.
bool DrawDab(void* window, const Dab& d);

// Release the owned brush material (session teardown). Game thread.
void ReleaseBrush();

}  // namespace ue_wrap::window_canvas
