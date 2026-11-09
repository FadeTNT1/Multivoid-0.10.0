// ue_wrap/desk/desk_press.cpp -- see ue_wrap/desk/desk_press.h.

#include "ue_wrap/desk/desk_press.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"

#include <unordered_map>

namespace ue_wrap::desk_press {
namespace {

namespace R = reflection;

// A multicast delegate's invocation list, the TArray a Blueprint event dispatcher holds inline.
struct ListHeader {
    void*   data = nullptr;
    int32_t num = 0;
    int32_t max = 0;
};

// The desk's and its game mode's members a press reads, each offset kept per class.
R::InstanceOffset g_playerUsing{L"player_using"};
R::InstanceOffset g_lookingAtPanel{L"lookingAtPanel"};
R::InstanceOffset g_componentButton{L"component_button"};
R::InstanceOffset g_gamemode{L"gamemode"};
R::InstanceOffset g_mainPlayer{L"mainPlayer"};
R::InstanceOffset g_selectIndex{L"play_selectIndex"};

uint8_t* Field(void* obj, R::InstanceOffset& member) {
    const int32_t off = obj ? member.Of(obj) : -1;
    return off < 0 ? nullptr : static_cast<uint8_t*>(obj) + off;
}

// The object in `obj`'s object member, live, or null.
void* ObjectField(void* obj, R::InstanceOffset& member) {
    uint8_t* f = Field(obj, member);
    void* o = f ? *reinterpret_cast<void**>(f) : nullptr;
    return (o && R::IsLive(o)) ? o : nullptr;
}

// The machine's player's two scroll-wheel dispatchers, each an inline multicast delegate whose whole value is its
// invocation TArray: checked by size once per player class before any header of theirs is moved.
bool ScrollLists(void* player, ListHeader*& down, ListHeader*& up) {
    struct Known {
        void*   cls = nullptr;
        int32_t idx = -1, serial = 0, down = -1, up = -1;
    };
    static Known k;
    void* cls = R::ClassOf(player);
    if (!cls) return false;
    if (cls != k.cls || !R::IsLiveByIndex(k.cls, k.idx) || R::SlotSerial(k.idx) != k.serial) {
        k = Known{};
        k.cls = cls;
        k.idx = R::InternalIndexOf(cls);
        k.serial = R::AllocateSlotSerial(k.idx);
        for (const R::StructFieldInfo& f : R::EnumerateStructFields(cls)) {
            if (f.size != static_cast<int32_t>(sizeof(ListHeader))) continue;
            if (f.name == L"input_scrollDown") k.down = f.offset;
            else if (f.name == L"input_scrollUp") k.up = f.offset;
        }
    }
    if (k.down < 0 || k.up < 0) return false;
    down = reinterpret_cast<ListHeader*>(static_cast<uint8_t*>(player) + k.down);
    up = reinterpret_cast<ListHeader*>(static_cast<uint8_t*>(player) + k.up);
    return true;
}

}  // namespace

void* Member(void* desk, const wchar_t* member) {
    static std::unordered_map<const wchar_t*, R::InstanceOffset> sOffsets;
    auto it = sOffsets.find(member);
    if (it == sOffsets.end()) it = sOffsets.emplace(member, R::InstanceOffset{member}).first;
    return ObjectField(desk, it->second);
}

bool Press(void* desk, void* player, void* component) {
    void* cls = desk ? R::ClassOf(desk) : nullptr;
    void* lookFn = cls ? R::FindDispatchFunctionCached(cls, L"lookAt") : nullptr;
    void* pressFn = cls ? R::FindDispatchFunctionCached(cls, L"actionOptionIndex") : nullptr;
    if (!lookFn || !pressFn || !component || !player) {
        UE_LOGW("desk_press: press not made (lookAt=%d actionOptionIndex=%d component=%d player=%d)",
                lookFn ? 1 : 0, pressFn ? 1 : 0, component ? 1 : 0, player ? 1 : 0);
        return false;
    }
    const FVector at = engine::GetComponentLocation(component);
    ParamFrame look(lookFn);
    if (!look.valid() || !look.Set<void*>(L"player", player) ||
        !hit_result::Write(look, L"hit", desk, component, at) || !Call(desk, look)) {
        UE_LOGW("desk_press: press not made: the desk's lookAt with a hit on the component did not run");
        return false;
    }
    struct { void* data; int32_t num; int32_t max; } text{};
    if (look.GetRaw(L"text", &text, sizeof(text)) && text.data) R::EngineFree(text.data);  // the engine wrote it
    ParamFrame press(pressFn);
    if (!press.valid() || !press.Set<void*>(L"player", player) ||
        !hit_result::Write(press, L"hit", desk, component, at) ||
        !press.Set<void*>(L"lookAtComponent", component) || !Call(desk, press)) {
        UE_LOGW("desk_press: press not made: actionOptionIndex with a hit on the component did not run");
        return false;
    }
    return true;
}

bool PressForAnother(void* desk, void* player, void* component, int* dropped) {
    if (dropped) *dropped = 0;
    uint8_t* usingField = Field(desk, g_playerUsing);
    uint8_t* panelField = Field(desk, g_lookingAtPanel);
    // The button the desk last took a press on. It is the desk's ONE copy: actionOptionIndex writes it from the hit
    // (:2355) and the held-button handlers read it afterwards (addFilterParam per scroll step, updText), so a
    // replayed press that left its button here would redirect a hold the machine's own player is still making.
    uint8_t* buttonField = Field(desk, g_componentButton);
    // The machine's own player, as the press's compare chain reaches it: gamemode->mainPlayer. None (a player
    // between lives) has no bindings to keep, and the press's own read of it then touches nothing.
    void* gamemode = ObjectField(desk, g_gamemode);
    void* own = gamemode ? ObjectField(gamemode, g_mainPlayer) : nullptr;
    ListHeader* lists[2] = {nullptr, nullptr};
    if (own && !ScrollLists(own, lists[0], lists[1])) lists[0] = lists[1] = nullptr;
    if (!usingField || !panelField || !buttonField || (own && (!lists[0] || !lists[1]))) {
        UE_LOGW("desk_press: press for another player not made: player_using=%d lookingAtPanel=%d "
                "component_button=%d, and the machine's player %p has scroll lists %d/%d", usingField ? 1 : 0,
                panelField ? 1 : 0, buttonField ? 1 : 0, own, lists[0] ? 1 : 0, lists[1] ? 1 : 0);
        return false;
    }
    void* const heldUsing = *reinterpret_cast<void**>(usingField);
    void* const heldButton = *reinterpret_cast<void**>(buttonField);
    const int32_t heldPanel = *reinterpret_cast<int32_t*>(panelField);
    ListHeader held[2];
    for (int i = 0; i < 2; ++i) {
        if (!lists[i]) continue;
        held[i] = *lists[i];
        *lists[i] = ListHeader{};
    }
    const bool pressed = Press(desk, player, component);
    for (int i = 0; i < 2; ++i) {
        if (!lists[i]) continue;
        if (lists[i]->data) {
            if (dropped) *dropped += lists[i]->num;
            R::EngineFree(lists[i]->data);
        }
        *lists[i] = held[i];
    }
    *reinterpret_cast<void**>(usingField) = heldUsing;
    *reinterpret_cast<int32_t*>(panelField) = heldPanel;
    *reinterpret_cast<void**>(buttonField) = heldButton;
    return pressed;
}

int32_t SelectedRow(void* desk) {
    uint8_t* f = Field(desk, g_selectIndex);
    return f ? *reinterpret_cast<int32_t*>(f) : -1;
}

bool SelectRow(void* desk, int32_t index) {
    uint8_t* f = Field(desk, g_selectIndex);
    if (!f) return false;
    *reinterpret_cast<int32_t*>(f) = index;
    return true;
}

void* Pressed(void* pressFunction, const uint8_t* locals) {
    const int32_t off = (pressFunction && locals) ? R::FindParamOffset(pressFunction, L"hit") : -1;
    return off < 0 ? nullptr : hit_result::Component(locals + off);
}

}  // namespace ue_wrap::desk_press
