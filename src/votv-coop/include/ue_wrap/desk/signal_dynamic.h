// ue_wrap/desk/signal_dynamic.h -- the Fstruct_signalDataDynamic bridge.
//
// This ONE struct is both the desk refiner's loaded signal (comp_data_0) and an element of
// gamemode.savedSignals_0, the desk signal library. Its rows are pure POD and strings, with no
// texture or sound pointers: those live in the per-TYPE template Fstruct_signal_data, rebuilt
// identically on every peer from the list_signals DataTable and re-derived natively at play time by
// lib_C.dynamicToSignal. So a row is fully serializable. The one heavy member is `image`, a PNG
// byte TArray used solely by the laptop photo view, which the live mirror SKIPS; a joiner converges
// on it through the save transfer instead.
//
// Member offsets are MEASURED (see Layout below): 0.10.0 added a member mid-struct, so a row
// written at the old stride lands every row after the first on its neighbour's bytes.
//
// Principle-7 engine-wrapper layer; no network logic.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::signal_dynamic {

// The measured layout of Fstruct_signalDataDynamic: each member's byte offset, the struct's span
// (PropertiesSize) and the stride an array of it is walked at (the span rounded to the struct's
// 8-byte alignment: its widest members are pointers and int64).
struct Layout {
    int32_t name = -1;      // FString
    int32_t level = -1;     // int32
    int32_t id = -1;        // FString (base64url, ~22 chars)
    int32_t size = -1;      // float
    int32_t decoded = -1;   // float
    int32_t date = -1;      // FDateTime (int64 ticks)
    int32_t isCopy = -1;    // bool
    int32_t polarity = -1;  // int32
    int32_t loc = -1;       // FVector2D signalLocation
    int32_t object = -1;    // FName
    int32_t signal = -1;    // FName (the template key)
    int32_t image = -1;     // Fstruct_byteImage = TArray<uint8>
    int32_t freq = -1;      // enum byte
    int32_t qual = -1;      // enum byte
    int32_t objType = -1;   // enum byte
    int32_t daq = -1;       // float downloadedAtQuality
    int32_t span = 0;
    int32_t stride = 0;
};

// The running build's layout, measured once the struct asset is loaded (retried at most once a
// second before that). Null while unmeasured or when the struct fails to read as the row; every
// caller refuses then. Game thread.
const Layout* GetLayout();

// The measured element stride, 0 while unmeasured: the one number an array walk of rows needs.
int32_t Stride();

// Whether `owner`.`member` (a UClass* / UScriptStruct* and the TArray property's name on it) is a
// TArray of THIS struct whose engine ElementSize is the measured stride. The stride is computed from the
// struct's span, so no walk of a row array may trust it until the engine's own number for that array
// has agreed: a mismatch walks every row after the first onto its neighbour's bytes, and a write there
// is heap corruption. False while unmeasured (asked again) and, said once per (owner, member), on a
// mismatch -- the caller refuses. Cached per pair. Game thread.
bool ArrayHoldsRows(void* owner, const wchar_t* member);
// The single-row counterpart: `member` is one struct_signalDataDynamic (the struct the layout was measured on).
bool MemberIsRow(void* owner, const wchar_t* member);

// Wire-facing view of one row. The photo is read only when asked (ReadStruct's withImage): the lanes that
// poll rows at a cadence leave it out, and the rack packs sixteen rows into one blob.
struct Row {
    std::wstring name;            // display name ("<type> [n]", user-renamable)
    std::wstring id;              // non-unique across copies; nothing looks rows up by it
    std::wstring object;          // FName leaf as string
    std::wstring signal;          // FName leaf as string -- the template key
    int32_t level = 0;
    int32_t polarity = 0;
    float   size = 0;             // saveSignal gate: must be > 0
    float   decoded = 0;          // saveSignal gate: must be >= size
    float   downloadedAtQuality = 0;
    float   locX = 0, locY = 0;
    int64_t date = 0;             // FDateTime ticks (ship non-epoch or the receiver stamps Now)
    bool    isCopy = false;
    uint8_t frequency = 0, quality = 0, objectType = 0;
    bool    hasData = false;      // comp empty-state marker (size>0 on real rows)
    std::vector<uint8_t> image;   // the laptop photo's PNG bytes, when read withImage

    // Member-wise, so the save-record splice can ask whether a leaf class stored its own row where
    // the base class stores one. Rows read without their photos compare equal whatever the photos
    // hold -- the same blind spot the digest has, and for the same reason.
    bool operator==(const Row&) const = default;
};

// Raw-read the struct at `base` into `out` (FStrings read directly; FNames
// via the engine name table), the photo too when `withImage`. Zero reflected calls.
// False while the layout is unmeasured.
bool ReadStruct(const void* base, Row& out, bool withImage = false);

// Write `in` into the LIVE struct at `base`: PODs raw, FStrings ENGINE-MINTED
// (fstring_utils -- the game later reassigns/destroys these fields with the
// engine allocator), FNames interned via fname_utils. The previous string
// buffers deliberately leak (pin doctrine; button-rate writes). The image
// array is emptied (Num=0, allocation untouched) so a stale photo never
// attaches to a different signal. All or nothing: false leaves the struct
// as it was -- a leaf the engine interns to NAME_None ("None") or a string
// that cannot be minted writes no field at all. Game thread.
bool WriteStructLive(void* base, const Row& in);

// Build the param-frame bytes for a struct PARAMETER (gamemode.saveSignal's `signal`), sized to
// the measured span: PODs raw, FStrings and the photo pointing AT `in`'s buffers (the callee
// deep-copies during the call -- `in` must outlive it), FNames interned. False with `out` empty
// while the layout is unmeasured. Game thread.
bool BuildParamBytes(const Row& in, std::vector<uint8_t>& out);

}  // namespace ue_wrap::signal_dynamic
