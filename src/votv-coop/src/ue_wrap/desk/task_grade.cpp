// ue_wrap/desk/task_grade.cpp -- see ue_wrap/desk/task_grade.h.

#include "ue_wrap/desk/task_grade.h"

#include "ue_wrap/actors/drive_bank.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/world_singleton.h"

#include <cwctype>

namespace ue_wrap::task_grade {
namespace {

namespace R  = reflection;
namespace FT = ftext_utils;
namespace FI = field_io;
namespace PN = profile::name;

void* MemberObject(void* owner, R::InstanceOffset& off) {
    if (!owner || !R::IsLive(owner)) return nullptr;
    const int32_t o = off.Of(owner);
    if (o < 0) return nullptr;
    void* v = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(owner) + o);
    return v && R::IsLive(v) ? v : nullptr;
}

}  // namespace

bool ReadSoldSignalIds(std::vector<std::wstring>& out) {
    static R::InstanceOffset s_off{PN::SaveSlotSoldSignalsProp};
    out.clear();
    void* save = ue_wrap::economy::SaveSlotPtr();
    if (!save) return false;
    const int32_t off = s_off.Of(save);
    if (off < 0) return false;
    out = FI::ReadFStringArrayField(save, off);
    return true;
}

void* RefDriveBox(void* sellLocation) {
    static R::InstanceOffset s_off{PN::DroneSellRefDriveBoxProp};
    return MemberObject(sellLocation, s_off);
}

void* RefZipDisc(void* sellLocation) {
    static R::InstanceOffset s_off{PN::DroneSellRefZipDiscProp};
    return MemberObject(sellLocation, s_off);
}

bool ReadBoxRows(void* box, std::vector<signal_dynamic::Row>& out) {
    return ue_wrap::drive_bank::ReadRows(box, out);
}

std::vector<std::wstring> ReadFloppyData(void* floppy) {
    static R::InstanceOffset s_off{PN::FloppyDataProp};
    if (!floppy || !R::IsLive(floppy)) return {};
    const int32_t off = s_off.Of(floppy);
    if (off < 0) return {};
    return FI::ReadFStringArrayField(floppy, off);
}

std::wstring ZipRowDish(const std::wstring& row) {
    // ParseIntoArray(row, "??", cullEmpty=true), then [2], RightChop(3), ToLower -- the grader's steps.
    std::vector<std::wstring> parts;
    size_t at = 0;
    while (at <= row.size()) {
        const size_t next = row.find(L"??", at);
        const std::wstring piece = row.substr(at, next == std::wstring::npos ? std::wstring::npos : next - at);
        if (!piece.empty()) parts.push_back(piece);
        if (next == std::wstring::npos) break;
        at = next + 2;
    }
    if (parts.size() < 3) return {};
    std::wstring dish = parts[2].size() > 3 ? parts[2].substr(3) : std::wstring{};
    for (wchar_t& c : dish) c = static_cast<wchar_t>(std::towlower(c));
    return dish;
}

bool ReadHintCall(const ue_wrap::script_gate::Call& call, std::wstring& text, uint8_t& type) {
    static R::ParamOffset s_text{PN::LibAddHintTextParam};
    static R::ParamOffset s_type{PN::LibAddHintTypeParam};
    if (!call.function || !call.locals) return false;
    const int32_t offText = s_text.Of(call.function);
    const int32_t offType = s_type.Of(call.function);
    if (offText < 0) return false;
    text = FT::FTextToString(call.locals + offText);
    type = offType >= 0 ? call.locals[offType] : 0;
    return true;
}

bool ShowHint(const std::wstring& text, uint8_t type) {
    void* libCls = R::FindClass(PN::LibClass);
    void* cdo = R::FindClassDefaultObject(PN::LibClass);
    void* fn = libCls ? R::FindFunction(libCls, PN::LibAddHintFn) : nullptr;
    void* gm = world_singleton::Gamemode();
    if (!cdo || !fn || !gm || !R::IsLive(gm)) return false;
    uint8_t ftext[FT::kFTextSize] = {};
    if (!FT::MintFText(text.c_str(), ftext)) return false;
    ParamFrame frame(fn);
    if (!frame.valid()) return false;
    if (!frame.SetRaw(PN::LibAddHintTextParam, ftext, FT::kFTextSize)) return false;
    frame.Set<uint8_t>(PN::LibAddHintTypeParam, type);
    frame.Set<bool>(PN::LibAddHintDebugParam, false);
    frame.Set<void*>(PN::LibWorldContextParam, gm);
    return Call(cdo, frame);
}

}  // namespace ue_wrap::task_grade
