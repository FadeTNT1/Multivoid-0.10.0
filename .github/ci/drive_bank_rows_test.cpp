// The drive box's row sidecar (ue_wrap/actors/drive_bank_rows.h) round-tripped through the production
// record wire (save_record_wire + signal_wire), with a fake box standing in for the engine arrays.
//
// The bug this holds: a box's `drives` rows rode no record, so a peer that received the box had
// drives_in set and empty slots, and the sale grader (which counts boxed rows) saw no signals.
#include "ue_wrap/actors/drive_bank_rows.h"

#include "coop/items/save_record_wire.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace DB = ue_wrap::drive_bank;
using ue_wrap::save_record::SaveRecord;
using Row = ue_wrap::signal_dynamic::Row;

namespace {

int g_checks = 0, g_fail = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("FAIL: %s\n", what); }
}

// The box as the engine holds it: eight slots of `drives` and `drives_in`.
struct FakeBox {
    std::vector<Row> drives;
    std::vector<uint8_t> in;
    explicit FakeBox(size_t slots = 8) : drives(slots), in(slots, 0) {}
};

Row MakeRow(const wchar_t* id, const wchar_t* signal, int32_t level, uint8_t quality, float size = 12.5f) {
    Row r;
    r.name = std::wstring(L"Drive ") + id;
    r.id = id;
    r.signal = signal;
    r.object = L"obj_" + std::wstring(signal);
    r.level = level;
    r.quality = quality;
    r.frequency = 2;
    r.objectType = 1;
    r.size = size;
    r.decoded = size;
    r.downloadedAtQuality = 0.25f + static_cast<float>(quality);
    r.locX = 1.5f * static_cast<float>(level);
    r.locY = -3.0f;
    r.date = 637000000000000000LL + level;
    r.polarity = level % 2;
    r.hasData = size > 0;
    return r;
}

void Put(FakeBox& b, size_t slot, const Row& r) { b.drives[slot] = r; b.in[slot] = 1; }

// capture -> record -> bytes -> record: what crosses between the peers.
bool Ship(const FakeBox& sender, SaveRecord& landed) {
    std::vector<Row> packed;
    const char* why = nullptr;
    if (!DB::Pack(sender.drives, sender.in, packed, why)) return false;
    SaveRecord sent;
    sent.className = L"prop_box_C";
    sent.key = L"box_1";
    sent.signals = std::move(packed);
    std::vector<uint8_t> bytes;
    coop::save_record_wire::SerSave(bytes, sent);
    size_t o = 0;
    landed = SaveRecord{};
    return coop::save_record_wire::DeSave(bytes, o, landed) && o == bytes.size();
}

// The receiver after loadData: its drives_in is the sender's (the game's own bools), its rows untouched.
const char* Land(FakeBox& receiver, const FakeBox& sender, const SaveRecord& rec) {
    receiver.in = sender.in;
    DB::Plan plan;
    const char* bad = DB::MakePlan(rec.signals, receiver.in, receiver.drives.size(), plan);
    if (bad) return bad;
    for (size_t i = 0; i < plan.act.size(); ++i) receiver.drives[i] = plan.rows[i];
    return nullptr;
}

bool Same(const Row& a, const Row& b) {
    return a.id == b.id && a.name == b.name && a.signal == b.signal && a.object == b.object &&
           a.level == b.level && a.quality == b.quality && a.frequency == b.frequency &&
           a.objectType == b.objectType && a.size == b.size && a.decoded == b.decoded &&
           a.downloadedAtQuality == b.downloadedAtQuality && a.locX == b.locX && a.locY == b.locY &&
           a.date == b.date && a.polarity == b.polarity && a.isCopy == b.isCopy;
}

bool Blank(const Row& r) { return r.id.empty() && r.signal.empty() && r.size == 0.0f && r.level == 0; }

}  // namespace

