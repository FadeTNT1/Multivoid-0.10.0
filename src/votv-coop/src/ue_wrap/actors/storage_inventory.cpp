// ue_wrap/actors/storage_inventory.cpp -- see ue_wrap/actors/storage_inventory.h.

#include "ue_wrap/actors/storage_inventory.h"

#include "ue_wrap/actors/inventory_item_codec.h"
#include "ue_wrap/actors/save_record.h"     // ReadArr, AllocZeroed, WriteArrHeader
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/engine/engine_widget.h"   // SpawnUObject
#include "ue_wrap/world/world_singleton.h"

#include <cstring>

namespace ue_wrap::storage_inventory {
namespace {

namespace R = ue_wrap::reflection;
namespace N = ue_wrap::profile::name;
namespace SR = ue_wrap::save_record;
namespace IC = ue_wrap::inventory_item_codec;

R::InstanceOffset g_offStorage(N::ContainerInventoryComponentProp);
R::InstanceOffset g_offOwner(N::InventoryComponentOwnerProp);
R::InstanceOffset g_offCanvas(N::InventoryCarriedArray);
R::InstanceOffset g_offMaxVol(N::InventoryMaxVolumeProp);
R::InstanceOffset g_offTotalVol(N::InventoryTotalVolumeProp);
R::InstanceOffset g_offActiveCanvas(N::InventoryActiveCanvasProp);
R::InstanceOffset g_offScreen(N::GamemodeInventoryScreenProp);
R::InstanceOffset g_offScreenStorage(N::ScreenContainerComponentProp);
R::InstanceOffset g_offWidgetOwner(N::ItemWidgetOwnerComponentProp);
R::InstanceOffset g_offWidgetIndex(N::ItemWidgetIndexProp);
R::InstanceOffset g_offCanvasComponent(N::CanvasConnectedComponentProp);
R::InstanceOffset g_offDragging(N::ScreenDraggingProp);
R::InstanceOffset g_offPlayerCanvas(N::ScreenPlayerCanvasProp);
R::InstanceOffset g_offWidgetCanvas(N::ItemWidgetOwnerCanvasProp);

constexpr const wchar_t* kSlotArrays[] = {N::InventoryHandsArray, N::InventoryEquipmentArray, N::InventorySlotArrayC,
                                          N::InventorySlotArrayD};

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

bool IsA(void* obj, const wchar_t* className) {
    void* base = obj ? ue_wrap::object_index::ClassByName(className) : nullptr;
    return base && R::IsDescendantOfAny(R::ClassOf(obj), &base, 1);
}

void* LiveObjAt(void* base, int32_t off) {
    if (!base || off < 0) return nullptr;
    void* p = ReadAt<void*>(base, off);
    return (p && SR::PlausibleObjPtr(p) && R::IsLive(p)) ? p : nullptr;
}

// A bitfield bool by name on an object's class, kept for the last class asked.
struct BoolField {
    const wchar_t* name;
    void* cls = nullptr;
    int32_t off = -1;
    uint8_t mask = 0;
    bool Read(void* obj, bool& out) {
        if (!obj) return false;
        void* const c = R::ClassOf(obj);
        if (c != cls) {
            cls = c;
            if (!R::FindBoolProperty(c, name, off, mask)) off = -1;
        }
        if (off < 0 || mask == 0) return false;
        out = (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
        return true;
    }
    bool Write(void* obj, bool value) {
        bool unused = false;
        if (!Read(obj, unused)) return false;
        uint8_t& byte = *(static_cast<uint8_t*>(obj) + off);
        byte = value ? static_cast<uint8_t>(byte | mask) : static_cast<uint8_t>(byte & ~mask);
        return true;
    }
};

// The canvas array of a component, read; null data when it does not resolve.
SR::Arr Canvas(void* comp) {
    const int32_t off = g_offCanvas.Of(comp);
    return off < 0 ? SR::Arr{} : SR::ReadArr(comp, off);
}

uint8_t* RowAt(void* comp, int32_t index) {
    const SR::Arr a = Canvas(comp);
    const int32_t stride = IC::RowStride();
    if (!a.data || stride <= 0 || index < 0 || index >= a.num) return nullptr;
    return const_cast<uint8_t*>(a.data) + static_cast<size_t>(index) * static_cast<size_t>(stride);
}

void* Fn(void* obj, const wchar_t* name) {
    return obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), name) : nullptr;
}

bool CalculateVolumes(void* comp) {
    void* const fn = Fn(comp, N::InventoryCalculateVolumesFn);
    if (!fn) return false;
    ParamFrame f(fn);
    const bool no = false, yes = true;
    f.SetRaw(L"countSlotBar_A", &no, 1);
    f.SetRaw(L"updateVisible", &yes, 1);
    f.SetRaw(L"recalucateMaxVolume", &yes, 1);
    return Call(comp, f);
}

// A component of the base class, made by the engine and owned by nobody: the collector destroys it, and every row
// decoded into it, once the call that made it has returned. Its outer is the game instance, never an actor, so no
// actor lists it among its components.
void* SpawnScratch() {
    void* const cls = ue_wrap::object_index::ClassByName(N::InventoryComponentClass);
    void* const outer = ue_wrap::world_singleton::GameInstance();
    if (!cls || !outer) return nullptr;
    void* const s = ue_wrap::engine::SpawnUObject(cls, outer);
    if (!s || Canvas(s).num != 0) return nullptr;   // a fresh component holds no row
    return s;
}

// Decode `rows` into a scratch component's canvas. False on a malformed row; the scratch, and what was built in it,
// is the collector's either way.
void* DecodeToScratch(const std::vector<std::vector<uint8_t>>& rows) {
    void* const s = SpawnScratch();
    const int32_t off = s ? g_offCanvas.Of(s) : -1;
    const int32_t stride = IC::RowStride();
    if (!s || off < 0 || stride <= 0) return nullptr;
    if (rows.empty()) return s;
    void* const buf = SR::AllocZeroed(rows.size(), static_cast<size_t>(stride));
    if (!buf) return nullptr;
    SR::WriteArrHeader(s, off, buf, static_cast<int32_t>(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i) {
        size_t o = 0;
        uint8_t* const row = static_cast<uint8_t*>(buf) + i * static_cast<size_t>(stride);
        if (!IC::Decode(rows[i], o, row) || o != rows[i].size() || IC::IsEmpty(row)) return nullptr;
    }
    return s;
}

// The open inventory screen, or null when it is closed.
void* OpenScreen() {
    static BoolField sActive{N::ScreenActiveProp};
    void* const gm = ue_wrap::world_singleton::Gamemode();
    void* const screen = gm ? LiveObjAt(gm, g_offScreen.Of(gm)) : nullptr;
    bool active = false;
    return (screen && sActive.Read(screen, active) && active) ? screen : nullptr;
}

// The screen's canvas that draws `comp` (its activeDisplayCanvas, bound back to it), while the screen is open: unbound
// before the rows change, since unbinding writes each widget's place back into the row it was drawn from. A canvas
// bound since to another component is not this one's to unbind.
void* ShownCanvasOf(void* comp) {
    if (!comp || !OpenScreen()) return nullptr;
    void* const canvas = LiveObjAt(comp, g_offActiveCanvas.Of(comp));
    return (canvas && LiveObjAt(canvas, g_offCanvasComponent.Of(canvas)) == comp) ? canvas : nullptr;
}

bool Despawn(void* canvas) {
    void* const fn = Fn(canvas, N::CanvasDespawnFn);
    if (!fn) return false;
    ParamFrame f(fn);
    return Call(canvas, f);
}

bool Assign(void* comp, void* canvas) {
    void* const fn = Fn(comp, N::InventoryAssignCanvasFn);
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<void*>(L"activeDisplayCanvas", canvas);
    return Call(comp, f);
}

// Re-derive after an in-place write: the volumes, and the screen's widgets when it shows the component.
void Rederive(void* comp) {
    if (void* const canvas = ShownCanvasOf(comp)) {
        if (Despawn(canvas) && Assign(comp, canvas)) return;
    }
    CalculateVolumes(comp);
}

// `itemOut`, when given, receives the item widget the add drew (null when the component draws none).
bool AddRowRaw(void* comp, const uint8_t* row, bool keepPlace, void** itemOut = nullptr) {
    void* const fn = Fn(comp, N::InventoryAddItemAdvancedFn);
    const int32_t stride = IC::RowStride();
    if (!fn || stride <= 0 || R::FindParamSize(fn, L"data") != stride) return false;
    float pos[2] = {0.f, 0.f}, rot = 0.f;
    IC::Placement(row, pos[0], pos[1], rot);
    ParamFrame f(fn);
    const bool random = !keepPlace, place = keepPlace;
    const float scale = 0.8f;   // the game's own spread for an item added at a random place
    f.SetRaw(L"data", row, stride);   // a value parameter: the body copies it into the canvas
    f.SetRaw(L"randomPositionOnCanvas", &random, 1);
    f.Set<float>(L"randScale", scale);
    f.SetRaw(L"setPositionAndRotation", &place, 1);
    f.SetRaw(L"itemPosition", pos, sizeof(pos));
    f.Set<float>(L"itemRotation", rot);
    if (!Call(comp, f)) return false;
    if (!f.Get<bool>(L"success_volume") || !f.Get<bool>(L"validClass")) return false;
    if (itemOut) {
        void* const widget = f.Get<void*>(L"item");
        *itemOut = (widget && SR::PlausibleObjPtr(widget) && R::IsLive(widget)) ? widget : nullptr;
    }
    return true;
}

bool RemoveAt(void* comp, bool slotBar, uint8_t slotIndex, int32_t index) {
    void* const fn = Fn(comp, N::InventoryRemoveItemFn);
    if (!fn) return false;
    ParamFrame f(fn);
    f.Set<int32_t>(L"indexToRemove", index);
    f.SetRaw(L"slotBar", &slotBar, 1);
    f.SetRaw(L"slotBarIndex", &slotIndex, 1);
    if (!Call(comp, f)) return false;
    return f.Get<bool>(L"success");
}

}  // namespace

void* StorageOf(void* actor) {
    if (!actor || !R::IsLive(actor)) return nullptr;
    void* const comp = LiveObjAt(actor, g_offStorage.Of(actor));
    return IsA(comp, N::InventoryComponentClass) ? comp : nullptr;
}

void* OwnerOf(void* component) {
    return (component && R::IsLive(component)) ? LiveObjAt(component, g_offOwner.Of(component)) : nullptr;
}

bool IsWorldStorage(void* component) {
    if (!component || !R::IsLive(component) || !IsA(component, N::InventoryComponentClass)) return false;
    if (IsA(component, N::PlayerInventoryComponentClass)) return false;
    void* const owner = OwnerOf(component);
    if (!owner || IsA(owner, N::MainPlayerClass)) return false;
    return StorageOf(owner) == component;
}

void* DroneStorage() {
    void* const drone = ue_wrap::drone::Find();
    return drone ? StorageOf(drone) : nullptr;
}

bool IsDrone(void* actor) {
    return actor && IsA(actor, N::DroneClass);
}

bool ReadRows(void* component, std::vector<Row>& out, size_t maxRows) {
    out.clear();
    if (!component || !IC::Ready()) return false;
    const SR::Arr a = Canvas(component);
    const int32_t stride = IC::RowStride();
    if (g_offCanvas.Of(component) < 0 || stride <= 0) return false;
    for (const wchar_t* name : kSlotArrays) {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(component), name);
        if (off >= 0 && SR::ReadArr(component, off).num > 0) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                UE_LOGW("storage_inventory: a world storage (%ls) holds rows in %ls -- only its canvas is carried",
                        R::ClassNameOf(component).c_str(), name);
            }
        }
    }
    for (int32_t i = 0; i < a.num; ++i) {
        const uint8_t* const row = a.data + static_cast<size_t>(i) * static_cast<size_t>(stride);
        if (IC::IsEmpty(row)) continue;
        if (out.size() >= maxRows) return false;
        Row r;
        r.index = i;
        r.amount = IC::Amount(row);
        r.fingerprint = IC::Fingerprint(row);
        if (!r.fingerprint || !IC::Encode(row, r.bytes)) return false;
        out.push_back(std::move(r));
    }
    return true;
}

