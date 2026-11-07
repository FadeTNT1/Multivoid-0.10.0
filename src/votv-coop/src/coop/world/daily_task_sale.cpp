// coop/world/daily_task_sale.cpp -- see coop/world/daily_task_sale.h.

#include "coop/world/daily_task_sale.h"

#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/daily_task.h"
#include "ue_wrap/desk/task_grade.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace coop::daily_task_sale {
namespace {

namespace DT = ue_wrap::daily_task;
namespace TG = ue_wrap::task_grade;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

// Declared on the sale location's class: prop_coingun_C has a sell of its own and is not matched.
UE_FACT_FN(kRowSaleGrade, "daily_task_sale", L"droneSellLocation_C", L"sell", Declared);
UE_FACT_FN(kRowSaleHint, "daily_task_sale", L"lib_C", L"addHint", Declared);
constexpr int kTagSale = 0x54534731;  // 'TSG1'
constexpr int kTagHint = 0x54534831;  // 'TSH1'

std::atomic<coop::net::Session*> g_session{nullptr};

bool IsHost() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Host;
}

// The task as plain values, so a snapshot outlives the engine arrays it was read from.
struct TaskSnap {
    bool ok = false;
    bool active = false;
    std::vector<int32_t> req, done, dishes;
    float reelBig = 0.f, reelSmall = 0.f;
};

TaskSnap ReadTask() {
    TaskSnap t;
    DT::View v{};
    if (!DT::Read(v)) return t;
    t.ok = true;
    t.active = v.active;
    t.req.assign(v.sigRequired, v.sigRequired + v.sigRequiredNum);
    t.done.assign(v.sigCompleted, v.sigCompleted + v.sigCompletedNum);
    t.dishes.assign(v.requiredDishes, v.requiredDishes + v.requiredDishesNum);
    t.reelBig = v.reelBig;
    t.reelSmall = v.reelSmall;
    return t;
}

std::string Ints(const std::vector<int32_t>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += std::to_string(v[i]);
    }
    return s + "]";
}

// What the task still asks for, as "level 3 x3" pieces.
std::string Asks(const std::vector<int32_t>& req) {
    std::string s;
    for (size_t i = 0; i < req.size(); ++i) {
        if (req[i] <= 0) continue;
        if (!s.empty()) s += ", ";
        s += "level " + std::to_string(i) + " x" + std::to_string(req[i]);
    }
    return s.empty() ? std::string("no signals") : s;
}

int OpenDishes(const std::vector<int32_t>& dishes) {
    int n = 0;
    for (int32_t d : dishes) n += d >= 0 ? 1 : 0;
    return n;
}

// The body's state between its entry and its exit. A sale nests nothing of its own kind, so one is enough.
struct SaleCtx {
    bool open = false;
    void* fn = nullptr;
    TaskSnap before;
    std::vector<std::wstring> soldBefore;
    bool soldBeforeOk = false;
    int hints = 0;
};
SaleCtx g_sale;

sg::Verdict OnSalePre(const sg::Call& br) {
    g_sale = SaleCtx{};
    if (!IsHost()) return sg::Verdict::Run;
    g_sale.open = true;
    g_sale.fn = br.function;
    g_sale.before = ReadTask();
    g_sale.soldBeforeOk = TG::ReadSoldSignalIds(g_sale.soldBefore);
    UE_LOGI("[task] sale begins (host): task active=%d asks %s, satellites open %d, tapes big=%.1f small=%.1f",
            g_sale.before.active ? 1 : 0, Asks(g_sale.before.req).c_str(), OpenDishes(g_sale.before.dishes),
            g_sale.before.reelBig, g_sale.before.reelSmall);
    return sg::Verdict::Run;
}

