// ue_wrap/actors/drive_bank.cpp -- see ue_wrap/actors/drive_bank.h.

#include "ue_wrap/actors/drive_bank.h"

#include "ue_wrap/actors/drive_bank_rows.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <chrono>
#include <cstring>
#include <unordered_map>

namespace ue_wrap::drive_bank {
namespace {

namespace R  = ue_wrap::reflection;
namespace SD = ue_wrap::signal_dynamic;
namespace PN = ue_wrap::profile::name;

// One class's verdict. A UClass is immortal once loaded, so the table is keyed by the pointer and Reset
// drops it. A failed class is kept too, so its refusal is measured and said once.
struct Bank {
    bool    ok = false;
    int32_t offDrives = -1;
    int32_t offIn = -1;
};
std::unordered_map<void*, Bank> g_banks;
void*    g_boxCls = nullptr;
uint64_t g_nextBoxTryMs = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Aprop_box_C, retried at most once a second while it is not loaded (a Blueprint class loads on demand).
void* BoxClass() {
    if (g_boxCls) return g_boxCls;
    const uint64_t now = NowMs();
    if (now < g_nextBoxTryMs) return nullptr;
    g_nextBoxTryMs = now + 1000;
    g_boxCls = R::FindClass(PN::DriveBoxClass);
    return g_boxCls;
}

const Bank* BankOf(void* actor) {
    if (!actor) return nullptr;
    void* cls = R::ClassOf(actor);
    if (!cls) return nullptr;
    const auto it = g_banks.find(cls);
    if (it != g_banks.end()) return it->second.ok ? &it->second : nullptr;
    void* box = BoxClass();
    if (!box) return nullptr;                            // not loaded: asked again, no verdict
    if (!R::IsDescendantOfAny(cls, &box, 1)) {           // not a drive box: final, and silent
        g_banks.emplace(cls, Bank{});
        return nullptr;
    }
    if (!SD::GetLayout()) return nullptr;                // the row struct is not measured yet: asked again
    Bank b;
    const char* why = nullptr;
    b.offDrives = R::FindPropertyOffset(cls, PN::DriveBoxDrivesProp);
    b.offIn = R::FindPropertyOffset(cls, PN::DriveBoxDrivesInProp);
    R::ArrayInnerInfo in;
    if (b.offDrives < 0 || b.offIn < 0) why = "has no drives / drives_in property";
    else if (!SD::ArrayHoldsRows(cls, PN::DriveBoxDrivesProp)) why = "drives is not an array of signal rows at the measured stride";
    else if (!R::ArrayPropertyInner(cls, PN::DriveBoxDrivesInProp, in) || in.innerType != L"BoolProperty" ||
             in.elementSize != 1)
        why = "drives_in is not an array of bool";
    if (why) {
        UE_LOGW("[DRIVE-BOX] %ls %s -- its signal rows are NOT carried and the box is left as the record "
                "leaves it (this line prints once per class)", R::ToString(R::NameOf(cls)).c_str(), why);
        g_banks.emplace(cls, Bank{});
        return nullptr;
    }
    b.ok = true;
    UE_LOGI("[DRIVE-BOX] %ls measured: drives@0x%X drives_in@0x%X rowStride=0x%X",
            R::ToString(R::NameOf(cls)).c_str(), b.offDrives, b.offIn, SD::Stride());
    return &(g_banks[cls] = b);
}

struct LiveArrays {
    const uint8_t* rows = nullptr;   // drives.Data
    const uint8_t* in = nullptr;     // drives_in.Data
    int32_t rowNum = 0;
    int32_t inNum = 0;
    int32_t stride = 0;
};

bool ReadArrays(void* actor, const Bank& b, LiveArrays& out) {
    out = LiveArrays{};
    out.stride = SD::Stride();
    if (out.stride <= 0) return false;
    const ue_wrap::save_record::Arr rows = ue_wrap::save_record::ReadArr(actor, b.offDrives);
    const ue_wrap::save_record::Arr in = ue_wrap::save_record::ReadArr(actor, b.offIn);
    out.rows = rows.data;
    out.rowNum = rows.num;
    out.in = in.data;
    out.inNum = in.num;
    return true;
}

std::vector<uint8_t> ReadOccupancy(const LiveArrays& a) {
    std::vector<uint8_t> occ(static_cast<size_t>(a.inNum), 0);
    for (int32_t i = 0; i < a.inNum; ++i) occ[static_cast<size_t>(i)] = a.in[i] ? 1 : 0;
    return occ;
}

// A string bounded for a log line: the leaf is the peer's to choose.
const wchar_t* Bounded(const std::wstring& s) { return s.c_str(); }

}  // namespace

bool IsBank(void* actor) { return BankOf(actor) != nullptr; }

bool ReadRows(void* actor, std::vector<Row>& out) {
    out.clear();
    const Bank* b = BankOf(actor);
    if (!b) return false;
    LiveArrays a;
    if (!ReadArrays(actor, *b, a) || a.rowNum < 0 || (a.rowNum > 0 && !a.rows)) return false;
    out.reserve(static_cast<size_t>(a.rowNum));
    for (int32_t i = 0; i < a.rowNum; ++i) {
        Row r;
        if (!SD::ReadStruct(a.rows + static_cast<size_t>(i) * a.stride, r)) return false;
        out.push_back(std::move(r));
    }
    return true;
}

bool Capture(void* actor, ue_wrap::save_record::SaveRecord& rec) {
    const Bank* b = BankOf(actor);
    if (!b) return false;
    LiveArrays a;
    if (!ReadArrays(actor, *b, a)) return false;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (a.rowNum != a.inNum || a.rowNum <= 0 || (a.rowNum > 0 && !a.rows) || (a.inNum > 0 && !a.in)) {
        UE_LOGW("[DRIVE-BOX] capture refused key='%.64ls': drives has %d slot(s) and drives_in %d -- the rows "
                "stay off this record", key.c_str(), a.rowNum, a.inNum);
        return false;
    }
    const std::vector<uint8_t> occ = ReadOccupancy(a);
    std::vector<Row> live;
    live.reserve(static_cast<size_t>(a.rowNum));
    for (int32_t i = 0; i < a.rowNum; ++i) {
        Row r;
        // An unoccupied slot is not read: a taken drive leaves its row behind, and Pack blanks it anyway.
        if (occ[static_cast<size_t>(i)] && !SD::ReadStruct(a.rows + static_cast<size_t>(i) * a.stride, r)) {
            UE_LOGW("[DRIVE-BOX] capture refused key='%.64ls': slot %d does not read as a signal row", key.c_str(), i);
            return false;
        }
        live.push_back(std::move(r));
    }
    std::vector<Row> packed;
    const char* why = nullptr;
    if (!Pack(live, occ, packed, why)) {
        UE_LOGW("[DRIVE-BOX] capture refused key='%.64ls': %s", key.c_str(), why);
        return false;
    }
    size_t rowsOut = 0;
    for (size_t i = 0; i < packed.size(); ++i) {
        if (!occ[i]) continue;
        ++rowsOut;
        UE_LOGI("[DRIVE-BOX]   slot=%zu id='%.48ls' level=%d quality=%u signal='%.64ls' size=%.2f", i,
                Bounded(packed[i].id), packed[i].level, static_cast<unsigned>(packed[i].quality),
                Bounded(packed[i].signal), packed[i].size);
    }
    rec.signals = std::move(packed);
    UE_LOGI("[DRIVE-BOX] captured %zu row(s) key='%.64ls' (slots=%zu)", rowsOut, key.c_str(), rec.signals.size());
    return true;
}

bool Apply(void* actor, const ue_wrap::save_record::SaveRecord& rec) {
    if (rec.signals.empty()) return false;
    const Bank* b = BankOf(actor);
    if (!b) return false;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    LiveArrays a;
    if (!ReadArrays(actor, *b, a) || a.rowNum <= 0 || !a.rows || a.inNum != a.rowNum || !a.in) {
        UE_LOGW("[DRIVE-BOX] apply refused key='%.64ls': the box's arrays do not read (drives=%d drives_in=%d)",
                key.c_str(), a.rowNum, a.inNum);
        return false;
    }
    Plan plan;
    if (const char* bad = MakePlan(rec.signals, ReadOccupancy(a), static_cast<size_t>(a.rowNum), plan)) {
        UE_LOGW("[DRIVE-BOX] apply REFUSED key='%.64ls': %s (record rows=%zu, box slots=%d) -- the box keeps "
                "the rows it has", key.c_str(), bad, rec.signals.size(), a.rowNum);
        return false;
    }
    // A slot is written only when it differs from what it holds: each write mints engine strings that
    // are never freed (signal_dynamic's pin doctrine), and a box republishes on every change.
    size_t changed = 0, failed = 0;
    for (size_t i = 0; i < plan.act.size(); ++i) {
        uint8_t* slot = const_cast<uint8_t*>(a.rows) + i * static_cast<size_t>(a.stride);
        Row cur;
        if (SD::ReadStruct(slot, cur) && cur == plan.rows[i]) continue;
        if (SD::WriteStructLive(slot, plan.rows[i])) ++changed;
        else ++failed;
    }
    if (failed)
        UE_LOGW("[DRIVE-BOX] apply key='%.64ls': %zu slot(s) could not be written (the engine string mint or a "
                "name leaf failed)", key.c_str(), failed);
    if (changed) {
        // The repaint reads `drives` for the light and the colour, so it runs after the rows are in. It is
        // our own call: the upd watch ignores it, so it does not publish back.
        if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), PN::DriveBoxUpdFn)) {
            ue_wrap::ParamFrame f(fn);
            if (f.valid()) ue_wrap::Call(actor, f);
        } else {
            UE_LOGW("[DRIVE-BOX] apply key='%.64ls': the box has no upd() -- the rows are in, the lights are stale",
                    key.c_str());
        }
    }
    UE_LOGI("[DRIVE-BOX] applied %zu row(s) key='%.64ls' (slots=%zu, changed=%zu%s)", WriteCount(plan),
            key.c_str(), plan.act.size(), changed, failed ? ", some failed" : "");
    return failed == 0;
}

void Reset() {
    g_banks.clear();
    g_boxCls = nullptr;
    g_nextBoxTryMs = 0;
}

}  // namespace ue_wrap::drive_bank