ApplyResult ApplyRows(void* component, const std::vector<std::vector<uint8_t>>& rows) {
    if (!component || !IC::Ready()) return ApplyResult::Unresolved;
    void* const scratch = DecodeToScratch(rows);
    if (!scratch) return ApplyResult::Malformed;
    void* const transfer = Fn(scratch, N::InventoryTransferFn);
    if (!transfer) return ApplyResult::Unresolved;
    void* const canvas = ShownCanvasOf(component);
    if (canvas) Despawn(canvas);
    ParamFrame f(transfer);
    f.Set<void*>(L"to", component);
    if (!Call(scratch, f)) return ApplyResult::Unresolved;
    if (!canvas || !Assign(component, canvas)) CalculateVolumes(component);
    return ApplyResult::Done;
}

int32_t FindRow(void* component, uint64_t fingerprint, uint8_t minAmount) {
    const SR::Arr a = Canvas(component);
    const int32_t stride = IC::RowStride();
    if (!a.data || stride <= 0 || !fingerprint) return -1;
    for (int32_t i = 0; i < a.num; ++i) {
        const uint8_t* const row = a.data + static_cast<size_t>(i) * static_cast<size_t>(stride);
        if (!IC::IsEmpty(row) && IC::Amount(row) >= minAmount && IC::Fingerprint(row) == fingerprint) return i;
    }
    return -1;
}

