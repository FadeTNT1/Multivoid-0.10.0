// ue_wrap/actors/save_record.h -- the Fstruct_save record: its POD form, its engine codec,
// and the TArray primitives both are built on.
//
// Principle-7 engine-wrapper layer, and the one implementation of this codec: a container's
// contents are a TArray<Fstruct_save> in the global saveSlot.GObjStack, exactly what the player
// inventory is, so both lanes read it from here rather than each keeping a copy.
//
// The fact the whole file rests on: a saved item is DATA, not an actor -- a Fstruct_save
// record of class, transform, key and typed key/value groups. Reading one means walking the
// nested TArrays: each value group is a TArray<Fstruct_mX> whose element wraps a single
// TArray<X>, giving a vector of vectors, except `signals`, which is a TArray of
// Fstruct_signalDataDynamic directly. FNames and UClasses are read as STRINGS, since neither
// pointer is portable across peers; they are re-interned or resolved by name on apply.
//
// Game thread (the write side mints FStrings + interns FNames through the engine).

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"  // Row -- the signal sub-element (reused, not reinvented)

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::save_record {

// How a class leaf name on a record resolves to a UClass as the record is written. A null resolver
// means reflection::FindClass, the authoritative walk-the-array-on-a-miss that is correct everywhere
// but costs a whole-array walk per unresolved name. A peer applying data IT RECEIVED FROM AN
// ATTACKER (the host taking a client's record) passes ue_wrap::object_index::ClassByName instead: a
// one-step lookup that answers a loaded class and a miss alike, so an attacker's unknown name costs a
// hash probe, not a 250k-object walk. A client applying the host's own
// record keeps the default: its object index can trail during a blocking world load, where the walk
// still finds a class the index does not list yet, and a cheating host is out of scope here.
using ClassResolver = void* (*)(const wchar_t*);

// One Fstruct_save record as engine-agnostic POD (the wire layer serializes this).
struct SaveRecord {
    std::wstring className;          // TSubclassOf<AActor> leaf name; empty = null class
    std::array<float, 10> xform{};   // FTransform: quat(x,y,z,w) + loc(x,y,z) + scale(x,y,z)
    std::wstring key;                // FName
    // Value groups: TArray<Fstruct_mX> (each Fstruct_mX = TArray<X>) -> vector<vector<X>>.
    std::vector<std::vector<uint8_t>>               bools;
    std::vector<std::vector<float>>                 floats;
    std::vector<std::vector<int32_t>>               ints;
    std::vector<std::vector<std::wstring>>          strings;
    std::vector<ue_wrap::signal_dynamic::Row>       signals;  // TArray<signal> directly (flat)
    std::vector<std::vector<std::wstring>>          classes;  // TSubclassOf leaf names
    std::vector<std::vector<std::array<float, 3>>>  vectors;  // FVector
    std::vector<std::vector<std::array<float, 3>>>  rotators; // FRotator (pitch,yaw,roll)
    std::vector<std::vector<std::array<float, 10>>> transforms; // FTransform packed
    std::vector<std::vector<uint8_t>>               bytes;
    std::vector<std::vector<std::wstring>>          names;    // FName
};

// THE LAYOUT IS MEASURED, never a constant. The record struct is a Blueprint UserDefinedStruct
// whose shape moves between game builds (0.9.0n's Fstruct_save carries a flat `signals` group;
// 0.10.0's struct_save1 has none, so every later group moved up and the struct shrank by sixteen
// bytes). Every member offset is read off the running struct by its stable name prefix, every value
// group is type-checked down to its inner element size, and the array stride is the struct's
// aligned size as the engine walks it. While the layout is unmeasured, or when the struct fails to
// read as a record, every read and write below refuses and touches nothing.
struct RecordLayout {
    void*   script = nullptr;  // the UScriptStruct measured
    int32_t span = 0;          // UStruct::PropertiesSize: every field ends inside this many bytes
    int32_t stride = 0;        // the element stride of a TArray of records (span rounded to 16)
};

// The running build's record layout (the struct Aprop_C::getData files a prop into, else the struct
// asset by name). Null while unmeasured; retried at most once a second. Game thread.
const RecordLayout* Layout();
// Its stride and span, 0 while unmeasured -- a caller walking an array of records refuses on 0.
int32_t RecordStride();
int32_t RecordSpan();
// Whether `scriptStruct` is the measured record struct: the check a caller makes before walking an
// array whose element type it found by reflection.
bool IsRecordStruct(void* scriptStruct);

inline constexpr int32_t kMxStride     = 0x10;  // Fstruct_mX wraps a single TArray<X> @ +0; checked per group as measured

// A corrupt/uninitialized TArray Num must never drive a runaway loop or a giant reserve.
inline constexpr int32_t kMaxArr = 200000;

// ---- TArray primitives (shared by every struct-array walk) ----------------------------------

// A pointer that could plausibly be a live UObject/heap buffer: non-null, above the null page,
// 8-aligned, inside the Win64 user-address range. Rejects garbage misread as a pointer BEFORE
// the engine IsLive/NameOf derefs it (a wild pointer faults the whole game-thread tick).
inline bool PlausibleObjPtr(const void* p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && (v & 0x7) == 0 && v < 0x7FFFFFFFFFFFull;
}

// A read TArray header, with a garbage header rejected to {nullptr, 0}.
struct Arr { const uint8_t* data = nullptr; int32_t num = 0; };
Arr ReadArr(const void* base, int32_t off);

// Write a UE TArray header {Data, Num, Max=Num} at base+off. (Max==Num is correct for a
// freshly-built array the engine will read then, on its next mutation, realloc wholesale.)
void WriteArrHeader(void* base, int32_t off, void* data, int32_t num);

// Allocate `count*elemSize` engine bytes, zeroed. Null (-> caller writes an empty array) on a
// zero count or an EngineAlloc failure (GMalloc unresolved). Zeroing makes every unset FString/
// FName/TArray field a valid empty (null,0,0) -- no garbage the engine could deref.
void* AllocZeroed(size_t count, size_t elemSize);

// FName read/write at a raw field address (empty string <-> NAME_None).
std::wstring ReadFNameAt(const void* base, int32_t off);
void WriteFNameField(void* dst, const std::wstring& leaf);

// ---- The record codec ------------------------------------------------------------------------

// Read one record at `base` into `rec` through the measured layout. Pure field reads +
// FName::ToString / FString copy (no UFunction dispatch). `rec` is overwritten field-by-field; pass
// a fresh record. False, reading nothing, while the layout is unmeasured.
bool ReadSaveRecord(const void* base, SaveRecord& rec);

// Write one record into the ZEROED RecordSpan()-byte slot at `base`, constructing engine-OWNED
// TArrays via reflection::EngineAlloc (FNames interned, FStrings engine-minted, UClasses
// FindClass'd). The slot MUST be zeroed by the caller (AllocZeroed); any previous buffers it
// referenced are intentionally orphaned -- recursively freeing nested Fstruct_save sub-arrays +
// FString buffers is far more crash-prone than a bounded leak, and the engine never double-frees
// a buffer it has lost the pointer to. Buffers WE allocate are GMalloc-owned (EngineAlloc), so
// the engine's later Array realloc / GC free of them is allocator-matched.
// `resolve` governs how every class leaf (the record's own class and its `classes` group) resolves;
// null = reflection::FindClass (see ClassResolver).
// False, writing nothing, while the layout is unmeasured. A value group the running struct has no
// member for is dropped (said once).
bool WriteSaveRecord(uint8_t* base, const SaveRecord& r, ClassResolver resolve = nullptr);

// ---- The GAME's own codec, on a LIVE actor --------------------------------------------------
//
// The two functions above read and write a record at a raw address. The two below get one FROM a
// live actor and give one back TO it, through the actor's own `getData` and `loadData`. That is
// the difference between a set of carried fields WE enumerate per class and the one the game
// serializes: a class that adds save state is carried without a line of our code changing.
//
// Both climb to the most-derived declaration, because reflection::FindFunction is exact-owner and
// the override is the whole point -- Aprop_floppyDisc_C::getData calls its parent's and then
// appends the disc's `data` and `readWrites`, so a call resolved at Aprop_C would return the base
// record and silently drop exactly the state this exists to carry.

// True when `actor`'s class declares a `getData` of its own somewhere below Aprop_C -- i.e. it
// serializes state beyond the base prop record. Resolved from the live class chain and cached per
// UClass, so it is a membership test the GAME answers; a hand-written class list would go stale
// the first time VOTV adds a save-backed prop. False for a non-prop or an unresolvable class.
bool OverridesGetData(void* cls);

// Capture `actor`'s save record as the actor itself serializes it. False when `getData` does not
// resolve or the call fails.
// An actor's own record as the payload that carries it to another peer is the pair Relay, Moddy's
// VOTV mod, publishes as a rule's spawn payload on its README Blueprint page (docs/credits.md).
bool CaptureRecord(void* actor, SaveRecord& out);

// `actor`'s record as the game's own addObject files it: the most-derived `getData`, with no splice convention, since
// a put stores the record raw. False when `actor` is not a prop, `getData` does not resolve or the call fails.
bool CaptureOwnRecord(void* actor, SaveRecord& out);

// Whether `cls` is Aprop_C or a descendant of it: the classes a prop record can name. One walk of the class chain.
bool IsPropClass(void* cls);

// The prop's name, names[0][0] (Aprop_C::getData packs [name, nametag] as the first names group). False when the
// record has no such element: a record that came off the wire is never indexed without this.
inline bool RecordName(const SaveRecord& r, std::wstring& out) {
    if (r.names.empty() || r.names[0].empty()) return false;
    out = r.names[0][0];
    return true;
}

// Hand a record back to `actor` through its own `loadData` -- the LEAF half of it only.
//
// Aprop_C::loadData restores the save Key, the two names, the four saved bools, the scale and the
// lifespan from whatever record it is handed, then re-runs `init()`, `physicsImpact->init()` and
// `setNametag()`. Taken at face value that is a primitive for rewriting any prop's identity and for
// destroying it through SetLifeSpan, and every one of those fields already rides the prop's spawn
// row, which is their authority. So the receiver's OWN base record is read first and spliced over
// the incoming one, ELEMENT by element over the base's own extent: what survives from `r` is
// exactly what Aprop_C::getData does not write, which is the leaf class's own save state and the
// only thing this pair exists to move -- including the TAIL of a group the base also writes, which
// is where a food keeps its temperature. Which elements those are is read off the base class at
// runtime, never listed here. Two dispatches: the base capture and the apply.
// `resolve` is forwarded to the write of the merged record (null = FindClass; see ClassResolver).
bool ApplyRecord(void* actor, const SaveRecord& r, ClassResolver resolve = nullptr);

// ---- the same dispatch, without the prop lineage ----------------------------------------------
//
// The pair above is the PROP codec: it resolves its verbs only for classes on the `Aprop_C` chain,
// and its apply splices the receiver's own base half over the sender's because for a prop the base
// fields already ride the spawn row. A class off that chain -- `Ahook_C` is an `Aactor_save_C`, and
// the two lineages part at `AActor` -- needs the same dispatch and neither the gate nor the splice,
// and it needs a CALLER-supplied UFunction because the lineage is what `VerbsFor` keys on.
//
// These are the dispatch half alone, so there is one implementation of it rather than a copy in
// every wrapper that serializes a non-prop. The lineage question, the splice and whatever
// authority the record's fields need belong to the caller, which is the only party that knows.

// Run `recordFn` (a `getData`-shaped UFunction with a `data` out-parameter) on `actor` and read the
// record it leaves in the frame. False if the frame has no `data` parameter that measures as a record
// (its own extent, not the frame's, bounds the read) or the call fails.
bool CaptureRecordVia(void* actor, void* recordFn, SaveRecord& out);

// Write `r` into `loadFn`'s `data` parameter and call it. THE RECORD IS APPLIED AS GIVEN: there is
// no splice here, so a caller handing this a record that came off the wire has already decided,
// field by field, which parts it is willing to take from the sender.
// `resolve` is forwarded to the record write (null = FindClass; see ClassResolver).
bool ApplyRecordVia(void* actor, void* loadFn, const SaveRecord& r, ClassResolver resolve = nullptr);

// Drop the cached class lookups (level change / disconnect).
void ResetCodecCache();

}  // namespace ue_wrap::save_record