// The grader's signal pass, replayed on the rows the scratch box holds after the body: a row of size > 0 whose id
// is not sold yet is sold, and it counts for the task when the task still asks for its level.
void ReportSignals(void* sellLocation, const TaskSnap& before, std::set<std::wstring>& sold) {
    void* box = TG::RefDriveBox(sellLocation);
    std::vector<ue_wrap::signal_dynamic::Row> rows;
    if (!box || !TG::ReadBoxRows(box, rows)) {
        UE_LOGI("[task]   signals: no drive box was read this sale");
        return;
    }
    std::vector<int32_t> req = before.req;
    int withData = 0, counted = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (!(r.size > 0.f)) continue;
        ++withData;
        const char* verdict;
        if (sold.count(r.id)) {
            verdict = "NOT counted: this signal was sold before";
        } else {
            sold.insert(r.id);
            if (!before.active) verdict = "sold; no daily task is active";
            else if (r.level < 0 || r.level >= static_cast<int32_t>(req.size())) verdict = "NOT counted: its level is outside the task";
            else if (req[static_cast<size_t>(r.level)] > 0) { --req[static_cast<size_t>(r.level)]; ++counted; verdict = "COUNTED"; }
            else verdict = "NOT counted: the task does not ask for this level";
        }
        UE_LOGI("[task]   signal slot %zu '%ls' (%ls) level %d size %.2f id '%.32ls' -- %s", i, r.name.c_str(),
                r.signal.c_str(), r.level, r.size, r.id.c_str(), verdict);
    }
    UE_LOGI("[task]   signals: %d boxed row(s) with data in the last box, %d counted; the task asked %s",
            withData, counted, Asks(before.req).c_str());
}

void ReportZip(void* sellLocation) {
    void* zip = TG::RefZipDisc(sellLocation);
    const std::vector<std::wstring> data = TG::ReadFloppyData(zip);
    if (!zip) {
        UE_LOGI("[task]   satellites: no zip floppy was read this sale");
        return;
    }
    std::wstring dishes;
    for (const std::wstring& row : data) {
        const std::wstring d = TG::ZipRowDish(row);
        if (d.empty()) continue;
        if (!dishes.empty()) dishes += L", ";
        dishes += d;
    }
    UE_LOGI("[task]   satellites: the last zip floppy holds %zu row(s), dishes: %ls", data.size(),
            dishes.empty() ? L"(none)" : dishes.c_str());
}

void OnSalePost(const sg::Call& br) {
    if (!g_sale.open || !IsHost()) { g_sale = SaleCtx{}; return; }
    const SaleCtx ctx = std::move(g_sale);
    g_sale = SaleCtx{};
    const TaskSnap after = ReadTask();
    std::vector<std::wstring> soldAfter;
    const bool soldAfterOk = TG::ReadSoldSignalIds(soldAfter);
    std::set<std::wstring> sold(ctx.soldBefore.begin(), ctx.soldBefore.end());
    ReportSignals(br.object, ctx.before, sold);
    ReportZip(br.object);
    const long long newSold = (ctx.soldBeforeOk && soldAfterOk)
        ? static_cast<long long>(soldAfter.size()) - static_cast<long long>(ctx.soldBefore.size()) : -1;
    if (!ctx.before.ok || !after.ok) {
        UE_LOGW("[task] sale graded (host): the task did not read (before=%d after=%d) -- no verdict",
                ctx.before.ok ? 1 : 0, after.ok ? 1 : 0);
        return;
    }
    UE_LOGI("[task] sale graded (host): new sold signal(s) %lld; sigRequired %s -> %s, sigCompleted %s -> %s, "
            "satellites open %d -> %d %s -> %s, tapes big %.1f -> %.1f small %.1f -> %.1f, active %d -> %d, hints %d",
            newSold, Ints(ctx.before.req).c_str(), Ints(after.req).c_str(), Ints(ctx.before.done).c_str(),
            Ints(after.done).c_str(), OpenDishes(ctx.before.dishes), OpenDishes(after.dishes),
            Ints(ctx.before.dishes).c_str(), Ints(after.dishes).c_str(), ctx.before.reelBig, after.reelBig,
            ctx.before.reelSmall, after.reelSmall, ctx.before.active ? 1 : 0, after.active ? 1 : 0, ctx.hints);
    if (ctx.before.active && !after.active)
        UE_LOGI("[task] sale graded (host): every part is done -- the task closed early and its reward was paid here");
}