bool EncodeRowAt(void* component, int32_t index, std::vector<uint8_t>& out, int amountOverride) {
    const uint8_t* const row = RowAt(component, index);
    return row && !IC::IsEmpty(row) && IC::Encode(row, out, IC::Mode::Full, amountOverride);
}

bool RemoveRowAt(void* component, int32_t index) {
    return RowAt(component, index) && RemoveAt(component, false, 0, index);
}

bool ReduceRowAt(void* component, int32_t index, uint8_t amount) {
    uint8_t* const row = RowAt(component, index);
    if (!row || IC::IsEmpty(row) || IC::Amount(row) <= amount) return false;
    IC::SetAmount(row, static_cast<uint8_t>(IC::Amount(row) - amount));
    Rederive(component);
    return true;
}

bool AddRow(void* component, const std::vector<uint8_t>& row, bool keepPlace) {
    if (!component || !IC::Ready()) return false;
    void* const scratch = DecodeToScratch({row});
    const SR::Arr a = scratch ? Canvas(scratch) : SR::Arr{};
    return a.num == 1 && AddRowRaw(component, a.data, keepPlace);
}

bool PeekRow(const std::vector<uint8_t>& row, std::wstring& name, uint8_t& amount, uint64_t& fingerprint) {
    if (!IC::Ready()) return false;
    void* const scratch = DecodeToScratch({row});
    const SR::Arr a = scratch ? Canvas(scratch) : SR::Arr{};
    if (a.num != 1) return false;
    name = IC::Name(a.data);
    amount = IC::Amount(a.data);
    fingerprint = IC::Fingerprint(a.data);
    return fingerprint != 0;
}

