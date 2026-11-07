// ue_wrap/actors/inventory.cpp -- see ue_wrap/actors/inventory.h.
//
// The Fstruct_save record codec + the TArray primitives live in ue_wrap/actors/save_record, which
// coop/props/container_contents_sync needs for the same walk; what stays here is the
// player-scoped concern: the saveSlot resolve, the three player arrays, and Fstruct_equipment.

#include "ue_wrap/actors/inventory.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace ue_wrap::inventory {
namespace {

namespace R = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

// The equipment and hold arrays, MEASURED. Both are TArray<Fstruct_equipment> on the save object,
// an element holding a propDynamic id, an embedded record and a tag. 0.10.0 moved both arrays,
// dropped `hold`, and its struct_equipment1 carries a struct_betterSave (`betterdata`) instead of a
// record. Swapping a header in at the old build's offsets, or zeroing a whole old record's span
// inside an element, is then a write into whatever lives there now -- heap corruption. So the array offsets, the element struct, its stride
// and its members are read off the running classes, the embedded member must BE the measured
// record struct, and a layout that does not read that way makes the equipment half of this file
// refuse (said once) while the carried half keeps working.
struct EquipLayout {
    bool    ok = false;
    int32_t offEquipment = -1;  // saveSlot.equipment (TArray header)
    int32_t offHold = -1;       // saveSlot.hold, -1 when the build has none
    int32_t stride = 0;         // element stride (the array's inner ElementSize)
    int32_t propName = -1;      // Fstruct_propDynamic.name FName, -1 when absent
    int32_t propKey = -1;       // Fstruct_propDynamic.key FName, -1 when absent
    int32_t data = -1;          // the embedded record
    int32_t tag = -1;           // FName, -1 when absent
};
EquipLayout g_equip;
void* g_equipMeasuredFor = nullptr;   // the save class the layout was measured on

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = std::wcslen(prefix);
    return s.size() >= n && ::_wcsnicmp(s.c_str(), prefix, n) == 0;
}

// One equipment element struct's members, measured against the record layout.
bool MeasureEquipStruct(void* equipStruct, int32_t stride, EquipLayout& L) {
    if (!equipStruct || stride <= 0) return false;
    const int32_t span = R::StructSize(equipStruct);
    if (span <= 0 || span > stride) return false;
    L.stride = stride;
    for (const R::StructFieldInfo& f : R::EnumerateStructFields(equipStruct)) {
        if (f.offset < 0 || f.offset + f.size > span) continue;
        void* inner = R::PropertyInnerStruct(equipStruct, f.name.c_str());
        if (inner && SR::IsRecordStruct(inner) && f.size >= SR::RecordSpan()) {
            L.data = f.offset;
        } else if (inner && f.offset == 0 && f.size == 16) {
            // Fstruct_propDynamic {name, key}: two FNames.
            const int32_t n = R::FindPropertyOffsetByPrefix(inner, L"name_");
            const int32_t k = R::FindPropertyOffsetByPrefix(inner, L"key_");
            if (n >= 0 && n + 8 <= 16) L.propName = n;
            if (k >= 0 && k + 8 <= 16) L.propKey = k;
        } else if (StartsWithI(f.name, L"tag_") && f.size == 8) {
            L.tag = f.offset;
        }
    }
    return L.data >= 0;
}

// The equipment layout of `save`'s class, measured once per class object.
const EquipLayout* EquipLayoutOf(void* save) {
    void* cls = save ? R::ClassOf(save) : nullptr;
    if (!cls) return nullptr;
    if (cls == g_equipMeasuredFor) return g_equip.ok ? &g_equip : nullptr;
    if (!SR::Layout()) return nullptr;   // the record must be measured first; retried next call
    g_equipMeasuredFor = cls;
    g_equip = EquipLayout{};
    EquipLayout L;
    L.offEquipment = R::FindPropertyOffset(cls, L"equipment");
    R::ArrayInnerInfo eq;
    const bool eqOk = L.offEquipment >= 0 && R::ArrayPropertyInner(cls, L"equipment", eq) &&
                      MeasureEquipStruct(eq.innerStruct, eq.elementSize, L);
    if (eqOk) {
        R::ArrayInnerInfo hd;
        const int32_t offHold = R::FindPropertyOffset(cls, L"hold");
        if (offHold >= 0 && R::ArrayPropertyInner(cls, L"hold", hd) && hd.innerStruct == eq.innerStruct &&
            hd.elementSize == eq.elementSize)
            L.offHold = offHold;
    }
    if (!eqOk) {
        // INFO, not a warning: this is the 0.9.0n fallback, which 0.10.0 never uses (its inventory is the
        // component's own bytes, ue_wrap/actors/inventory_component), so on 0.10.0 it is expected.
        UE_LOGI("inventory: %ls.equipment does not read as an array of {propDynamic, record, tag} "
                "(this build files equipment differently) -- the worn and held halves are NOT read or "
                "written; the carried half is unaffected (this line prints once per save class)",
                R::ToString(R::NameOf(cls)).c_str());
        return nullptr;
    }
    L.ok = true;
    g_equip = L;
    UE_LOGI("inventory: equipment measured -- equipment@0x%X hold@%d stride=0x%X data@0x%X name@%d key@%d tag@%d",
            L.offEquipment, L.offHold, L.stride, L.data, L.propName, L.propKey, L.tag);
    return &g_equip;
}

