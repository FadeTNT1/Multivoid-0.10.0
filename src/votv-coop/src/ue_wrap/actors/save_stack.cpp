// ue_wrap/actors/save_stack.cpp -- saveSlot.GObjStack, measured. See ReadGObjStack in
// ue_wrap/actors/save_record.h.

#include "ue_wrap/actors/save_record.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <unordered_map>

namespace ue_wrap::save_record {
namespace {

namespace R = ue_wrap::reflection;

struct StackShape { bool ok = false; int32_t off = -1; int32_t outer = 0; int32_t record = 0; };
std::unordered_map<void*, StackShape> g_stacks;   // per save UClass; ResetGObjStackCache drops it

// Why `cls`'s GObjStack does not read as TArray<struct_mObject{ TArray<record> obj @ +0 }>, or null.
const char* MeasureStack(void* cls, const RecordLayout& rec, StackShape& s) {
    s.off = R::FindPropertyOffset(cls, L"GObjStack");
    if (s.off < 0) return "no GObjStack member";
    R::ArrayInnerInfo outer;
    if (!R::ArrayPropertyInner(cls, L"GObjStack", outer) || !outer.innerStruct)
        return "GObjStack is not an array of structs";
    const int32_t mspan = R::StructSize(outer.innerStruct);
    if (mspan < 16 || outer.elementSize < mspan || outer.elementSize > 0x100)
        return "implausible struct_mObject size";
    const auto fields = R::EnumerateStructFields(outer.innerStruct);
    if (fields.empty() || fields[0].offset != 0 || fields[0].size != 16)
        return "struct_mObject's first member is not a TArray at +0";
    R::ArrayInnerInfo obj;
    if (!R::ArrayPropertyInner(outer.innerStruct, fields[0].name.c_str(), obj) || !obj.innerStruct)
        return "struct_mObject's first member is not an array of structs";
    if (obj.innerStruct != rec.script)
        return "the `obj` element struct is not the measured record struct";
    if (!ConfirmRecordStride(rec.script, obj.elementSize))
        return "the `obj` element size disagrees with the record stride";
    s.outer = outer.elementSize;
    s.record = obj.elementSize;
    s.ok = true;
    return nullptr;
}

}  // namespace

void ResetGObjStackCache() { g_stacks.clear(); }

bool ReadGObjStack(void* save, GObjStackView& out) {
    out = GObjStackView{};
    void* cls = save ? R::ClassOf(save) : nullptr;
    if (!cls) return false;
    auto it = g_stacks.find(cls);
    if (it == g_stacks.end()) {
        const RecordLayout* rec = Layout();
        if (!rec) return false;                       // record not measured yet: retried next call
        StackShape s;
        if (const char* why = MeasureStack(cls, *rec, s))
            UE_LOGE("save_record: %ls.GObjStack does not read as an array of record arrays (%s) -- "
                    "every GObjStack walk and write is REFUSED (this line prints once per save class)",
                    R::ToString(R::NameOf(cls)).c_str(), why);
        else
            UE_LOGI("save_record: GObjStack measured -- outer stride=0x%X, record stride=0x%X",
                    s.outer, s.record);
        it = g_stacks.emplace(cls, s).first;
    }
    if (!it->second.ok) return false;
    const Arr stack = ReadArr(save, it->second.off);
    out.data = stack.data;
    out.num = stack.num;
    out.outerStride = it->second.outer;
    out.recordStride = it->second.record;
    return true;
}

}  // namespace ue_wrap::save_record