bool ReadTerms(void* component, Terms& out) {
    static BoolField sInfinite{N::InventoryInfiniteVolumeProp}, sLocked{N::ContainerLockedProp};
    out = Terms{};
    const int32_t maxOff = g_offMaxVol.Of(component), totalOff = g_offTotalVol.Of(component);
    if (!component || maxOff < 0 || totalOff < 0 || !sInfinite.Read(component, out.infinite)) return false;
    out.maxVolume = ReadAt<float>(component, maxOff);
    out.totalVolume = ReadAt<float>(component, totalOff);
    bool locked = false;
    if (void* const owner = OwnerOf(component); owner && sLocked.Read(owner, locked)) out.locked = locked;
    return true;
}

bool TakeFromPlayer(void* playerComponent, uint64_t fingerprint, uint8_t amount) {
    if (!playerComponent || !fingerprint || !IC::Ready()) return false;
    const int32_t stride = IC::RowStride();
    // The canvas first (a dragged item lives there), then the hands, the equipment and the other slot rows.
    const int32_t canvasOff = g_offCanvas.Of(playerComponent);
    const wchar_t* const arrays[] = {N::InventoryCarriedArray, N::InventoryHandsArray, N::InventoryEquipmentArray,
                                     N::InventorySlotArrayC, N::InventorySlotArrayD};
    for (size_t k = 0; k < sizeof(arrays) / sizeof(arrays[0]); ++k) {
        const int32_t off = k == 0 ? canvasOff : R::FindPropertyOffset(R::ClassOf(playerComponent), arrays[k]);
        if (off < 0) continue;
        const SR::Arr a = SR::ReadArr(playerComponent, off);
        for (int32_t i = 0; i < a.num; ++i) {
            uint8_t* const row = const_cast<uint8_t*>(a.data) + static_cast<size_t>(i) * static_cast<size_t>(stride);
            if (IC::IsEmpty(row) || IC::Amount(row) < amount || IC::Fingerprint(row) != fingerprint) continue;
            if (IC::Amount(row) > amount) {
                IC::SetAmount(row, static_cast<uint8_t>(IC::Amount(row) - amount));
                Rederive(playerComponent);
                return true;
            }
            return RemoveAt(playerComponent, k != 0, static_cast<uint8_t>(k == 0 ? 0 : k - 1), i);
        }
    }
    return false;
}