// The player's GObjStack slot (0.9.0n). Baked, not restored: prop_inventoryContainer_player_C's
// component template serializes index=0, and the saveSlot CDO pre-seeds the slot. The live reader
// checks the running component against it.
constexpr int32_t kPersonalSlot    = 0;

int32_t g_offSave = -1;

// ---- live personal store: reflected offsets (resolved once; -1 = looked and failed) ----------
// -2 = not looked at yet. Never guessed: an unresolvable offset makes the reader inert, it does
// not fall back to a hardcoded number.
int32_t g_offPlayerContainer  = -2;  // mainGamemode_C.playerContainer  -> Aprop_inventoryContainer_player_C*
int32_t g_offContainerPropInv = -2;  // prop_container_C.propInventory  -> UpropInventory_C*
int32_t g_offInvPlayer        = -2;  // propInventory_C.Player          -> the personal discriminator
int32_t g_offInvIndex         = -2;  // propInventory_C.Index           -> the GObjStack slot

int32_t CachedOffset(int32_t& slot, void* cls, const wchar_t* name) {
    if (slot == -2) {
        slot = cls ? R::FindPropertyOffset(cls, name) : -1;
        if (slot < 0)
            UE_LOGI("inventory: could not resolve %ls -- the live-store reader is inert (the 0.9.0n store; "
                    "0.10.0 does not have it)", name);
    }
    return slot;
}

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

void ReadEquipRecord(const EquipLayout& L, const void* base, EquipRecord& rec) {
    if (L.propName >= 0) rec.propName = SR::ReadFNameAt(base, L.propName);
    if (L.propKey >= 0)  rec.propKey  = SR::ReadFNameAt(base, L.propKey);
    SR::ReadSaveRecord(reinterpret_cast<const uint8_t*>(base) + L.data, rec.data);
    if (L.tag >= 0)      rec.tag      = SR::ReadFNameAt(base, L.tag);
}

// The player's contents array, addressed ONLY through the assertions that prove it is the
// personal store: the container component, its `Player` flag, and its running Index against the
// baked slot. Returns the slot index and fills `contents` and the engine's record stride, or -1
// having read nothing.
//
// Both readers go through it. The count reader used to address GObjStack[0] directly, which is
// the same slot on every build measured -- but "the same slot in practice" is what an address
// assertion exists to stop a reader from relying on, and the count is what the quick-slot edge
// makes its decision on.
int32_t ValidatedPersonalSlot(SR::Arr& contents, int32_t* recordStride = nullptr) {
    void* save = ResolveSaveSlot();
    void* gm = world_singleton::Gamemode();
    if (!save || !gm) return -1;

    if (CachedOffset(g_offPlayerContainer, R::ClassOf(gm), L"playerContainer") < 0) return -1;
    void* pc = ReadAt<void*>(gm, g_offPlayerContainer);
    if (!pc || !SR::PlausibleObjPtr(pc) || !R::IsLive(pc)) return -1;

    if (CachedOffset(g_offContainerPropInv, R::ClassOf(pc), L"propInventory") < 0) return -1;
    void* inv = ReadAt<void*>(pc, g_offContainerPropInv);
    if (!inv || !SR::PlausibleObjPtr(inv) || !R::IsLive(inv)) return -1;

    // ADDRESS ASSERTION (fail-closed): this must be the PERSONAL store and nothing else. Same flag
    // that container_contents_sync's BOUNDARY 1 refuses on -- opposite sides of one boundary.
    if (CachedOffset(g_offInvPlayer, R::ClassOf(inv), L"Player") < 0) return -1;
    if (ReadAt<uint8_t>(inv, g_offInvPlayer) == 0) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("inventory: playerContainer.propInventory is NOT flagged personal (Player==0) "
                    "-- refusing to read it as the live personal store. Expected player=True from "
                    "the class's component template; something resolved to the wrong container.");
        }
        return -1;
    }

    if (CachedOffset(g_offInvIndex, R::ClassOf(inv), L"Index") < 0) return -1;
    const int32_t idx = ReadAt<int32_t>(inv, g_offInvIndex);
    if (idx < 0) return -1;  // -1 = never initialised
    if (idx != kPersonalSlot) {
        // A different running index means the baked slot and the game disagree about where the
        // player's items live.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGE("inventory: the player container runs on GObjStack[%d], not the baked slot %d",
                    idx, kPersonalSlot);
        }
    }

    SR::GObjStackView stack;
    if (!SR::ReadGObjStack(save, stack)) return -1;
    const uint8_t* slot = stack.Slot(idx);
    if (!slot) return -1;

    // The struct_mObject element's single field is the contents TArray<Fstruct_save> at +0.
    contents = SR::ReadArr(slot, 0);
    if (recordStride) *recordStride = stack.recordStride;
    return idx;
}

}  // namespace