// lib_C::addHint called from the sale's own body: the host shows it, and every client gets it too.
sg::Verdict OnHintPre(const sg::Call& call) {
    if (!g_sale.open || !g_sale.fn || call.callerFunction != g_sale.fn) return sg::Verdict::Run;
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return sg::Verdict::Run;
    std::wstring text;
    uint8_t type = 0;
    if (!TG::ReadHintCall(call, text, type) || text.empty()) {
        UE_LOGW("[task] sale hint did not read -- the clients do not get it");
        return sg::Verdict::Run;
    }
    coop::net::TaskSaleHintPayload p{};
    p.type = type;
    size_t n = text.size();
    if (n > coop::net::kTaskSaleHintCap) {
        UE_LOGW("[task] sale hint %zu chars > cap %u -- clamped", n, static_cast<unsigned>(coop::net::kTaskSaleHintCap));
        n = coop::net::kTaskSaleHintCap;
    }
    for (size_t i = 0; i < n; ++i) p.text[i] = static_cast<uint16_t>(text[i]);
    p.len = static_cast<uint8_t>(n);
    const bool sent = s->SendReliable(coop::net::ReliableKind::TaskSaleHint, &p, sizeof(p));
    ++g_sale.hints;
    UE_LOGI("[task] sale hint '%ls' (type %u) %s", text.c_str(), static_cast<unsigned>(type),
            sent ? "sent to the clients" : "NOT sent (refused)");
    return sg::Verdict::Run;
}

bool g_registered = false;
bool g_settled = false;

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    if (g_settled || !IsHost()) return;
    if (!g_registered) {
        g_registered = sg::WatchName(kRowSaleGrade, kTagSale, &OnSalePre, &OnSalePost) &&
                       sg::WatchName(kRowSaleHint, kTagHint, &OnHintPre, nullptr);
        if (!g_registered) {
            g_settled = true;
            UE_LOGE("[task] the sale watches were REFUSED by the gate -- no sale report, the sale's hints stay on the host");
        }
        return;
    }
    const bool liveSale = sg::NameWatchLive(kRowSaleGrade, kTagSale);
    const bool liveHint = sg::NameWatchLive(kRowSaleHint, kTagHint);
    if (liveSale && liveHint) {
        g_settled = true;
        UE_LOGI("[task] the sale watches are LIVE (droneSellLocation_C.sell report, lib_C.addHint relay)");
    } else if (sg::NameWatchSettled(kRowSaleGrade, kTagSale) && sg::NameWatchSettled(kRowSaleHint, kTagHint)) {
        g_settled = true;
        UE_LOGE("[task] a sale watch is DEAD (sell=%d addHint=%d) -- %s", liveSale ? 1 : 0, liveHint ? 1 : 0,
                liveSale ? "the sale's hints stay on the host" : "no sale report and no hint relay");
    }
}

void OnTaskSaleHint(const coop::net::TaskSaleHintPayload& p) {
    if (p.len > coop::net::kTaskSaleHintCap) {
        UE_LOGW("[task] TaskSaleHint length %u over cap -- dropped", static_cast<unsigned>(p.len));
        return;
    }
    std::wstring text;
    text.reserve(p.len);
    for (uint8_t i = 0; i < p.len; ++i) text.push_back(static_cast<wchar_t>(p.text[i]));
    const uint8_t type = p.type;
    GT::Post([text, type]() {
        coop::net::Session* s = g_session.load(std::memory_order_acquire);
        if (!s || !s->connected()) return;
        const bool shown = TG::ShowHint(text, type);
        UE_LOGI("[task] the host's sale hint '%ls' (type %u) %s", text.c_str(), static_cast<unsigned>(type),
                shown ? "shown" : "NOT shown (the hint library or the gamemode is not up)");
    });
}

}  // namespace coop::daily_task_sale