bool PeekPlacement(const std::vector<uint8_t>& row, float& x, float& y, float& rotation) {
    if (!IC::Ready()) return false;
    void* const scratch = DecodeToScratch({row});
    const SR::Arr a = scratch ? Canvas(scratch) : SR::Arr{};
    return a.num == 1 && IC::Placement(a.data, x, y, rotation);
}

bool GiveToPlayer(void* playerComponent, const std::vector<uint8_t>& row, void*& widget) {
    widget = nullptr;
    if (!playerComponent || !IC::Ready()) return false;
    void* const scratch = DecodeToScratch({row});
    const SR::Arr a = scratch ? Canvas(scratch) : SR::Arr{};
    if (a.num != 1) return false;
    // With the screen open and nothing held, the row goes where the game's own beginDrag puts a container item that
    // becomes the held one: at the place its data carries. Otherwise it lands at a random free place.
    void* const screen = OpenScreen();
    const bool resumable = screen && !LiveObjAt(screen, g_offDragging.Of(screen));
    return AddRowRaw(playerComponent, a.data, /*keepPlace=*/resumable, &widget);
}

HeldDrag ResumeDrag(void* widget) {
    static BoolField sRmb{N::ScreenDragRmbProp}, sCtrl{N::ScreenStackCtrlProp};
    static BoolField sHoldLmb{N::ScreenHoldLmbProp}, sHoldRmb{N::ScreenHoldRmbProp};
    void* const screen = OpenScreen();
    if (!screen || !widget || !R::IsLive(widget)) return HeldDrag::Stored;
    if (LiveObjAt(screen, g_offDragging.Of(screen))) return HeldDrag::Stored;
    // beginDrag keeps a widget of the player's own canvas as the held one; any other widget it would add again.
    void* const playerCanvas = LiveObjAt(screen, g_offPlayerCanvas.Of(screen));
    if (!playerCanvas || LiveObjAt(widget, g_offWidgetCanvas.Of(widget)) != playerCanvas) return HeldDrag::Stored;
    void* const begin = Fn(screen, N::ScreenBeginDragFn);
    if (!begin) return HeldDrag::Stored;
    // The host already split the stack the key asked for; beginDrag must not split the granted row again.
    bool rmb = false, ctrl = false;
    const bool hadRmb = sRmb.Read(screen, rmb), hadCtrl = sCtrl.Read(screen, ctrl);
    if (hadRmb) sRmb.Write(screen, false);
    if (hadCtrl) sCtrl.Write(screen, false);
    ParamFrame f(begin);
    f.Set<void*>(L"item", widget);
    const bool called = Call(screen, f);
    if (hadRmb) sRmb.Write(screen, rmb);
    if (hadCtrl) sCtrl.Write(screen, ctrl);
    if (!called || LiveObjAt(screen, g_offDragging.Of(screen)) != widget) return HeldDrag::Stored;
    bool lmb = false, holdRmb = false;
    const bool knowLmb = sHoldLmb.Read(screen, lmb), knowRmb = sHoldRmb.Read(screen, holdRmb);
    if ((!knowLmb && !knowRmb) || lmb || holdRmb) return HeldDrag::Held;
    // The button went up while the host answered: the drag ends where the cursor is now, as its release would have.
    void* const end = Fn(screen, N::ScreenEndDragFn);
    if (!end) return HeldDrag::Held;
    ParamFrame e(end);
    return Call(screen, e) ? HeldDrag::Dropped : HeldDrag::Held;
}

void* ShownStorage() {
    static BoolField sInContainer{N::ScreenInContainerProp};
    void* const screen = OpenScreen();
    if (!screen) return nullptr;
    bool inContainer = true;
    if (sInContainer.Read(screen, inContainer) && !inContainer) return nullptr;
    return LiveObjAt(screen, g_offScreenStorage.Of(screen));
}