int main() {
    // 1. Occupied and empty slots, distinct ids / templates / levels / qualities, survive the whole trip.
    {
        FakeBox a, b;
        Put(a, 0, MakeRow(L"AAAA0001", L"sig_pulsar", 0, 1));
        Put(a, 2, MakeRow(L"BBBB0002", L"sig_quasar", 1, 2));
        Put(a, 5, MakeRow(L"CCCC0003", L"sig_nebula", 2, 3));
        Put(a, 7, MakeRow(L"DDDD0004", L"sig_comet", 3, 0));
        SaveRecord rec;
        Check(Ship(a, rec), "a box with four drives packs, serialises and parses");
        Check(rec.signals.size() == 8, "one row per slot crosses, the empty ones blank");
        const char* bad = Land(b, a, rec);
        Check(bad == nullptr, "the receiving box accepts the rows");
        bool all = true;
        for (size_t i : {0u, 2u, 5u, 7u}) all = all && Same(a.drives[i], b.drives[i]);
        Check(all, "every occupied slot lands with its id, template, level, quality, date and size");
        Check(b.drives[2].level == 1 && b.drives[5].level == 2 && b.drives[7].level == 3 && b.drives[0].level == 0,
              "each row keeps its own level (the grader counts per level)");
        Check(b.drives[0].id != b.drives[2].id && b.drives[5].signal != b.drives[7].signal,
              "distinct ids and templates stay distinct");
        Check(Blank(b.drives[1]) && Blank(b.drives[3]) && Blank(b.drives[4]) && Blank(b.drives[6]),
              "the empty slots stay empty");
        Check(b.in == a.in, "occupancy is the sender's");
    }

    // 2. A taken drive leaves its row behind in the sender; it must not travel, and a stale row on the
    //    receiver in an unoccupied slot is cleared.
    {
        FakeBox a, b;
        Put(a, 3, MakeRow(L"EEEE0005", L"sig_old", 2, 1));
        a.in[3] = 0;                                      // taken out; the row stays in the array
        Put(a, 4, MakeRow(L"FFFF0006", L"sig_kept", 1, 2));
        Put(b, 3, MakeRow(L"STALE", L"sig_stale", 3, 3)); // the receiver still holds an old drive here
        b.in[3] = 0;
        SaveRecord rec;
        Check(Ship(a, rec), "a box with a stale row packs");
        Check(Blank(rec.signals[3]), "an unoccupied slot ships blank whatever the array still holds");
        Check(Land(b, a, rec) == nullptr, "the record lands");
        Check(Blank(b.drives[3]), "a stale row in an unoccupied slot is cleared");
        Check(Same(a.drives[4], b.drives[4]), "the occupied slot lands");
    }

    // 3. An emptied box is a sidecar of blanks, not an absent one: the receiver's old rows are cleared.
    {
        FakeBox a, b;
        Put(b, 1, MakeRow(L"GGGG0007", L"sig_gone", 1, 1));
        SaveRecord rec;
        Check(Ship(a, rec) && rec.signals.size() == 8, "an empty box still ships its eight blank rows");
        Check(Land(b, a, rec) == nullptr, "it lands");
        Check(Blank(b.drives[1]) && b.in[1] == 0, "the receiver's drive is gone (occupancy and row)");
    }

    // 4. A record with no sidecar (a peer that sends none) leaves the slots alone.
    {
        FakeBox b;
        Put(b, 6, MakeRow(L"HHHH0008", L"sig_mine", 2, 2));
        DB::Plan plan;
        Check(DB::MakePlan({}, b.in, b.drives.size(), plan) == nullptr && plan.act.empty(),
              "an empty sidecar is no plan and no refusal");
    }

    // 5. Duplicate ids both survive: the sender can hold two copies and nothing here merges them.
    {
        FakeBox a, b;
        Put(a, 0, MakeRow(L"SAME0009", L"sig_dup", 1, 1));
        Put(a, 1, MakeRow(L"SAME0009", L"sig_dup", 1, 1));
        SaveRecord rec;
        Check(Ship(a, rec) && Land(b, a, rec) == nullptr, "a box holding two rows of one id lands");
        Check(Same(a.drives[0], b.drives[0]) && Same(a.drives[1], b.drives[1]), "both rows are kept");
    }

    // 6. A photo never rides (the box does not need it, and the record is capped).
    {
        FakeBox a;
        Row r = MakeRow(L"IIII0010", L"sig_img", 1, 1);
        r.image = {1, 2, 3, 4};
        Put(a, 0, r);
        std::vector<Row> packed;
        const char* why = nullptr;
        Check(DB::Pack(a.drives, a.in, packed, why) && packed[0].image.empty(), "the photo is left out of the pack");
    }

    // 7. The apply refuses a bank it does not trust, whole, and the box keeps what it had.
    {
        FakeBox a;
        Put(a, 0, MakeRow(L"JJJJ0011", L"sig_ok", 1, 1));
        SaveRecord good;
        Check(Ship(a, good), "baseline record");
        auto refuses = [&](SaveRecord rec, const char* what) {
            FakeBox b;
            Put(b, 4, MakeRow(L"KEEP0012", L"sig_keep", 2, 2));
            b.in = a.in;                                  // loadData's occupancy
            const Row before = b.drives[4];
            const char* bad = Land(b, a, rec);
            Check(bad != nullptr && Same(before, b.drives[4]), what);
        };
        SaveRecord r = good;
        r.signals.pop_back();
        refuses(r, "a row count that is not the slot count is refused");
        r = good; r.signals[0].level = 4;
        refuses(r, "a level past 3 is refused");
        r = good; r.signals[0].level = -1;
        refuses(r, "a negative level is refused");
        r = good; r.signals[0].size = std::numeric_limits<float>::quiet_NaN();
        refuses(r, "a NaN size is refused");
        r = good; r.signals[0].downloadedAtQuality = std::numeric_limits<float>::infinity();
        refuses(r, "an infinite quality is refused");
        r = good; r.signals[0].size = 0.0f;
        refuses(r, "an occupied slot with no data is refused");
        r = good; r.signals[0].signal = L"None";
        refuses(r, "a None template leaf is refused");
        r = good; r.signals[0].object = L"nOnE";
        refuses(r, "a None object leaf is refused in any case");
        r = good; r.signals.resize(DB::kMaxSlots + 1);
        refuses(r, "a row count past the ceiling is refused");

        DB::Plan plan;
        Check(DB::MakePlan(good.signals, std::vector<uint8_t>(7, 0), 8, plan) != nullptr,
              "an occupancy of the wrong length is refused");
        Check(DB::MakePlan(good.signals, {}, 0, plan) != nullptr, "a box with no slots is refused");
        Check(DB::MakePlan(std::vector<Row>(DB::kMaxSlots + 1), std::vector<uint8_t>(DB::kMaxSlots + 1, 0),
                           DB::kMaxSlots + 1, plan) != nullptr, "a box past the slot ceiling is refused");
    }

    // 8. The receiver's occupancy is the authority: a row the sender sent for a slot its drives_in leaves
    //    empty is cleared, never written.
    {
        FakeBox a, b;
        Put(a, 1, MakeRow(L"LLLL0013", L"sig_x", 1, 1));
        SaveRecord rec;
        Check(Ship(a, rec), "record");
        FakeBox other;                                    // a loadData that left slot 1 unoccupied
        DB::Plan plan;
        Check(DB::MakePlan(rec.signals, other.in, 8, plan) == nullptr && plan.act[1] == DB::Act::Clear &&
              DB::WriteCount(plan) == 0, "a row for a slot the box did not flag occupied is not written");
    }

    // 9. Pack's own refusals.
    {
        std::vector<Row> out;
        const char* why = nullptr;
        Check(!DB::Pack({}, {}, out, why) && why, "no slots packs nothing");
        Check(!DB::Pack(std::vector<Row>(8), std::vector<uint8_t>(7, 0), out, why) && out.empty(),
              "drives and drives_in of different lengths pack nothing");
        Check(!DB::Pack(std::vector<Row>(DB::kMaxSlots + 1), std::vector<uint8_t>(DB::kMaxSlots + 1, 0), out, why),
              "an implausible slot count packs nothing");
    }

    // 10. A truncated record is refused by the wire before it reaches the apply.
    {
        FakeBox a;
        Put(a, 0, MakeRow(L"MMMM0014", L"sig_cut", 1, 1));
        std::vector<Row> packed;
        const char* why = nullptr;
        DB::Pack(a.drives, a.in, packed, why);
        SaveRecord sent;
        sent.signals = packed;
        std::vector<uint8_t> bytes;
        coop::save_record_wire::SerSave(bytes, sent);
        bytes.resize(bytes.size() - 5);
        size_t o = 0;
        SaveRecord landed;
        Check(!coop::save_record_wire::DeSave(bytes, o, landed), "a record cut mid-row does not parse");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