void* ResolveSaveSlot() {
    void* gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (g_offSave < 0) g_offSave = world_singleton::GamemodeSaveSlotOffset(gm);
    if (g_offSave < 0) return nullptr;
    void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSave);
    return (save && R::IsLive(save)) ? save : nullptr;
}

std::wstring ReadHeldItemName() {
    void* save = ResolveSaveSlot();
    const EquipLayout* L = EquipLayoutOf(save);
    if (!save || !L || L->offHold < 0 || L->propName < 0) return {};
    const SR::Arr hd = SR::ReadArr(save, L->offHold);
    if (!hd.data || hd.num < 1) return {};
    std::wstring name = SR::ReadFNameAt(hd.data, L->propName);
    if (name == L"None") name.clear();
    return name;
}

bool WriteHeldItem(const std::wstring& name, const wchar_t* classLeaf) {
    void* save = ResolveSaveSlot();
    const EquipLayout* L = EquipLayoutOf(save);
    if (!save || !L || L->offHold < 0 || L->propName < 0) return false;
    const SR::Arr hd = SR::ReadArr(save, L->offHold);
    if (!hd.data || hd.num < 1) return false;
    if (classLeaf && !R::FindClass(classLeaf)) return false;
    auto* slot = const_cast<uint8_t*>(hd.data);
    // The record as the game's own pickup leaves it, as far as the hand reads it: the class updateHold
    // spawns, and the name the spawned actor's loadData takes, its first names entry, which
    // propToDynamic also made the slot's own name from; an emptied hand's has neither. The rest of the
    // record stays, and its old buffers are orphaned, as every rewrite of one is.
    SR::SaveRecord rec;
    if (!SR::ReadSaveRecord(slot + L->data, rec)) return false;
    rec.className = classLeaf ? classLeaf : L"";
    if (classLeaf) rec.names.assign(1, std::vector<std::wstring>{name});
    else rec.names.clear();
    // Zeroed over the record's OWN measured span, inside the element the layout proved holds it.
    std::memset(slot + L->data, 0, static_cast<size_t>(SR::RecordSpan()));
    if (!SR::WriteSaveRecord(slot + L->data, rec)) return false;
    SR::WriteFNameField(slot + L->propName, name);
    return true;
}

bool ReadAll(PlayerInventory& out) {
    out.inventory.clear();
    out.equipment.clear();
    out.hold.clear();
    LivePersonalStore live;
    if (!ReadLivePersonalStore(live)) return false;  // pre-world: no container, nothing carried yet
    void* save = ResolveSaveSlot();
    if (!save) return false;
    out.inventory = std::move(live.records);
    const EquipLayout* L = EquipLayoutOf(save);
    if (!L) return true;   // this build's equipment is not a record array: the carried half only
    auto readArr = [&](int32_t off, std::vector<EquipRecord>& dst) {
        if (off < 0) return;
        const SR::Arr a = SR::ReadArr(save, off);
        dst.reserve(static_cast<size_t>(a.num));
        for (int32_t i = 0; i < a.num; ++i) {
            EquipRecord r;
            ReadEquipRecord(*L, a.data + static_cast<size_t>(i) * static_cast<size_t>(L->stride), r);
            dst.push_back(std::move(r));
        }
    };
    readArr(L->offEquipment, out.equipment);
    readArr(L->offHold, out.hold);
    return true;
}

int32_t LivePersonalStoreCount() {
    SR::Arr contents{};
    return ValidatedPersonalSlot(contents) < 0 ? -1 : contents.num;
}

bool ReadLivePersonalStore(LivePersonalStore& out) {
    out.slotIndex = -1;
    out.records.clear();
    SR::Arr contents{};
    int32_t stride = 0;
    const int32_t idx = ValidatedPersonalSlot(contents, &stride);
    if (idx < 0 || stride <= 0) return false;
    out.slotIndex = idx;
    out.records.reserve(static_cast<size_t>(contents.num));
    for (int32_t i = 0; i < contents.num; ++i) {
        SR::SaveRecord r;
        SR::ReadSaveRecord(contents.data + static_cast<size_t>(i) * static_cast<size_t>(stride), r);
        out.records.push_back(std::move(r));
    }
    return true;
}

}  // namespace ue_wrap::inventory
