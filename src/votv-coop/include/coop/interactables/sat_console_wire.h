// coop/interactables/sat_console_wire.h -- the SAT console lane's blobs, encoded and decoded with their bounds.
//
// ReliableKind::SatConsole carries a BlobChunkPayload blob whose first byte is the op. Every decoder checks
// the op, bounds each string by its field's own limit BEFORE it allocates, refuses a NUL inside a string (it
// would truncate the engine's FString), checks a count against the bytes that remain, and requires the blob
// to be consumed exactly. A decoder that returns false leaves its output unspecified.
//
// Directions: Line, Input and Present go client -> host; Busy, Screen and InputRelay go host -> client.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::sat_console_wire {

enum Op : uint8_t {
    kOpLine       = 0,  // client -> host: a line to run on the host's terminal for its typist
    kOpRetired    = 1,  // was the host's printed line to its typist; the screen's log replaced it
    kOpBusy       = 2,  // host -> typist: the level of its terminal's busy flag
    kOpScreen     = 3,  // host -> client: the whole shared screen
    kOpInput      = 4,  // client -> host: the holder's live input line
    kOpInputRelay = 5,  // host -> client: the holder's live input line, for those who watch
    kOpPresent    = 6,  // client -> host: what a line the typist ran itself printed
};

constexpr int32_t  kNoDish        = -1;     // init with no dish: the ROOT console
constexpr int32_t  kMaxDishIndex  = 1023;   // far past any map's dishes; a wild index is refused, not looked up
constexpr uint8_t  kNoHolder      = 0xFF;
constexpr size_t   kMaxInputChars = 255;    // MTA's clamp too; every command name is short
constexpr size_t   kMaxNameChars  = 64;
constexpr size_t   kMaxHistory    = 3000;   // fixLog keeps the rightmost 3000 characters
constexpr size_t   kMaxPrintChars = 8192;   // `debug rules` prints the game rules as JSON, a few thousand
constexpr uint8_t  kMaxBrackets   = 2;      // writeToLog's bracketsType: 0 output, 1 echo, 2 bare

struct Line {
    int32_t dish = kNoDish;
    std::wstring name, used, line;
};

// The host's whole screen: what a peer shows, and who is typing into it. `ackInput` and `ackPresent` are the
// recipient's own last input and presentation sequences the host took, so a typist's terminal can tell its
// own unmerged output from a stale canonical screen.
struct Screen {
    uint32_t rev = 0;
    uint32_t gen = 0;            // the ownership generation: moves with the sat claim's holder
    uint8_t  holder = kNoHolder;
    uint32_t ackInput = 0;
    uint32_t ackPresent = 0;
    uint8_t  busy = 0;
    int32_t  dish = kNoDish;
    std::wstring name, input, log;
};

struct Input {
    uint32_t stamp = 0;          // the sender's count of its own sat claims; a newer claim outranks every older packet
    uint32_t seq = 0;            // within a stamp, a packet that is not newer than the last taken is stale
    int32_t  dish = kNoDish;
    std::wstring name, text;
};

struct InputRelay {
    uint8_t  holder = kNoHolder;
    uint32_t gen = 0;
    uint32_t seq = 0;
    std::wstring text;
};

enum class PresentKind : uint8_t { Write = 0, Clear = 1 };

struct Present {
    uint32_t seq = 0;
    PresentKind kind = PresentKind::Write;
    uint8_t  type = 0;
    std::wstring text;
};

void EncodeLine(std::vector<uint8_t>& out, const Line& v);
void EncodeBusy(std::vector<uint8_t>& out, bool busy);
void EncodeScreen(std::vector<uint8_t>& out, const Screen& v);
void EncodeInput(std::vector<uint8_t>& out, const Input& v);
void EncodeInputRelay(std::vector<uint8_t>& out, const InputRelay& v);
void EncodePresent(std::vector<uint8_t>& out, const Present& v);

// The op byte of a blob, or false for an empty one.
bool PeekOp(const std::vector<uint8_t>& blob, uint8_t& op);

bool DecodeLine(const std::vector<uint8_t>& blob, Line& out);
bool DecodeBusy(const std::vector<uint8_t>& blob, bool& busy);
bool DecodeScreen(const std::vector<uint8_t>& blob, Screen& out);
bool DecodeInput(const std::vector<uint8_t>& blob, Input& out);
bool DecodeInputRelay(const std::vector<uint8_t>& blob, InputRelay& out);
bool DecodePresent(const std::vector<uint8_t>& blob, Present& out);

// The input line as the host takes it from a peer: the engine's text box holds one line, so a control
// character is dropped rather than kept.
std::wstring SanitizeInput(const std::wstring& s);

}  // namespace coop::sat_console_wire