bool IsInventoryScreen(void* obj) {
    return obj && IsA(obj, N::InventoryScreenClass);
}

bool BeginDragRow(const uint8_t* locals, void* function, void*& component, int32_t& index) {
    static R::ParamOffset sItem(L"item");
    component = nullptr;
    index = -1;
    const int32_t off = sItem.Of(function);
    void* const widget = (locals && off >= 0) ? ReadAt<void*>(locals, off) : nullptr;
    if (!widget || !SR::PlausibleObjPtr(widget) || !R::IsLive(widget)) return false;
    const int32_t idxOff = g_offWidgetIndex.Of(widget);
    component = LiveObjAt(widget, g_offWidgetOwner.Of(widget));
    if (!component || idxOff < 0) return false;
    index = ReadAt<int32_t>(widget, idxOff);
    return true;
}

uint8_t BeginDragAmount(uint8_t rowAmount) {
    static BoolField sRmb{N::ScreenDragRmbProp}, sCtrl{N::ScreenStackCtrlProp};
    void* const screen = OpenScreen();
    bool rmb = false, ctrl = false;
    if (screen) {
        sRmb.Read(screen, rmb);
        sCtrl.Read(screen, ctrl);
    }
    if (rowAmount <= 1 || !(rmb || ctrl)) return rowAmount;
    return ctrl ? static_cast<uint8_t>(rowAmount / 2) : uint8_t{1};
}

bool AddCallRow(const uint8_t* locals, void* function, std::vector<uint8_t>& row, uint64_t& fingerprint,
                uint8_t& amount, bool& keepPlace) {
    static R::ParamOffset sData(L"data"), sPlace(L"setPositionAndRotation"), sPos(L"itemPosition"),
        sRot(L"itemRotation");
    row.clear();
    const int32_t dataOff = sData.Of(function), placeOff = sPlace.Of(function);
    if (!locals || dataOff < 0 || placeOff < 0 || !IC::Ready()) return false;
    const uint8_t* const data = locals + dataOff;
    if (IC::IsEmpty(data)) return false;
    fingerprint = IC::Fingerprint(data);
    amount = IC::Amount(data);
    keepPlace = locals[placeOff] != 0;
    if (fingerprint == 0 || amount == 0) return false;
    if (!keepPlace) return IC::Encode(data, row);
    // The call names where the row lands (itemPosition, itemRotation) and addItemAdvanced overwrites the row's own
    // place with it: the encoded row carries the named place, which is what the host's add then keeps.
    const int32_t posOff = sPos.Of(function), rotOff = sRot.Of(function), stride = IC::RowStride();
    if (posOff < 0 || rotOff < 0 || stride <= 0) return false;
    std::vector<uint8_t> placed(data, data + stride);
    float pos[2], rot;
    std::memcpy(pos, locals + posOff, sizeof(pos));
    std::memcpy(&rot, locals + rotOff, sizeof(rot));
    return IC::SetPlacement(placed.data(), pos[0], pos[1], rot) && IC::Encode(placed.data(), row);
}

void* TransferCallTarget(const uint8_t* locals, void* function) {
    static R::ParamOffset sTo(L"to");
    const int32_t off = sTo.Of(function);
    void* const to = (locals && off >= 0) ? ReadAt<void*>(locals, off) : nullptr;
    return (to && SR::PlausibleObjPtr(to) && R::IsLive(to)) ? to : nullptr;
}

void* ItemWidgetComponent(void* itemWidget) {
    return (itemWidget && R::IsLive(itemWidget)) ? LiveObjAt(itemWidget, g_offWidgetOwner.Of(itemWidget)) : nullptr;
}

void* DoubleClickComponent(void* screen) {
    static R::InstanceOffset sObject(N::ScreenDoubleClickObjectProp);
    return (screen && R::IsLive(screen)) ? ItemWidgetComponent(LiveObjAt(screen, sObject.Of(screen))) : nullptr;
}

bool RowInfo(void* component, int32_t index, uint64_t& fingerprint, uint8_t& amount) {
    const uint8_t* const row = RowAt(component, index);
    if (!row || IC::IsEmpty(row)) return false;
    fingerprint = IC::Fingerprint(row);
    amount = IC::Amount(row);
    return fingerprint != 0;
}

}  // namespace ue_wrap::storage_inventory
